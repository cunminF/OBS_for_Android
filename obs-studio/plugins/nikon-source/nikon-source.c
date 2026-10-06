// Nikon Webcam 采集源（Z 系列，PTP/MTP Live View 拉流）。
//
// 原理：Nikon Z 系列机身在 MTP/PTP 模式下提供 remote live view —— StartLiveView 之后
// 用 GetLiveViewImageEx 逐帧拉 JPEG（上限 XGA 1024x768），本插件自己实现 PTP 事务状态机
// （cmd → data → resp，Bulk-Only Transport），拉到 JPEG 后用 turbojpeg 解成 BGRA，
// 走 obs_source_output_video 推出（异步视频源）。
//
// fd 从哪来：与 android-capture(uvc-input.c) 同一条路 —— Android 上应用没有 /dev/bus/usb
// 的访问权，USB 设备只能由 Java 侧 UsbManager 打开，fd 经 libobs 的 Android USB 总线
// （obs-android.h）传进来，再由 libusb_wrap_sys_device() 包住。fd 归 Java 所有，
// 这里绝不 close(fd)，只用 obs_android_usb_close() 归还。
//
// 协议依据：Nikon《Z 8 USB MTP 协议规格书》Rev.2.00（提取简报 z8-lv-protocol-brief.md，
// 字节级偏移以它为准）：OpenSession → 查 0xD1A4(LiveViewProhibitionCondition)==0 →
// 设 0xD1AC(尺寸)/0xD1BC(质量) → GetLiveViewCompressedSize 分配缓冲 → StartLiveView →
// DeviceReady 轮询 → 循环 GetLiveViewImageEx。退出 EndLiveView → CloseSession
// （规格书明确：忘发 EndLiveView 直接 CloseSession 也安全）。
//
// 线程模型：采集循环在插件自己的 pthread 上（activate 起、deactivate/destroy 拆，
// shutting_down 原子量 + join，与 camera2-input.c 的拆除协议同款）。libusb 句柄、
// PTP 状态、帧缓冲全部只活在这条线程里，主结构里只留状态文本和帧计数给属性页读。

#include <libusb-1.0/libusb.h>
#include <turbojpeg.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

/* obs.h 里的包含是 #ifdef __ANDROID__ 的，头本身与平台无关（同 uvc-input.c 的取舍） */
#include <obs-android.h>

/* PTP 操作码 / 属性码 / 响应码（规格书 §6.2/6.3/6.5.11） */
#define PTP_OP_GetDeviceInfo 0x1001
#define PTP_OP_OpenSession 0x1002
#define PTP_OP_CloseSession 0x1003
#define PTP_OP_GetDevicePropValue 0x1015
#define PTP_OP_SetDevicePropValue 0x1016
#define PTP_OP_DeviceReady 0x90C8
#define PTP_OP_StartLiveView 0x9201
#define PTP_OP_EndLiveView 0x9202
#define PTP_OP_GetLiveViewCompressedSize 0x9423
#define PTP_OP_GetLiveViewImageEx 0x9428

#define PTP_PROP_LV_PROHIBITION 0xD1A4 /* 必须 == 0 才允许 remote LV */
#define PTP_PROP_LV_IMAGE_SIZE 0xD1AC  /* 1=QVGA 2=VGA 3=XGA */
#define PTP_PROP_LV_COMPRESSION 0xD1BC /* 0~5 */

#define PTP_RESP_OK 0x2001
#define PTP_RESP_INCOMPLETE_TRANSFER 0x2007
#define PTP_RESP_ACCESS_DENIED 0x200F
#define PTP_RESP_DEVICE_BUSY 0x2019
#define PTP_RESP_SESSION_ALREADY_OPEN 0x201E
#define PTP_RESP_NOT_LIVEVIEW 0xA00B

#define PTP_EVENT_LV_STATE_CHANGED 0xC10C /* Param1: 0=OFF 1=ON */

/* Nikon VID；PID 不写死（不同固件/速度模式会变），匹配靠 VID + 接口类 06/01/01 */
#define NIKON_VID 0x04B0

/* LiveViewObject 数据集（简报 §4，规格书 §9.7.1）：
 * 偏移 8 = 显示信息区大小（恒 1024），偏移 12 = JPEG 字节数，JPEG 本体在偏移 1024 起。 */
#define LVO_HEADER_SIZE 1024
#define LVO_OFF_DISP_SIZE 8
#define LVO_OFF_JPEG_SIZE 12

/* 端点回退值（简报 §0）：描述符读不到时用这组 */
#define FALLBACK_EP_OUT 0x02
#define FALLBACK_EP_IN 0x81
#define FALLBACK_EP_INT 0x83

/* 超时：命令/响应 2 s 足够（同一条 USB3 总线上的控制往返）；
 * 数据阶段放宽到 5 s —— XGA JPEG 一百多 KB，再慢也不该碰到它。 */
#define PTP_CMD_TIMEOUT_MS 2000
#define PTP_DATA_TIMEOUT_MS 5000

/* DeviceReady 轮询：简报建议 50-100 ms；StartLiveView 整机切换给 15 s 余量 */
#define DEVICE_READY_POLL_MS 75
#define DEVICE_READY_TIMEOUT_MS 15000

/* 连续 USB 错误超过这个数就放弃本次会话，走整体拆除 + 指数退避重开 */
#define MAX_CONSEC_USB_ERR 3
#define RETRY_BACKOFF_MIN_MS 1000ULL
#define RETRY_BACKOFF_MAX_MS 8000ULL

/* 协议层错误的内部哨兵（libusb 错误码都是小的负数，错不开） */
#define NK_ERR_PROTOCOL (-1000)
#define NK_ERR_NOT_OK (-1001) /* 事务走完了但响应码不是 OK，resp 里带着真码 */

struct nikon_source {
	obs_source_t *source;

	/* 设置（settings 快照；usb_device 是 UsbDevice.getDeviceName()，形如 /dev/bus/usb/001/002） */
	char usb_device[OBS_ANDROID_USB_NAME_LEN];
	int lv_size;    /* 1/2/3 */
	int lv_quality; /* 0~5 */

	/* 采集线程与拆除协议（照 camera2-input.c：shutting_down 置 1 后 join） */
	pthread_t thread;
	volatile long thread_started;
	volatile long shutting_down;

	/* 状态文本：采集线程写、UI 线程读，mtx 只护它（读到半条文案最多难看一帧） */
	pthread_mutex_t mtx;
	char status[256];

	/* 只写给人看的计数：采集线程写、属性页读，不做任何判定依据 */
	uint64_t frames_out;
	uint64_t frames_dropped;
	uint32_t cur_w;
	uint32_t cur_h;
};

/* 一次 PTP 会话的全部运行期状态，只活在采集线程栈上 */
struct nikon_cam {
	libusb_context *ctx;
	libusb_device_handle *h;
	int iface;
	int config_value; /* SET_CONFIGURATION 硬恢复用 */
	uint8_t ep_out;
	uint8_t ep_in;
	uint8_t ep_int;
	int bridge_fd; /* 从总线借来的 fd，归还用 obs_android_usb_close */
	uint32_t tid;  /* 事务号：OpenSession 用 0，之后从 1 递增 */
};

/* ---------------- 小端读写（PTP 容器全部 LSB first，规格书 §5） ---------------- */

static void put_u16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint16_t get_u16le(const uint8_t *p)
{
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* LiveViewObject **数据集内部**的字段是大端（真机 Z8 实测：版本字节 00 02，尺寸 00 00 04 00=1024）。
 * 注意这与 PTP 容器头的小端是两套序，简报里"全部小端"只适用于容器层。 */
static uint32_t get_u32be(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void set_status(struct nikon_source *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	pthread_mutex_lock(&s->mtx);
	vsnprintf(s->status, sizeof(s->status), fmt, args);
	pthread_mutex_unlock(&s->mtx);
	va_end(args);
}

/* 可中断的睡眠：退避期间也要对 deactivate/destroy 及时响应 */
static void sleep_check(struct nikon_source *s, uint32_t ms)
{
	while (ms-- && !os_atomic_load_long(&s->shutting_down))
		os_sleep_ms(1);
}

/* ---------------- USB 错误恢复（简报 §8：Cancel → ClearFeature → DeviceReset） ---------------- */

/* with_cancel=false 用于"刚 claim 完接口、还不知道对面挂着哪个事务"的开局清扫：
 * 上一轮（尤其 App 被 force-stop 的那次）可能把相机丢在远程 LV + 数据阶段半途，
 * bulk 管道是 stall 的，不先清一遍 OpenSession 都会超时（真机实测 -7）。 */
static void usb_recover(struct nikon_cam *c, bool with_cancel)
{
	/* 1) 先放空 Bulk-In：进程被杀时往往是一条 GetLiveViewImageEx 命令已发出、
	 *    相机备好的几十~几百 KB 数据还端在手里（数据阶段半途）。这种状态下相机
	 *    不回任何新命令（OpenSession 都超时），DeviceReset 也不一定理 —— 真机实测。
	 *    只有把残余数据读走、让数据阶段收尾，相机才回到命令阶段。 */
	uint8_t *scratch = bmalloc(16384);
	uint32_t drained = 0;
	for (int i = 0; i < 64; i++) {
		int xfer = 0;
		const int r = libusb_bulk_transfer(c->h, c->ep_in, scratch, 16384, &xfer, 250);
		drained += (uint32_t) xfer;
		if (r == LIBUSB_ERROR_TIMEOUT && xfer == 0)
			break;
		if (r != 0 && r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_OVERFLOW)
			break;
	}
	bfree(scratch);
	if (drained)
		blog(LOG_WARNING, "nikon: 从 Bulk-In 放掉了上一轮残留的 %u 字节", (unsigned) drained);

	/* 2) 清两个 bulk 端点的 halt（§3.1 确认支持 ClearFeature(ENDPOINT_HALT)），
	 *    再按需发类请求 Cancel(0x64) 取消当前事务、DeviceReset(0x66) 让设备回 idle。 */
	libusb_clear_halt(c->h, c->ep_in);
	libusb_clear_halt(c->h, c->ep_out);

	if (with_cancel) {
		uint8_t cancel[6];
		put_u16le(cancel, 0x4001); /* Cancellation Code */
		put_u32le(cancel + 2, c->tid);
		libusb_control_transfer(c->h, 0x21, 0x64, 0, 0, cancel, sizeof(cancel), 1000);
	}

	libusb_control_transfer(c->h, 0x21, 0x66, 0, 0, NULL, 0, 1000);

	blog(LOG_WARNING, "nikon: USB 错误恢复已执行（放空 Bulk-In + ClearFeature halt%s + DeviceReset）",
	     with_cancel ? " + Cancel" : "");
}

/* ---------------- PTP 事务状态机（cmd → data → resp，规格书 §5） ---------------- */

static int ptp_send_cmd(struct nikon_cam *c, uint16_t code, int nparams, uint32_t p1, uint32_t p2, uint32_t p3)
{
	uint8_t buf[24];
	const uint32_t len = 12 + 4 * (uint32_t) nparams;
	int xfer = 0;

	put_u32le(buf, len);
	put_u16le(buf + 4, 1); /* Container Type = Command */
	put_u16le(buf + 6, code);
	put_u32le(buf + 8, c->tid);
	put_u32le(buf + 12, p1);
	put_u32le(buf + 16, p2);
	put_u32le(buf + 20, p3);

	const int r = libusb_bulk_transfer(c->h, c->ep_out, buf, (int) len, &xfer, PTP_CMD_TIMEOUT_MS);
	if (r != 0)
		return r;
	return xfer == (int) len ? 0 : LIBUSB_ERROR_IO;
}

/* 从 Bulk-In 收一个完整容器。缓冲区必须容得下整个容器（数据阶段的缓冲按
 * GetLiveViewCompressedSize 分配，命令/响应用 64 字节栈缓冲，都碰不到截断）。
 * 相机可能把容器拆在多个 URB 里，所以按"头里的 Container Length"攒够为止。 */
static int ptp_recv_container(struct nikon_cam *c, uint8_t *buf, uint32_t cap, uint32_t *out_len,
			      unsigned timeout_ms)
{
	uint32_t got = 0;

	while (got < 12) {
		int xfer = 0;
		const int r = libusb_bulk_transfer(c->h, c->ep_in, buf + got, (int) (cap - got), &xfer, timeout_ms);
		got += (uint32_t) xfer;
		if (r != 0 && r != LIBUSB_ERROR_OVERFLOW)
			return r;
		if (xfer == 0)
			break;
	}
	if (got < 12)
		return LIBUSB_ERROR_IO;

	const uint32_t total = get_u32le(buf);
	if (total < 12 || total > cap)
		return NK_ERR_PROTOCOL;

	while (got < total) {
		int xfer = 0;
		const int r = libusb_bulk_transfer(c->h, c->ep_in, buf + got, (int) (total - got), &xfer, timeout_ms);
		got += (uint32_t) xfer;
		if (r != 0 && r != LIBUSB_ERROR_OVERFLOW)
			return r;
		if (xfer == 0)
			break;
	}
	if (got < total)
		return LIBUSB_ERROR_IO;

	*out_len = got;
	return 0;
}

/* 读响应块并校验 Type/TransactionID；响应参数（可选，4 字节）放 rparam */
static int ptp_read_resp(struct nikon_cam *c, uint16_t *resp, uint32_t *rparam)
{
	uint8_t buf[64];
	uint32_t len = 0;

	const int r = ptp_recv_container(c, buf, sizeof(buf), &len, PTP_CMD_TIMEOUT_MS);
	if (r != 0)
		return r;
	if (get_u16le(buf + 4) != 3 || get_u32le(buf + 8) != c->tid)
		return NK_ERR_PROTOCOL;
	*resp = get_u16le(buf + 6);
	if (rparam)
		*rparam = len >= 16 ? get_u32le(buf + 12) : 0;
	return 0;
}

/* 无数据阶段的事务：cmd → resp。响应码非 OK 时返回 NK_ERR_NOT_OK，真码在 *resp。 */
static int ptp_simple(struct nikon_cam *c, uint16_t code, int nparams, uint32_t p1, uint16_t *resp, uint32_t *rparam)
{
	c->tid++;
	int r = ptp_send_cmd(c, code, nparams, p1, 0, 0);
	if (r != 0)
		return r;
	r = ptp_read_resp(c, resp, rparam);
	if (r != 0)
		return r;
	return *resp == PTP_RESP_OK ? 0 : NK_ERR_NOT_OK;
}

/* 主机→相机数据阶段（SetDevicePropValue 用）：cmd → data → resp。
 * payload 只用于属性原始值（≤4 字节），栈缓冲放得下整个容器。 */
static int ptp_data_out(struct nikon_cam *c, uint16_t code, uint32_t p1, const uint8_t *payload, uint32_t plen,
			uint16_t *resp)
{
	uint8_t buf[64];
	int xfer = 0;

	if (plen + 12 > sizeof(buf))
		return NK_ERR_PROTOCOL;

	c->tid++;
	int r = ptp_send_cmd(c, code, 1, p1, 0, 0);
	if (r != 0)
		return r;

	put_u32le(buf, 12 + plen);
	put_u16le(buf + 4, 2); /* Data */
	put_u16le(buf + 6, code);
	put_u32le(buf + 8, c->tid);
	memcpy(buf + 12, payload, plen);

	r = libusb_bulk_transfer(c->h, c->ep_out, buf, (int) (12 + plen), &xfer, PTP_CMD_TIMEOUT_MS);
	if (r != 0)
		return r;
	if (xfer != (int) (12 + plen))
		return LIBUSB_ERROR_IO;

	r = ptp_read_resp(c, resp, NULL);
	if (r != 0)
		return r;
	return *resp == PTP_RESP_OK ? 0 : NK_ERR_NOT_OK;
}

/* 相机→主机数据阶段：cmd → data → resp。
 * 命令被拒时相机跳过数据阶段直接回错误响应块（Bulk-Only 标准行为），这里把这种情况
 * 也归一成 NK_ERR_NOT_OK + *resp，调用方不用分两条路。 */
static int ptp_data_in(struct nikon_cam *c, uint16_t code, int nparams, uint32_t p1, uint8_t *buf, uint32_t cap,
		       uint32_t *payload_len, uint16_t *resp, unsigned data_timeout_ms)
{
	uint32_t len = 0;

	c->tid++;
	int r = ptp_send_cmd(c, code, nparams, p1, 0, 0);
	if (r != 0)
		return r;

	r = ptp_recv_container(c, buf, cap, &len, data_timeout_ms);
	if (r != 0)
		return r;

	if (get_u16le(buf + 4) == 3) {
		/* 没有数据阶段，这就是响应块 */
		*resp = get_u16le(buf + 6);
		return *resp == PTP_RESP_OK ? NK_ERR_PROTOCOL : NK_ERR_NOT_OK;
	}
	if (get_u16le(buf + 4) != 2 || get_u16le(buf + 6) != code || get_u32le(buf + 8) != c->tid)
		return NK_ERR_PROTOCOL;

	*payload_len = get_u32le(buf) - 12;

	r = ptp_read_resp(c, resp, NULL);
	if (r != 0)
		return r;
	return *resp == PTP_RESP_OK ? 0 : NK_ERR_NOT_OK;
}

/* ---------------- 设备枚举 / 端点 ---------------- */

/* 按 VID + 接口类 06/01/01 找接口和三个端点；描述符读不到就退回简报 §0 的固定值 */
static void find_endpoints(struct nikon_cam *c)
{
	struct libusb_config_descriptor *cfg = NULL;
	libusb_device *dev = libusb_get_device(c->h);
	bool found = false;

	c->iface = 0;
	c->ep_out = FALLBACK_EP_OUT;
	c->ep_in = FALLBACK_EP_IN;
	c->ep_int = FALLBACK_EP_INT;

	int r = libusb_get_active_config_descriptor(dev, &cfg);
	if (r != 0)
		r = libusb_get_config_descriptor(dev, 0, &cfg);
	if (r != 0 || !cfg) {
		blog(LOG_WARNING, "nikon: 读不到配置描述符（%d），端点用回退值 0x%02X/0x%02X/0x%02X", r,
		     c->ep_out, c->ep_in, c->ep_int);
		return;
	}

	c->config_value = cfg->bConfigurationValue;

	for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];
		for (int a = 0; a < itf->num_altsetting && !found; a++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
			if (alt->bInterfaceClass != 0x06 || alt->bInterfaceSubClass != 0x01 ||
			    alt->bInterfaceProtocol != 0x01)
				continue;
			c->iface = alt->bInterfaceNumber;
			for (int e = 0; e < alt->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
				const uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
				if (type == LIBUSB_TRANSFER_TYPE_BULK &&
				    (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
					c->ep_out = ep->bEndpointAddress;
				else if (type == LIBUSB_TRANSFER_TYPE_BULK)
					c->ep_in = ep->bEndpointAddress;
				else if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
					c->ep_int = ep->bEndpointAddress;
			}
			found = true;
		}
	}
	libusb_free_config_descriptor(cfg);

	blog(LOG_INFO, "nikon: %s，接口 %d，端点 out=0x%02X in=0x%02X int=0x%02X",
	     found ? "描述符匹配到 PTP 成像接口" : "描述符里没有 06/01/01 接口，用回退端点", c->iface, c->ep_out,
	     c->ep_in, c->ep_int);
}

/* 读中断端点并丢弃；返回 true 表示收到 LiveViewStateChanged(OFF)。
 * 超时必须给小的正值：libusb 的 timeout=0 语义是"无限等"而不是"非阻塞"，
 * 真机实测传 0 会把采集线程永远堵在这里（无事件时中断端点本来就静默）。 */
static bool drain_events(struct nikon_cam *c)
{
	uint8_t buf[64];
	int xfer = 0;
	bool lv_off = false;

	for (int i = 0; i < 4; i++) {
		const int r = libusb_bulk_transfer(c->h, c->ep_int, buf, sizeof(buf), &xfer, 5);
		if (r != 0 || xfer < 12)
			break;
		if (get_u16le(buf + 4) == 4) { /* Event */
			const uint16_t code = get_u16le(buf + 6);
			const uint32_t param = xfer >= 16 ? get_u32le(buf + 12) : 0;
			if (code == PTP_EVENT_LV_STATE_CHANGED) {
				blog(LOG_WARNING, "nikon: 事件 LiveViewStateChanged → %s", param ? "ON" : "OFF");
				if (param == 0)
					lv_off = true;
			} else {
				blog(LOG_INFO, "nikon: 丢弃事件 0x%04X param=%u", (unsigned) code, (unsigned) param);
			}
		}
	}
	return lv_off;
}

/* ---------------- Live View 会话 ---------------- */

/* StartLiveView + DeviceReady 轮询直到非 Busy（简报 §2：OK 只表示"开始切换"）。
 * 返回 0 = 已进入 remote live view。 */
static int start_liveview(struct nikon_source *s, struct nikon_cam *c)
{
	uint16_t resp = 0;

	int r = ptp_simple(c, PTP_OP_StartLiveView, 0, 0, &resp, NULL);
	if (r != 0) {
		blog(LOG_WARNING, "nikon: StartLiveView 失败 r=%d resp=0x%04X", r, (unsigned) resp);
		return r;
	}

	const uint64_t deadline = os_gettime_ns() + (uint64_t) DEVICE_READY_TIMEOUT_MS * 1000000ULL;
	while (!os_atomic_load_long(&s->shutting_down)) {
		r = ptp_simple(c, PTP_OP_DeviceReady, 0, 0, &resp, NULL);
		if (r == 0)
			return 0;
		if (r == NK_ERR_NOT_OK && resp == PTP_RESP_DEVICE_BUSY) {
			if (os_gettime_ns() >= deadline) {
				blog(LOG_WARNING, "nikon: DeviceReady 轮询 %d ms 仍是 Busy，放弃",
				     DEVICE_READY_TIMEOUT_MS);
				return NK_ERR_NOT_OK;
			}
			sleep_check(s, DEVICE_READY_POLL_MS);
			continue;
		}
		blog(LOG_WARNING, "nikon: DeviceReady 失败 r=%d resp=0x%04X", r, (unsigned) resp);
		return r;
	}
	return LIBUSB_ERROR_INTERRUPTED;
}

/* 完整跑一轮：拿 fd → libusb → 会话 → LiveView → 取帧循环，直到出错或关停。
 * 所有运行期资源在返回前全部拆干净（fd 归还总线），可安全重进。
 * fail_count 是跨轮传递的连续失败计数，>=2 时本轮启用 SET_CONFIGURATION 硬恢复。
 * 返回 true = 本轮有进展（进过 Live View），false = 一无所获。 */
static bool nikon_run_once(struct nikon_source *s, bool *perm_requested, int fail_count)
{
	struct nikon_cam c = {0};
	struct obs_android_usb_device devs[8];
	uint8_t *rx = NULL;
	uint8_t *bgra = NULL;
	size_t bgra_size = 0;
	tjhandle tj = NULL;
	bool lv_started = false;
	bool session_open = false;
	int usb_errs = 0;

	/* ---- 1. 在总线清单里找选中的设备 ---- */
	if (!s->usb_device[0]) {
		set_status(s, "未选择设备（在属性页里选一台 Nikon 相机）");
		return false;
	}
	const int ndev = obs_android_usb_enum_devices(devs, 8);
	if (ndev < 0) {
		set_status(s, "USB 总线未接通（宿主 App 没注册 obs_android_set_usb_host）");
		return false;
	}
	bool found = false;
	bool has_perm = false;
	for (int i = 0; i < ndev; i++) {
		if (strcmp(devs[i].name, s->usb_device) == 0) {
			found = true;
			has_perm = devs[i].has_permission;
			break;
		}
	}
	if (!found) {
		set_status(s, "设备 '%s' 不在 USB 清单里（已拔出？）", s->usb_device);
		return false;
	}
	if (!has_perm) {
		set_status(s, "设备 '%s' 未授权", s->usb_device);
		/* 授权弹窗一轮只发起一次，免得退避循环每转一圈弹一个框 */
		if (!*perm_requested) {
			const int pr = obs_android_usb_request_permission(s->usb_device);
			blog(LOG_INFO, "nikon: 设备未授权，已发起授权请求（返回 %d：%s）", pr,
			     pr == 1 ? "弹窗中" : pr == 0 ? "其实已有权限" : "失败");
			*perm_requested = true;
		}
		return false;
	}
	*perm_requested = false;

	/* ---- 2. 借 fd，包 libusb ---- */
	c.bridge_fd = obs_android_usb_open(s->usb_device);
	if (c.bridge_fd <= 0) {
		set_status(s, "打开设备失败（fd=%d）—— 设备被拔或授权刚被收回", c.bridge_fd);
		return false;
	}
	set_status(s, "正在初始化 USB/PTP 会话…");

	/* 必须关设备发现：fd 是 Java 借来的、枚举本来就用不上，而 libusb 默认初始化要起
	 * NETLINK_KOBJECT_UEVENT 热插拔监听 —— Android 的 SELinux 不让应用建这种 netlink
	 * socket，libusb_init 直接失败（真机实测 -1/LIBUSB_ERROR_IO）。
	 * NO_DEVICE_DISCOVERY 跳过热插拔与 /dev/bus/usb 扫描，wrap_sys_device 不受影响。 */
	const struct libusb_init_option opts[] = {{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}}};
	int r = libusb_init_context(&c.ctx, opts, 1);
	if (r != 0) {
		blog(LOG_ERROR, "nikon: libusb_init_context 失败 %d", r);
		set_status(s, "libusb 初始化失败 %d", r);
		goto out;
	}
	r = libusb_wrap_sys_device(c.ctx, (intptr_t) c.bridge_fd, &c.h);
	if (r != 0 || !c.h) {
		blog(LOG_ERROR, "nikon: libusb_wrap_sys_device(fd=%d) 失败 %d", c.bridge_fd, r);
		set_status(s, "包装 USB fd 失败（%d）", r);
		c.h = NULL;
		goto out;
	}
	find_endpoints(&c);

	/* 探一下控制端点活不活：GetDeviceStatus（PIMA 类请求，0xA1/0x67；简报说文档
	 * 写的 0x66 疑似笔误，两个都试）。bulk 管道全挂时 EP0 往往还能答 —— 能答
	 * 就说明固件没死透，值得继续用 SET_CONFIGURATION 捶。 */
	{
		uint8_t st[4] = {0};
		int sr = libusb_control_transfer(c.h, 0xA1, 0x67, 0, 0, st, sizeof(st), 1000);
		if (sr < 0)
			sr = libusb_control_transfer(c.h, 0xA1, 0x66, 0, 0, st, sizeof(st), 1000);
		if (sr == 4)
			blog(LOG_INFO, "nikon: GetDeviceStatus 探针：code=0x%04X（0x2001=OK 0x2019=Busy）",
			     (unsigned) get_u16le(st + 2));
		else
			blog(LOG_WARNING, "nikon: GetDeviceStatus 探针失败 r=%d（控制端点也不答，"
					  "相机固件可能已挂，只能断电/拔线重启相机）", sr);
	}

	/* 连续失败 >=2 轮后的硬恢复，逐级加重：
	 *  a) SET_CONFIGURATION：强制设备整个功能复位（端点/data toggle/接口状态重来）。
	 *     真机实测返回 -6(BUSY)，Android usbfs 不放这条路，留着只为日志作证。
	 *  b) libusb_reset_device：底层是 USBDEVFS_RESET，给设备所在端口发真正的 USB 复位，
	 *     设备重新枚举 —— 这是无 root 应用能摸到的最重的一锤。成功的话旧 fd 立即作废，
	 *     设备会以新节点回到总线清单（Java 侧热插拔广播负责收尾），本轮直接按失败退避，
	 *     下一轮要能在清单里找到"新"设备才走得下去（节点名可能变，比如 001/002 → 001/003）。 */
	if (fail_count >= 2 && c.config_value > 0) {
		const int cr = libusb_set_configuration(c.h, c.config_value);
		blog(LOG_WARNING, "nikon: 连续失败 %d 轮，试 SET_CONFIGURATION(%d) 硬恢复：r=%d", fail_count,
		     c.config_value, cr);
	}
	if (fail_count >= 3) {
		const int rr = libusb_reset_device(c.h);
		blog(LOG_WARNING, "nikon: 连续失败 %d 轮，试 USBDEVFS_RESET 端口复位：r=%d%s", fail_count, rr,
		     rr == 0 ? "（设备将重新枚举，节点名可能变）" : "");
	}

	r = libusb_claim_interface(c.h, c.iface);
	if (r != 0) {
		blog(LOG_ERROR, "nikon: claim interface %d 失败 %d（设备被别的源占着？）", c.iface, r);
		set_status(s, "占用 USB 接口失败（%d）—— 别的源（如 UVC 摄像头）可能正占着这台相机", r);
		goto out;
	}

	/* 开局清扫：上一轮可能把相机丢在数据阶段半途（App 被 force-stop 时拆除代码
	 * 根本没机会跑），不先把管道清回 idle 的话 OpenSession 会超时。 */
	c.tid = 0;
	usb_recover(&c, false);

	tj = tjInitDecompress();
	if (!tj) {
		blog(LOG_ERROR, "nikon: tjInitDecompress 失败：%s", tjGetErrorStr());
		set_status(s, "JPEG 解码器初始化失败");
		goto out;
	}

	/* ---- 3. PTP 会话 ---- */
	uint16_t resp = 0;
	uint32_t rparam = 0;
	/* OpenSession 的事务号必须是 0、之后从 1 递增（规格书 §5.1）。ptp_* 帮手进事务时
	 * 先 ++ 再发，所以这里预置 0xFFFFFFFF，回绕后发出的第一条恰是 0。 */
	c.tid = 0xFFFFFFFF;
	r = ptp_simple(&c, PTP_OP_OpenSession, 1, 1, &resp, &rparam);
	if (r == NK_ERR_NOT_OK && resp == PTP_RESP_SESSION_ALREADY_OPEN) {
		blog(LOG_INFO, "nikon: 会话已开着（上次没关干净），接着用");
	} else if (r != 0) {
		blog(LOG_ERROR, "nikon: OpenSession 失败 r=%d resp=0x%04X", r, (unsigned) resp);
		set_status(s, "OpenSession 失败（resp=0x%04X）", (unsigned) resp);
		goto out;
	}
	session_open = true;

	/* LiveViewProhibitionCondition(0xD1A4) 必须 == 0：电量耗尽/过热/拍摄处理中都会置位 */
	uint8_t propbuf[64];
	uint32_t plen = 0;
	r = ptp_data_in(&c, PTP_OP_GetDevicePropValue, 1, PTP_PROP_LV_PROHIBITION, propbuf, sizeof(propbuf), &plen,
			&resp, PTP_CMD_TIMEOUT_MS);
	if (r != 0) {
		blog(LOG_ERROR, "nikon: GetDevicePropValue(0xD1A4) 失败 r=%d resp=0x%04X", r, (unsigned) resp);
		set_status(s, "读 LiveView 许可条件失败（resp=0x%04X）", (unsigned) resp);
		goto out;
	}
	const uint32_t prohibit = plen >= 4 ? get_u32le(propbuf + 12) : 0xFFFFFFFF;
	if (prohibit != 0) {
		blog(LOG_WARNING, "nikon: LiveViewProhibitionCondition=0x%08X（bit8 电量耗尽 / bit15 拍摄处理中 / "
				  "bit17 温度升高 / bit28 镜头错误），进不了 Live View",
		     (unsigned) prohibit);
		set_status(s, "相机禁止 Live View（条件位 0x%08X：查电量/温度/镜头/是否正在拍摄）", (unsigned) prohibit);
		goto out;
	}

	/* 设 LiveView 尺寸与质量（StartLiveView 之前设） */
	uint8_t v8 = (uint8_t) s->lv_size;
	r = ptp_data_out(&c, PTP_OP_SetDevicePropValue, PTP_PROP_LV_IMAGE_SIZE, &v8, 1, &resp);
	if (r != 0) {
		blog(LOG_WARNING, "nikon: 设 LiveViewImageSize=%d 失败 r=%d resp=0x%04X", s->lv_size, r,
		     (unsigned) resp);
		set_status(s, "设置 LiveView 分辨率失败（resp=0x%04X）", (unsigned) resp);
		goto out;
	}
	v8 = (uint8_t) s->lv_quality;
	r = ptp_data_out(&c, PTP_OP_SetDevicePropValue, PTP_PROP_LV_COMPRESSION, &v8, 1, &resp);
	if (r != 0) {
		blog(LOG_WARNING, "nikon: 设 LiveViewImageCompression=%d 失败 r=%d resp=0x%04X", s->lv_quality, r,
		     (unsigned) resp);
		set_status(s, "设置 JPEG 质量失败（resp=0x%04X）", (unsigned) resp);
		goto out;
	}

	/* 按 LiveViewObjectMaxSize 分配接收缓冲（改完上面两个属性必须重查，简报 §5） */
	uint32_t max_size = 0;
	{
		r = ptp_data_in(&c, PTP_OP_GetLiveViewCompressedSize, 0, 0, propbuf, sizeof(propbuf), &plen, &resp,
				PTP_CMD_TIMEOUT_MS);
		if (r != 0) {
			blog(LOG_ERROR, "nikon: GetLiveViewCompressedSize 失败 r=%d resp=0x%04X", r, (unsigned) resp);
			set_status(s, "查询 LiveView 缓冲上限失败（resp=0x%04X）", (unsigned) resp);
			goto out;
		}
		max_size = plen >= 4 ? get_u32le(propbuf + 12) : 0;
	}
	if (max_size < LVO_HEADER_SIZE + 4096 || max_size > 64U * 1024U * 1024U) {
		blog(LOG_ERROR, "nikon: LiveViewObjectMaxSize=%u 不合理", (unsigned) max_size);
		set_status(s, "相机报告的缓冲上限不合理（%u）", (unsigned) max_size);
		goto out;
	}
	rx = bmalloc(max_size);
	blog(LOG_INFO, "nikon: 会话已建立，LiveViewObject 上限 %u 字节", (unsigned) max_size);

	/* ---- 4. 进 Live View ---- */
	set_status(s, "正在启动 Live View…");
	r = start_liveview(s, &c);
	if (r != 0) {
		set_status(s, "StartLiveView 失败");
		goto out;
	}
	lv_started = true;
	blog(LOG_INFO, "nikon: 已进入 remote live view（尺寸档 %d，质量档 %d）", s->lv_size, s->lv_quality);

	/* ---- 5. 取帧循环 ---- */
	uint32_t last_len = 0;
	uint64_t last_hash = 0;
	uint32_t poll_count = 0;
	bool need_restart_lv = false;

	while (!os_atomic_load_long(&s->shutting_down)) {
		/* 中断端点定期读丢弃（每次最多堵 5 ms，所以隔 30 次轮询才读一回）；
		 * LiveViewStateChanged(OFF) = 相机自己关了 LV */
		if (++poll_count % 30 == 0 && drain_events(&c))
			need_restart_lv = true;

		if (need_restart_lv) {
			blog(LOG_INFO, "nikon: 重新 StartLiveView");
			r = start_liveview(s, &c);
			if (r != 0) {
				if (++usb_errs > MAX_CONSEC_USB_ERR)
					break;
				usb_recover(&c, true);
			} else {
				usb_errs = 0;
				need_restart_lv = false;
			}
			continue;
		}

		uint32_t frame_payload = 0;
		r = ptp_data_in(&c, PTP_OP_GetLiveViewImageEx, 0, 0, rx, max_size, &frame_payload, &resp,
				PTP_DATA_TIMEOUT_MS);
		if (r == NK_ERR_NOT_OK) {
			if (resp == PTP_RESP_NOT_LIVEVIEW) {
				/* 不在 remote LV 了（相机侧退了 LV）—— 重走 StartLiveView */
				need_restart_lv = true;
				continue;
			}
			if (resp == PTP_RESP_INCOMPLETE_TRANSFER || resp == PTP_RESP_ACCESS_DENIED) {
				blog(LOG_INFO, "nikon: GetLiveViewImageEx resp=0x%04X，重试", (unsigned) resp);
				sleep_check(s, 50);
				continue;
			}
			blog(LOG_WARNING, "nikon: GetLiveViewImageEx 未处理的 resp=0x%04X，放弃本次会话",
			     (unsigned) resp);
			break;
		}
		if (r != 0) {
			blog(LOG_WARNING, "nikon: GetLiveViewImageEx USB/协议错误 %d（第 %d 次）", r, usb_errs + 1);
			usb_recover(&c, true);
			if (++usb_errs > MAX_CONSEC_USB_ERR) {
				blog(LOG_ERROR, "nikon: 连续 USB 错误超过 %d 次，拆除本次会话", MAX_CONSEC_USB_ERR);
				break;
			}
			continue;
		}
		usb_errs = 0;

		/* LiveViewObject 解析：rx 还带着 12 字节 PTP 容器头，数据集本体从 rx+12 起。
		 * 数据集内部字段是大端（实测，见 get_u32be 那段）：偏移 0-1 是版本 00 02，
		 * 偏移 8 = 显示信息区大小（1024），偏移 12 = JPEG 字节数，JPEG 在偏移 1024 起。 */
		const uint8_t *obj = rx + 12;
		if (frame_payload < LVO_HEADER_SIZE + 2 || obj[0] != 0x00 || obj[1] != 0x02) {
			s->frames_dropped++;
			if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
				blog(LOG_WARNING, "nikon: LiveViewObject 头不对（payload=%u，版本=%02X %02X），丢帧",
				     (unsigned) frame_payload, obj[0], obj[1]);
			continue;
		}
		const uint32_t jpeg_len = get_u32be(obj + LVO_OFF_JPEG_SIZE);
		if (jpeg_len == 0 || LVO_HEADER_SIZE + jpeg_len > frame_payload) {
			s->frames_dropped++;
			blog(LOG_WARNING, "nikon: JPEG 长度 %u 越界（payload=%u），丢帧", (unsigned) jpeg_len,
			     (unsigned) frame_payload);
			continue;
		}
		const uint8_t *jpeg = obj + LVO_HEADER_SIZE;

		/* 去重：数据集没有帧序号，轮询快了就拿到同一帧（长度 + 内容哈希都不变就跳过） */
		uint64_t hash = 1469598103934665603ULL; /* FNV-1a */
		for (uint32_t i = 0; i < jpeg_len; i++)
			hash = (hash ^ jpeg[i]) * 1099511628211ULL;
		if (jpeg_len == last_len && hash == last_hash) {
			sleep_check(s, 10); /* 拿到重复帧说明比相机出帧快，缓一拍 */
			continue;
		}

		/* JPEG → BGRA */
		int jw = 0, jh = 0, jsub = 0, jcs = 0;
		if (tjDecompressHeader3(tj, jpeg, jpeg_len, &jw, &jh, &jsub, &jcs) != 0 || jw <= 0 || jh <= 0) {
			s->frames_dropped++;
			blog(LOG_WARNING, "nikon: JPEG 头解析失败：%s", tjGetErrorStr());
			continue;
		}
		const size_t need = (size_t) jw * (size_t) jh * 4;
		if (need > bgra_size) {
			bfree(bgra);
			bgra = bmalloc(need);
			bgra_size = need;
		}
		if (tjDecompress2(tj, jpeg, jpeg_len, bgra, jw, jw * 4, jh, TJPF_BGRA, 0) != 0) {
			s->frames_dropped++;
			blog(LOG_WARNING, "nikon: JPEG 解码失败：%s", tjGetErrorStr());
			continue;
		}

		struct obs_source_frame out = {0};
		out.width = (uint32_t) jw;
		out.height = (uint32_t) jh;
		out.format = VIDEO_FORMAT_BGRA;
		out.data[0] = bgra;
		out.linesize[0] = (uint32_t) jw * 4;
		out.full_range = true;
		out.timestamp = os_gettime_ns();
		/* 与 uvc-input.c 同一处坑：YUV 着色器无条件读 color_matrix / range 上下限，
		 * 不填就是纯黑。BGRA 虽然不走 YUV 转换，照仓内先例（v4l2-input.c:128）填上无害。 */
		video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_FULL, VIDEO_FORMAT_BGRA,
						       out.color_matrix, out.color_range_min, out.color_range_max);
		obs_source_output_video(s->source, &out);

		last_len = jpeg_len;
		last_hash = hash;
		s->frames_out++;
		s->cur_w = (uint32_t) jw;
		s->cur_h = (uint32_t) jh;

		if (s->frames_out == 1 || s->frames_out % 300 == 0)
			blog(LOG_INFO, "nikon: 第 %llu 帧 %dx%d（JPEG %u 字节，累计丢弃 %llu）",
			     (unsigned long long) s->frames_out, jw, jh, (unsigned) jpeg_len,
			     (unsigned long long) s->frames_dropped);
		if (s->frames_out == 1 || s->frames_out % 150 == 0)
			set_status(s, "采集中：%dx%d，已送出 %llu 帧", jw, jh, (unsigned long long) s->frames_out);
	}

out:
	/* 退出顺序：EndLiveView → CloseSession（规格书：只发 CloseSession 也安全，
	 * 所以 EndLiveView 失败不阻断后面的拆除） */
	if (lv_started) {
		uint16_t eresp = 0;
		const int er = ptp_simple(&c, PTP_OP_EndLiveView, 0, 0, &eresp, NULL);
		if (er != 0)
			blog(LOG_INFO, "nikon: EndLiveView r=%d resp=0x%04X（CloseSession 仍会兜底）", er,
			     (unsigned) eresp);
	}
	if (session_open) {
		uint16_t cresp = 0;
		ptp_simple(&c, PTP_OP_CloseSession, 0, 0, &cresp, NULL);
	}
	if (tj)
		tjDestroy(tj);
	if (c.h) {
		libusb_release_interface(c.h, c.iface);
		libusb_close(c.h);
	}
	if (c.ctx)
		libusb_exit(c.ctx);
	if (c.bridge_fd > 0)
		obs_android_usb_close(c.bridge_fd); /* fd 归 Java，只还不关 */
	if (rx)
		bfree(rx);
	if (bgra)
		bfree(bgra);

	blog(LOG_INFO, "nikon: 本轮采集结束（送出 %llu 帧，丢弃 %llu 帧）", (unsigned long long) s->frames_out,
	     (unsigned long long) s->frames_dropped);
	return lv_started;
}

static void *nikon_thread_main(void *arg)
{
	struct nikon_source *s = arg;
	uint64_t retry_ms = 0;
	bool perm_requested = false;
	int fail_count = 0;

	blog(LOG_INFO, "nikon: 采集线程启动（设备 '%s'，尺寸档 %d，质量档 %d）", s->usb_device, s->lv_size,
	     s->lv_quality);

	while (!os_atomic_load_long(&s->shutting_down)) {
		const bool progress = nikon_run_once(s, &perm_requested, fail_count);
		if (os_atomic_load_long(&s->shutting_down))
			break;
		fail_count = progress ? 0 : fail_count + 1;
		/* 指数退避：相机是独占设备，连续敲门抢不过别人还刷爆日志（camera2 同款账） */
		retry_ms = retry_ms ? (retry_ms * 2 > RETRY_BACKOFF_MAX_MS ? RETRY_BACKOFF_MAX_MS : retry_ms * 2)
				    : RETRY_BACKOFF_MIN_MS;
		char prev[sizeof(s->status)];
		pthread_mutex_lock(&s->mtx);
		snprintf(prev, sizeof(prev), "%s", s->status);
		pthread_mutex_unlock(&s->mtx);
		blog(LOG_INFO, "nikon: %llu ms 后重试（上次状态：%s）", (unsigned long long) retry_ms, prev);
		sleep_check(s, (uint32_t) retry_ms);
	}

	blog(LOG_INFO, "nikon: 采集线程退出");
	return NULL;
}

/* ---------------- 线程启停 ---------------- */

static void nikon_start_thread(struct nikon_source *s)
{
	if (os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 0);
	if (pthread_create(&s->thread, NULL, nikon_thread_main, s) != 0) {
		blog(LOG_ERROR, "nikon: 采集线程创建失败");
		return;
	}
	os_atomic_set_long(&s->thread_started, 1);
}

static void nikon_stop_thread(struct nikon_source *s)
{
	if (!os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 1);
	/* join 最坏要堵一个 PTP 超时（命令 2 s / 数据 5 s）。这条调用落在 deactivate/destroy
	 * 上（UI 或视频线程），与 camera2-input.c 里"拆除停渲染循环 133 ms"是同一类取舍：
	 * 触发点都是人手点的（删源/切场景/关程序），不为此再引一条任务线程。 */
	pthread_join(s->thread, NULL);
	os_atomic_set_long(&s->thread_started, 0);
}

/* ---------------- 源接口 ---------------- */

static const char *nikon_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Nikon Webcam（Z 系列）";
}

static void nikon_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "usb_device", "");
	obs_data_set_default_int(settings, "lv_size", 3);   /* XGA 1024x768 */
	obs_data_set_default_int(settings, "lv_quality", 3);
}

static void nikon_settings_apply(struct nikon_source *s, obs_data_t *settings)
{
	const char *dev = obs_data_get_string(settings, "usb_device");
	snprintf(s->usb_device, sizeof(s->usb_device), "%s", dev ? dev : "");

	const long long size = obs_data_get_int(settings, "lv_size");
	s->lv_size = (size >= 1 && size <= 3) ? (int) size : 3;

	const long long q = obs_data_get_int(settings, "lv_quality");
	s->lv_quality = (q >= 0 && q <= 5) ? (int) q : 3;
}

static void *nikon_create(obs_data_t *settings, obs_source_t *source)
{
	struct nikon_source *s = bzalloc(sizeof(*s));
	s->source = source;
	/* 与 camera2-input.c 同：PTHREAD_MUTEX_INITIALIZER 过不了 windows pthread 移植层 */
	pthread_mutex_init_value(&s->mtx);
	if (pthread_mutex_init(&s->mtx, NULL) != 0)
		blog(LOG_ERROR, "nikon: 建锁失败，状态文本没有互斥了");
	nikon_settings_apply(s, settings);
	return s;
}

static void nikon_destroy(void *data)
{
	struct nikon_source *s = data;
	if (!s)
		return;
	nikon_stop_thread(s);
	pthread_mutex_destroy(&s->mtx);
	bfree(s);
}

static void nikon_activate(void *data)
{
	nikon_start_thread(data);
}

static void nikon_deactivate(void *data)
{
	nikon_stop_thread(data);
}

static void nikon_update(void *data, obs_data_t *settings)
{
	struct nikon_source *s = data;
	const char *dev = obs_data_get_string(settings, "usb_device");
	const long long size = obs_data_get_int(settings, "lv_size");
	const long long q = obs_data_get_int(settings, "lv_quality");

	/* 与 uvc-input.c 同款判法：只拿"设置项 vs 设置项"比，没变化就不重启 */
	const bool changed = strcmp(dev ? dev : "", s->usb_device) != 0 || (int) size != s->lv_size ||
			     (int) q != s->lv_quality;
	if (!changed)
		return;

	nikon_stop_thread(s);
	nikon_settings_apply(s, settings);
	if (obs_source_active(s->source))
		nikon_start_thread(s);
}

/* 属性页【申请授权】：权限结果是异步的，返回 false 不刷属性页（同 camera2 的取舍） */
static bool on_request_permission(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct nikon_source *s = data;

	if (!s || !s->usb_device[0]) {
		blog(LOG_INFO, "nikon: 属性页点【申请授权】但还没选设备，先选设备再点");
		return false;
	}
	const int r = obs_android_usb_request_permission(s->usb_device);
	blog(LOG_INFO, "nikon: 属性页对 '%s' 点【申请授权】，总线返回 %d（%s）", s->usb_device, r,
	     r == 1 ? "弹窗中，用户选完后重新激活本源生效"
	     : r == 0 ? "其实已有权限"
		      : "失败（拿不到 Activity 或被永久拒绝）");
	return false;
}

static obs_properties_t *nikon_properties(void *data)
{
	struct nikon_source *s = data;
	obs_properties_t *props = obs_properties_create();

	/* 设备清单是打开属性页这一刻的快照（同 uvc-input.c：插拔后要重开对话框） */
	obs_property_t *dev_list = obs_properties_add_list(props, "usb_device", "Nikon 相机（USB）",
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	if (n < 0) {
		obs_property_list_add_string(dev_list, "（USB 总线未接通：宿主 App 没注册 obs_android_set_usb_host）",
					     "");
	} else if (n == 0) {
		obs_property_list_add_string(dev_list, "（当前没有 USB 设备，插上相机（MTP 模式）后重新打开本属性页）",
					     "");
	} else {
		for (int i = 0; i < n; i++) {
			char text[OBS_ANDROID_USB_LABEL_LEN + OBS_ANDROID_USB_NAME_LEN + 16];
			snprintf(text, sizeof(text), "%s [%s]%s", devs[i].label, devs[i].name,
				 devs[i].has_permission ? "" : "（未授权）");
			obs_property_list_add_string(dev_list, text, devs[i].name);
		}
		obs_property_list_add_string(dev_list, "（不使用）", "");
	}

	obs_properties_add_button2(props, "request_perm", "申请 USB 授权", on_request_permission, s);

	obs_property_t *size_list = obs_properties_add_list(props, "lv_size", "Live View 分辨率", OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(size_list, "QVGA 320×240", 1);
	obs_property_list_add_int(size_list, "VGA 640×480", 2);
	obs_property_list_add_int(size_list, "XGA 1024×768（默认，上限）", 3);

	obs_properties_add_int_slider(props, "lv_quality", "JPEG 质量（0 最省带宽 ~ 5 最好画质）", 0, 5, 1);

	if (s) {
		char state[512];
		pthread_mutex_lock(&s->mtx);
		snprintf(state, sizeof(state), "%s", s->status);
		pthread_mutex_unlock(&s->mtx);
		if (!state[0])
			snprintf(state, sizeof(state), "空闲（源未激活或尚未开始采集）。已送出 %llu 帧",
				 (unsigned long long) s->frames_out);
		obs_properties_add_text(props, "cap_state", state, OBS_TEXT_INFO);
	}

	return props;
}

struct obs_source_info nikon_source_info = {
	.id = "nikon_webcam_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* 异步源：靠 obs_source_output_video 推帧，宽高由 libobs 从帧里推（同 uvc-input.c） */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = nikon_getname,
	.create = nikon_create,
	.destroy = nikon_destroy,
	.activate = nikon_activate,
	.deactivate = nikon_deactivate,
	.update = nikon_update,
	.get_defaults = nikon_get_defaults,
	.get_properties = nikon_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	const struct libusb_version *uv = libusb_get_version();

	/* obs_register_source 返回 void，注册被拒只会在 blog 里说一声，必须自己回读确认
	 * （android-capture 实测踩过"libobs 拒绝、模块谎报成功"） */
	obs_register_source(&nikon_source_info);

	const uint32_t flags = obs_get_source_output_flags("nikon_webcam_input");
	if (!flags) {
		blog(LOG_ERROR, "nikon-source: 'nikon_webcam_input' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO, "nikon-source: 已注册源类型 'nikon_webcam_input'（flags=0x%x / libusb %u.%u.%u.%u）",
	     (unsigned) flags, (unsigned) uv->major, (unsigned) uv->minor, (unsigned) uv->micro,
	     (unsigned) uv->nano);
	return true;
}
