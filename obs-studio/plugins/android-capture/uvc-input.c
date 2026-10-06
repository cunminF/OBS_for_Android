// UVC 采集源（USB Webcam / HDMI 采集卡，自写最小 UVC-over-libusb，无 root）。
//
// 为什么是重写而不是继续用 libuvc：旧实现经 libuvc 的 uvc_init(&ctx, NULL) 起内部
// libusb 上下文 —— 那一步在 Android 上必挂（libusb 默认初始化要建 NETLINK_KOBJECT_UEVENT
// 热插拔 socket，SELinux 直接拒，真机实测 -1/LIBUSB_ERROR_IO）。nikon-source 已真机验证
// 唯一可行的模式：libusb_init_context(LIBUSB_OPTION_NO_DEVICE_DISCOVERY) +
// libusb_wrap_sys_device(fd)。libuvc 0.0.7 不允许把自建上下文喂给 uvc_init 还让它泵事件
// （own_usb_ctx=0 就不起事件线程，回调永远不来），所以这条路只能自己写。
//
// 第二道坎：平板的内核 uvcvideo 驱动会抢先绑定采集卡（出现 /dev/videoN），claim 接口
// 返回 LIBUSB_ERROR_BUSY。Camera2 没注册外接摄像头、应用没 root 读不了 /dev/videoN，
// 所以唯一的活路是 claim 前 detach 内核驱动（本机 ROM 实测放行 USBDEVFS_DISCONNECT，
// 但代码仍按"尽力而为 + 清晰日志"写：set_auto_detach + 显式 detach，失败照记不误）。
//
// 第三道坎（真机第二、五轮测出来的）：这类采集卡**不把每个 UVC payload 用短包收尾**，
// 且 **payload 数据长度不固定** —— 每帧用一个 0 字节 payload 带 BMH EOF 位收尾（hex dump
// 实锤：12 字节头 0c 8e/0c 8d + JPEG 数据），帧内各块长度也未必一致。所以一切
// 定长/等距假设都不成立，解析器是纯字节扫描状态机：扫下一个合法头（HLE 白名单 +
// BMH EOH/保留位校验），两个头之间的字节全部归前一个头的 payload；帧边界只信 EOF；
// 误同步防护 = 帧内所有头的 PTS 必须一致（随机数据撞 PTS 约 2^-32）+ FID 一致。
// 送帧校验：MJPEG 必须 FFD8 开头、FFD9 在尾部填充窗内；未压缩必须够定长。
//
// 拔插健壮性（真机第四轮）：bulk/控制传输见 -1(IO)/-4(NO_DEVICE) 立刻拆会话重枚举；
// 设备匹配优先路径字符串，失配按 vid:pid（label 前缀）认同一台换号重插的设备；
// 流停滞有 clear_halt → 重新协商 → reset_device → 拆会话 的看门狗阶梯；协商侧
// GET probe 失败先重试、再跳过 readback 直接 SET+commit、最后 SET_INTERFACE 踢一脚。
//
// 拓扑假设（按 UGREEN 95348 / MS2130 类采集卡实测，也覆盖普通 UVC 摄像头）：
//   接口 N   = VideoControl（EP0 走 probe/commit，不需要 claim）
//   接口 N+1 = VideoStreaming，bulk IN 端点（MS2130 无等时端点，不需要 alt-setting 带宽协商）
// 描述符解析、probe/commit 协商、payload 重组都是按 UVC 1.0/1.1 规范自写的最小实现，
// 只支持三种载荷：MJPEG（turbojpeg 解码）、YUYV（直通 VIDEO_FORMAT_YUY2）、NV12。
//
// fd 从哪来：与 nikon-source 同一条路 —— Java 侧 UsbManager 打开设备，fd 经 libobs 的
// Android USB 总线（obs-android.h）传进来。fd 归 Java 所有，这里绝不 close(fd)，
// 只用 obs_android_usb_close() 归还。
//
// 线程模型：照抄 nikon-source —— 采集循环在插件自己的 pthread 上（activate 起、
// deactivate/destroy 拆，shutting_down 原子量 + join），libusb 句柄、帧缓冲全部只活在
// 这条线程里，主结构里只留状态文本和帧计数给属性页读。

#include <libusb-1.0/libusb.h>
#include <turbojpeg.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

/* obs.h 里的包含是 #ifdef __ANDROID__ 的，头本身与平台无关（同 nikon-source 的取舍） */
#include <obs-android.h>

/* ---------------- UVC 常量（规范 4.3.1.1 / A.9） ---------------- */

#define UVC_CC_VIDEO 0x0E
#define UVC_SC_VIDEOCONTROL 0x01
#define UVC_SC_VIDEOSTREAMING 0x02

/* class-specific 描述符（CS_INTERFACE = 0x24）的 bDescriptorSubType */
#define VC_HEADER 0x01
#define VS_FORMAT_UNCOMPRESSED 0x04
#define VS_FRAME_UNCOMPRESSED 0x05
#define VS_FORMAT_MJPEG 0x06
#define VS_FRAME_MJPEG 0x07

/* probe/commit 控制 */
#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
#define UVC_GET_LEN 0x85
#define UVC_GET_DEF 0x87
#define UVC_VS_PROBE_CONTROL 0x01
#define UVC_VS_COMMIT_CONTROL 0x02

/* UVC payload 头（表 2-5）：byte0 = HLE（头长），byte1 = BMH（位域） */
#define UVC_BMH_FID 0x01 /* Frame ID，帧间翻转 */
#define UVC_BMH_EOF 0x02 /* End of Frame */
#define UVC_BMH_ERR 0x40 /* 流错误：本 payload 起丢弃，下一 payload 开新帧 */
#define UVC_MAX_HLE 12

#define MAX_FRAME_BYTES (32U * 1024U * 1024U) /* 帧缓冲上限，防爆 */
#define MAX_MODES 64                          /* 单个 format 下的 frame 描述符总数上限 */

/* 连续 USB 错误超过这个数就放弃本次会话，走整体拆除 + 指数退避重开（同 nikon-source） */
#define MAX_CONSEC_USB_ERR 5
#define RETRY_BACKOFF_MIN_MS 1000ULL
#define RETRY_BACKOFF_MAX_MS 8000ULL
/* 同一实例连续这么多轮没送出任何帧 = 实例已死（IN 路径全 -1，只有拔插能救），
 * 退避拉到 30s 一档，别 1 秒一轮地锤（真机 1617：猝死后 GET probe -1 无限循环） */
#define DEAD_ROUND_LIMIT 5U
#define DEAD_ROUND_BACKOFF_MS 30000ULL

/* bulk 读超时：设备正常出流时读块间隔远小于它；超时只意味着断流/拔线 */
#define BULK_TIMEOUT_MS 1000
/* bulk 读块长（单 URB 大小，调它就改这里）：64KB × 16 个 = 总在飞 1MB。
 * 平板 xHCI 对大而少的 URB 有调度/兼容性问题（真机：256KB×8 时帧频繁少几十字节），
 * 小而多更稳；总量不变，FIFO 防溢能力不变 */
#define BULK_CHUNK_SIZE (64U * 1024U)
/* 异步流水线：始终挂在总线上的 bulk 传输数（在飞数，调它就改这里；libuvc 默认 8，
 * Windows UVC 驱动同款思路）。64KB × 32 = 总在飞 2MB——采集卡内部 FIFO 很小、
 * HDMI 侧持续灌入，总在飞字节越深，平板 xHCI 高带宽（1080p60 YUYV ≈250MB/s）下
 * 越不容易溢出；回调/拷贝开销按完成频次计（64KB memcpy + 重新提交，微秒级），
 * 与在飞数无关，不会劣化 720p 的延迟 */
#define XFER_IN_FLIGHT 32
/* 瞬时错误容错（Linux uvcvideo 语义：单个 URB 错了原地重提交，错误率持续超标才升级）：
 * 每秒错误超 XFER_ERR_RATE_LIMIT 记一个坏秒，连续 XFER_ERR_BAD_SECONDS 秒才拆会话 */
#define XFER_ERR_RATE_LIMIT 20U
#define XFER_ERR_BAD_SECONDS 5U
/* 冻结看门狗：流"在跑"但连续这么久没有任何完整帧送出（数据来不来都算）→ 重新协商
 * 重进流；连续这么多次无效 → 拆会话。真机：1080p60 YUYV 喂不饱冻死，保帧机制把
 * "流死了"显示成"稳定"，不能再让画面定格装死 */
#define FREEZE_WATCHDOG_NS 8000000000ULL
#define FREEZE_MAX_RETRY 3U
/* 纯字节扫描状态机（不定长 payload）：误同步防护靠 PTS 一致性（帧内所有头的 PTS
 * 相等，随机数据撞上的概率约 2^-32）+ FID 一致性。边界只信 EOF（这台设备用空
 * payload 带 EOF 位收尾每帧）；连续拒这么多"边界声称"候选 = EOF 真丢了，重同步 */
#define REJECT_RESYNC 64U
/* 连续坏帧这么多 = 同步多半是假的，重同步 */
#define BADFRAME_RESYNC 30U
/* 流缓冲高水位：攒这么多还定不了下一个头 = 流异常，丢前半重扫（必须 < sbuf_cap - 读块） */
#define SBUF_HIGH_WATER (768U * 1024U)
/* MJPEG 送帧校验：FFD9 允许距帧尾这么多字节内的填充 */
#define FRAME_TAIL_SEARCH 512U
/* 连续失步这么多次后 hex dump 当前位置 64 字节诊断（每轮只打这一次） */
#define RESYNC_DUMP_AFTER 3

/* 停滞看门狗阶梯（单位 = 连续无数据读的次数，1 次 ≈ 1 秒 bulk 读超时）：
 * clear_halt → 重新协商 → reset_device → 拆会话重开，每步留日志 */
#define STALL_CLEAR_HALT 8U
#define STALL_RENEGOTIATE 16U
#define STALL_RESET_DEVICE 24U
#define STALL_TEARDOWN 32U

/* 未压缩 format GUID 的前 4 字节就是 FourCC（小端 ASCII，UVC 规范附录 A） */
#define FOURCC_YUY2 0x32595559 /* "YUY2" */
#define FOURCC_NV12 0x3231564E /* "NV12" */

enum uvc_mode_kind {
	MODE_MJPEG = 0,
	MODE_YUYV,
	MODE_NV12,
};

/* 一个可选模式 = format 描述符 × frame 描述符摊平后的结果 */
struct uvc_mode {
	enum uvc_mode_kind kind;
	uint8_t fmt_index;
	uint8_t frame_index;
	uint16_t w;
	uint16_t h;
	uint8_t bpp;
	uint32_t max_frame_size;
	uint32_t def_interval; /* 100 ns 单位 */
	/* bFrameIntervalType==0 时是连续区间 [min,max] 步长 step；否则是离散表 */
	uint32_t min_iv;
	uint32_t max_iv;
	uint32_t step_iv;
	uint32_t intervals[16];
	int n_intervals;
};

struct uvc_source {
	obs_source_t *source;

	/* 设置快照（usb_device 是 UsbDevice.getDeviceName()，形如 /dev/bus/usb/002/002） */
	char usb_device[OBS_ANDROID_USB_NAME_LEN];
	int width;
	int height;
	int fps;
	int prefer_fmt;      /* enum uvc_mode_kind，-1 = 自动（优先 MJPEG） */
	char fmt_setting[16]; /* video_format 设置的原始字符串，打日志消歧义用 */
	char usb_id[16];      /* 首台匹配设备的 "vid:pid"（label 前缀），拔插换号后按它认设备 */

	/* 采集线程与拆除协议（照 nikon-source/camera2-input.c：shutting_down 置 1 后 join） */
	pthread_t thread;
	volatile long thread_started;
	volatile long shutting_down;

	/* 状态文本：采集线程写、UI 线程读，mtx 只护它 */
	pthread_mutex_t mtx;
	char status[256];

	/* 只写给人看的计数：采集线程写、属性页读，不做任何判定依据 */
	uint64_t frames_out;
	uint64_t frames_dropped;
	uint32_t cur_w;
	uint32_t cur_h;
	uint32_t cur_fps_x100; /* 实测帧率 ×100（每秒估一次） */
};

/* 一轮采集会话的全部运行期状态，只活在采集线程栈上 */
struct uvc_dev {
	libusb_context *ctx;
	libusb_device_handle *h;
	int bridge_fd; /* 从总线借来的 fd，归还用 obs_android_usb_close */

	int vc_iface;
	int vs_iface;
	int vs_alt; /* VS 的工作 altsetting（协商失败时 SET_INTERFACE 踢状态机用） */
	uint8_t ep_in;
	uint16_t uvc_version;  /* bcdUVC，决定 probe 结构长度 */
	uint32_t probe_len;    /* 26（1.0）/ 34（1.1）/ 48（1.5），GET_LEN 优先 */
	uint32_t payload_size; /* dwMaxPayloadTransferSize（0 = 设备没报，读块用固定大块） */

	struct uvc_mode modes[MAX_MODES];
	int n_modes;
	int chosen; /* modes[] 下标，-1 = 未选 */

	uint32_t quirks;           /* 命中 uvc_quirks 表的标志位（0 = 无） */
	int eff_fmt;               /* 生效的偏好格式（怪癖默认覆盖后；否则 = s->prefer_fmt） */
	int eff_w, eff_h, eff_fps; /* 生效的分辨率/帧率（怪癖默认覆盖后） */

	uint32_t frame_interval; /* 协商结果，100 ns */
	uint32_t frame_buf_size; /* dwMaxVideoFrameSize 协商读回值（可能是脏值，只当下限参考） */
};

/* bulk 字节流的解析状态：设备会把几百个 payload 混进一次传输，必须跨读块攒流再切。
 * 这台设备 payload 数据长度不固定（存在 0 字节的 EOF 收尾 payload），一切定长/等距
 * 假设都不成立——纯字节扫描状态机：扫下一个合法头，两个头之间的字节归前一个头。 */
struct uvc_stream {
	uint8_t *sbuf; /* 未解析的流字节（跨读块保留，消费后压实） */
	size_t sbuf_cap;
	size_t sbuf_len;
	size_t scan_off; /* 扫描游标：之前的字节已确认不是候选头起点 */

	uint8_t hle; /* 会话头长（同步时从首个合法头学来，0 = 未同步；这台 = 12） */

	uint64_t sync_read;     /* 未同步期间累计读入字节（诊断用） */
	unsigned resync_streak; /* 连续重同步次数，够 RESYNC_DUMP_AFTER 打一次 hex dump */
	unsigned rejected;      /* 连续被拒的"边界声称"候选头，够 REJECT_RESYNC 重同步 */
	unsigned bad_streak;    /* 连续坏帧数，够 BADFRAME_RESYNC 重同步 */
	bool diag_dumped;       /* 本轮已打过一次诊断 dump，之后不再打 */
	uint64_t frames_done;   /* 重组完成的帧数（含坏帧），打点限频用 */

	uint8_t *frame_buf; /* 当前帧的纯载荷重组（只含数据、绝不含 payload 头） */
	size_t frame_cap;
	size_t frame_len;
	uint32_t frame_total; /* 未压缩帧的定长；MJPEG = 0（不定长） */
	bool assembling;
	uint8_t last_fid;
	uint32_t frame_pts; /* 当前帧登记的 PTS（帧内所有头的 PTS 应相等） */
	bool has_pts;

	uint64_t err_first_ns; /* 当前 BMH ERR 连续段起点 ns（0 = 不在段内） */
	uint64_t err_count;    /* 本会话累计 BMH ERR 头数 */

	uint64_t ll_err;       /* 以下是限频日志槽：上次放行时间 ns（0 = 还没打过） */
	uint64_t ll_highwater;
	uint64_t ll_badframe;
	uint64_t ll_overflow;
	uint64_t ll_resync;
};

/* ---------------- 小端读写（UVC 描述符/控制结构全部 LSB first） ---------------- */

static uint16_t get_u16le(const uint8_t *p)
{
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

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

static const char *mode_kind_name(enum uvc_mode_kind k)
{
	switch (k) {
	case MODE_MJPEG:
		return "MJPEG";
	case MODE_YUYV:
		return "YUYV";
	case MODE_NV12:
		return "NV12";
	default:
		return "?";
	}
}

/* 未压缩帧的精确字节数；MJPEG 返回 0（不定长） */
static uint32_t raw_frame_bytes(const struct uvc_mode *m)
{
	if (m->kind == MODE_YUYV)
		return (uint32_t) m->w * (uint32_t) m->h * 2;
	if (m->kind == MODE_NV12)
		return (uint32_t) m->w * (uint32_t) m->h * 3 / 2;
	return 0;
}

static void set_status(struct uvc_source *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	pthread_mutex_lock(&s->mtx);
	vsnprintf(s->status, sizeof(s->status), fmt, args);
	pthread_mutex_unlock(&s->mtx);
	va_end(args);
}

/* 可中断的睡眠：退避期间也要对 deactivate/destroy 及时响应 */
static void sleep_check(struct uvc_source *s, uint32_t ms)
{
	while (ms-- && !os_atomic_load_long(&s->shutting_down))
		os_sleep_ms(1);
}

/* ---------------- VS 描述符解析（format/frame 摊平成 modes[]） ---------------- */

/* 帧间隔：fps → 100 ns 单位；离散表取最接近的一档，连续区间按步长取整夹取 */
static uint32_t pick_interval(const struct uvc_mode *m, int fps)
{
	if (fps <= 0)
		return m->def_interval;

	const uint32_t want = 10000000U / (uint32_t) fps;

	if (m->n_intervals > 0) {
		uint32_t best = m->intervals[0];
		for (int i = 1; i < m->n_intervals; i++) {
			const uint32_t iv = m->intervals[i];
			const int64_t d_old = (int64_t) best - (int64_t) want;
			const int64_t d_new = (int64_t) iv - (int64_t) want;
			if (d_new * d_new < d_old * d_old)
				best = iv;
		}
		return best;
	}
	if (m->min_iv == 0 || m->max_iv == 0)
		return m->def_interval;

	uint32_t iv = want < m->min_iv ? m->min_iv : want > m->max_iv ? m->max_iv : want;
	if (m->step_iv)
		iv = m->min_iv + ((iv - m->min_iv) / m->step_iv) * m->step_iv;
	return iv;
}

/* 往 modes[] 里追加一个 frame；返回 false = 满了 */
static bool add_mode(struct uvc_dev *d, enum uvc_mode_kind kind, uint8_t fmt_index, uint8_t bpp,
		     const uint8_t *f /* FRAME 描述符本体（含头 3 字节） */, size_t flen)
{
	if (d->n_modes >= MAX_MODES || flen < 26)
		return false;

	struct uvc_mode *m = &d->modes[d->n_modes];
	memset(m, 0, sizeof(*m));
	m->kind = kind;
	m->fmt_index = fmt_index;
	m->frame_index = f[3];
	m->bpp = bpp;
	m->w = get_u16le(f + 5);
	m->h = get_u16le(f + 7);
	m->max_frame_size = get_u32le(f + 17);
	m->def_interval = get_u32le(f + 21);
	m->min_iv = m->def_interval;
	m->max_iv = m->def_interval;
	m->step_iv = 0;

	const uint8_t itype = f[25];
	if (itype == 0) {
		/* 连续区间：dwMinFrameInterval/dwMaxFrameInterval/dwFrameIntervalStep */
		if (flen >= 38) {
			m->min_iv = get_u32le(f + 26);
			m->max_iv = get_u32le(f + 30);
			m->step_iv = get_u32le(f + 34);
		}
	} else {
		const int n = itype > 16 ? 16 : itype;
		for (int i = 0; i < n && 26 + 4 * (i + 1) <= (int) flen; i++)
			m->intervals[m->n_intervals++] = get_u32le(f + 26 + 4 * i);
	}

	if (m->w == 0 || m->h == 0)
		return true; /* 非法 frame 不致命，跳过但不记 */
	d->n_modes++;
	return true;
}

/* 走一个 altsetting 的 extra 字节链，把 MJPEG / 未压缩（YUY2、NV12）的 frame 全收进 modes[]。 */
static void parse_vs_extra(struct uvc_dev *d, const uint8_t *extra, int len)
{
	enum uvc_mode_kind cur_kind = MODE_MJPEG;
	uint8_t cur_fmt = 0;
	uint8_t cur_bpp = 0;
	bool cur_valid = false;
	int off = 0;

	while (off + 3 <= len) {
		const uint8_t dlen = extra[off];
		const uint8_t dtype = extra[off + 1];
		const uint8_t subtype = extra[off + 2];
		if (dlen < 3 || off + dlen > len)
			break;

		if (dtype == 0x24 /* CS_INTERFACE */) {
			switch (subtype) {
			case VS_FORMAT_MJPEG:
				cur_kind = MODE_MJPEG;
				cur_fmt = extra[off + 3];
				cur_bpp = 0;
				cur_valid = true;
				break;
			case VS_FORMAT_UNCOMPRESSED: {
				const uint32_t fourcc = get_u32le(extra + off + 5);
				cur_fmt = extra[off + 3];
				cur_bpp = extra[off + 21];
				cur_valid = true;
				if (fourcc == FOURCC_YUY2)
					cur_kind = MODE_YUYV;
				else if (fourcc == FOURCC_NV12)
					cur_kind = MODE_NV12;
				else
					cur_valid = false; /* UYVY/RGB 等没映射，跳过 */
				break;
			}
			case VS_FRAME_MJPEG:
			case VS_FRAME_UNCOMPRESSED:
				if (cur_valid &&
				    ((subtype == VS_FRAME_MJPEG) == (cur_kind == MODE_MJPEG)))
					add_mode(d, cur_kind, cur_fmt, cur_bpp, extra + off, dlen);
				break;
			default:
				break;
			}
		}
		off += dlen;
	}
}

/* 读配置描述符，找出 VC/VS 接口、bulk IN 端点、bcdUVC，并解析全部 VS 模式。 */
static bool uvc_scan_device(struct uvc_dev *d)
{
	struct libusb_config_descriptor *cfg = NULL;
	libusb_device *dev = libusb_get_device(d->h);

	d->vc_iface = -1;
	d->vs_iface = -1;
	d->vs_alt = 0;
	d->ep_in = 0;
	d->uvc_version = 0x0100;
	d->n_modes = 0;

	int r = libusb_get_active_config_descriptor(dev, &cfg);
	if (r != 0)
		r = libusb_get_config_descriptor(dev, 0, &cfg);
	if (r != 0 || !cfg) {
		blog(LOG_ERROR, "uvc: 读不到配置描述符（%d）", r);
		return false;
	}

	for (int i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];
		for (int a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
			if (alt->bInterfaceClass != UVC_CC_VIDEO)
				continue;

			if (alt->bInterfaceSubClass == UVC_SC_VIDEOCONTROL && d->vc_iface < 0) {
				d->vc_iface = alt->bInterfaceNumber;
				/* VC_HEADER 的 bcdUVC 决定 probe 结构长度 */
				const uint8_t *ex = alt->extra;
				if (alt->extra_length >= 5 && ex[1] == 0x24 && ex[2] == VC_HEADER)
					d->uvc_version = get_u16le(ex + 3);
				continue;
			}
			if (alt->bInterfaceSubClass != UVC_SC_VIDEOSTREAMING)
				continue;

			/* 只要 bulk IN 的 altsetting：MS2130 类采集卡没有等时端点，
			 * 等时那条路（alt-setting 带宽协商）本实现不做，见到了也只记日志 */
			uint8_t bulk_in = 0;
			for (int e = 0; e < alt->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
				const uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
				if (type == LIBUSB_TRANSFER_TYPE_BULK &&
				    (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
					bulk_in = ep->bEndpointAddress;
			}
			if (!bulk_in) {
				blog(LOG_WARNING,
				     "uvc: VS 接口 %d alt %d 没有 bulk IN 端点（等时设备不受支持），跳过",
				     alt->bInterfaceNumber, alt->bAlternateSetting);
				continue;
			}

			if (d->vs_iface < 0) {
				d->vs_iface = alt->bInterfaceNumber;
				d->vs_alt = alt->bAlternateSetting;
				d->ep_in = bulk_in;
			}
			parse_vs_extra(d, alt->extra, alt->extra_length);
		}
	}
	libusb_free_config_descriptor(cfg);

	blog(LOG_INFO, "uvc: 描述符扫描：VC 接口=%d VS 接口=%d bulk-IN=0x%02X bcdUVC=0x%04X 模式数=%d", d->vc_iface,
	     d->vs_iface, d->ep_in, (unsigned) d->uvc_version, d->n_modes);

	if (d->vs_iface < 0 || d->ep_in == 0 || d->n_modes == 0) {
		blog(LOG_ERROR, "uvc: 没找到可用的 UVC 流接口/模式（设备可能不是标准 UVC）");
		return false;
	}

	for (int i = 0; i < d->n_modes; i++) {
		const struct uvc_mode *m = &d->modes[i];
		blog(LOG_INFO, "uvc:   模式[%d] %s fmt=%u frame=%u %ux%u 间隔=%u(100ns) 档数=%d 最大帧长=%u", i,
		     mode_kind_name(m->kind), (unsigned) m->fmt_index, (unsigned) m->frame_index,
		     (unsigned) m->w, (unsigned) m->h, (unsigned) m->def_interval, m->n_intervals,
		     (unsigned) m->max_frame_size);
	}
	return true;
}

/* ---------------- 设备怪癖表（vid:pid → 行为标志，真机实测录入，勿凭猜添加） ---------------- */

#define QUIRK_PREFER_YUYV 0x01 /* MJPEG/NV12 通路出全帧黑场（固件 bug），仅 YUYV 出真实画面 */

struct uvc_quirk {
	uint16_t vid;
	uint16_t pid;
	uint32_t flags;
};

static const struct uvc_quirk uvc_quirks[] = {
	/* UGREEN 95348（MS2130 类）：MJPEG/NV12 全帧黑场，Windows 上同样复现；
	 * YUYV 各分辨率只有 166666（60fps）一档离散帧率 */
	{0x2b89, 0x5348, QUIRK_PREFER_YUYV},
};

static uint32_t quirks_for(const char *usb_id /* "2b89:5348" 形（label 前缀） */)
{
	unsigned vid = 0, pid = 0;
	if (!usb_id || sscanf(usb_id, "%x:%x", &vid, &pid) != 2)
		return 0;
	for (size_t i = 0; i < sizeof(uvc_quirks) / sizeof(uvc_quirks[0]); i++)
		if (uvc_quirks[i].vid == vid && uvc_quirks[i].pid == pid)
			return uvc_quirks[i].flags;
	return 0;
}

/* ---------------- 模式选择（优先用户设置，回退 MJPEG 1080p → 最大 MJPEG → 最大未压缩） ---------------- */

static int mode_score(const struct uvc_dev *d, int idx, const struct uvc_source *s)
{
	const struct uvc_mode *m = &d->modes[idx];
	UNUSED_PARAMETER(s); /* 评分全部用怪癖覆盖后的生效值（d->eff_*） */

	/* 用户指定的格式 + 分辨率命中（用怪癖覆盖后的生效值） */
	if (d->eff_fmt >= 0 && m->kind == (enum uvc_mode_kind) d->eff_fmt && m->w == d->eff_w &&
	    m->h == d->eff_h)
		return 100;
	/* 用户指定的格式 + 1080p */
	if (d->eff_fmt >= 0 && m->kind == (enum uvc_mode_kind) d->eff_fmt && m->w == 1920 && m->h == 1080)
		return 90;
	/* MJPEG 1080p */
	if (m->kind == MODE_MJPEG && m->w == 1920 && m->h == 1080)
		return 80;
	/* 用户指定分辨率（格式没匹配上时） */
	if (m->w == d->eff_w && m->h == d->eff_h)
		return 70;
	return -1; /* 交给面积回退 */
}

static int choose_mode(struct uvc_dev *d, const struct uvc_source *s)
{
	int best = -1;
	int best_score = -1;
	uint32_t best_area = 0;

	for (int i = 0; i < d->n_modes; i++) {
		const struct uvc_mode *m = &d->modes[i];
		const int score = mode_score(d, i, s);
		if (score > best_score) {
			best_score = score;
			best = i;
			best_area = 0;
			continue;
		}
		/* 同分或都没命中时：MJPEG 优先于未压缩，面积大者优先 */
		if (score == best_score || (best_score < 0)) {
			const uint32_t area = (uint32_t) m->w * (uint32_t) m->h;
			const int kind_rank = m->kind == MODE_MJPEG ? 1 : 0;
			const int best_rank = best >= 0 && d->modes[best].kind == MODE_MJPEG ? 1 : 0;
			if (best < 0 || kind_rank > best_rank || (kind_rank == best_rank && area > best_area)) {
				best = i;
				best_area = area;
			}
		}
	}

	/* 怪癖改判：仅 YUYV 可用的卡，同分辨率有 YUYV 档就无条件改选（这台卡
	 * MJPEG/NV12 出全帧黑场，用户手选 MJPEG 也救不回来，日志说清原因） */
	if (best >= 0 && (d->quirks & QUIRK_PREFER_YUYV) && d->modes[best].kind != MODE_YUYV) {
		const struct uvc_mode *bm = &d->modes[best];
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (m->kind == MODE_YUYV && m->w == bm->w && m->h == bm->h) {
				blog(LOG_WARNING,
				     "uvc: 怪癖改选 YUYV（这台卡 MJPEG/NV12 出全帧黑场，仅 YUYV 可用；原选 %s %ux%u）",
				     mode_kind_name(bm->kind), (unsigned) bm->w, (unsigned) bm->h);
				best = i;
				break;
			}
		}
	}

	/* 真机教训改判：这类卡（MS2130）1080p 的未压缩档只有几 fps，能用的是 MJPEG。
	 * 评分若选出了非 MJPEG，但同分辨率有帧率更高的 MJPEG 档，改选 MJPEG 并把账打出来
	 * —— 显式选了未压缩的用户看到日志也知道为什么被改判、原档到底有多慢。
	 * （QUIRK_PREFER_YUYV 的卡跳过：那台 MJPEG 是黑场，绝不能往 MJPEG 改判） */
	if (best >= 0 && d->modes[best].kind != MODE_MJPEG && !(d->quirks & QUIRK_PREFER_YUYV)) {
		const struct uvc_mode *bm = &d->modes[best];
		const uint32_t biv = pick_interval(bm, d->eff_fps);
		const uint32_t best_fps100 = biv ? (uint32_t) (1000000000ULL / biv) : 0;
		int mj = -1;
		uint32_t mj_fps100 = 0;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (m->kind != MODE_MJPEG || m->w != bm->w || m->h != bm->h)
				continue;
			const uint32_t iv = pick_interval(m, d->eff_fps);
			const uint32_t f = iv ? (uint32_t) (1000000000ULL / iv) : 0;
			if (mj < 0 || f > mj_fps100) {
				mj = i;
				mj_fps100 = f;
			}
		}
		if (mj >= 0 && mj_fps100 > best_fps100) {
			blog(LOG_INFO,
			     "uvc: 改选 MJPEG（同为 %ux%u：%s 档最快 ≈%.2f fps，MJPEG 档 ≈%.2f fps；设置项 video_format='%s'）",
			     (unsigned) bm->w, (unsigned) bm->h, mode_kind_name(bm->kind), best_fps100 / 100.0,
			     mj_fps100 / 100.0, s->fmt_setting);
			best = mj;
		}
	}
	return best;
}

/* ---------------- probe/commit 协商（UVC 规范 4.3.1.1） ---------------- */

/* GET_LEN 问 probe 结构的实际长度；问不到就按 bcdUVC 推（1.0=26 / 1.1=34 / 1.5=48） */
static uint32_t probe_struct_len(struct uvc_dev *d)
{
	uint8_t l[2] = {0};
	const int r = libusb_control_transfer(d->h, 0xA1, UVC_GET_LEN, UVC_VS_PROBE_CONTROL << 8,
					      (uint16_t) d->vs_iface, l, sizeof(l), 1000);
	if (r == 2) {
		const uint32_t n = get_u16le(l);
		if (n >= 26 && n <= 48)
			return n;
	}
	return d->uvc_version >= 0x0150 ? 48 : d->uvc_version >= 0x0110 ? 34 : 26;
}

/* 把协商结果从 probe 结构里读回 d（间隔 / 帧长 / payload 长） */
static void probe_readback(struct uvc_dev *d, const uint8_t *p, uint32_t len)
{
	if (len >= 26) {
		d->frame_interval = get_u32le(p + 4);
		d->frame_buf_size = get_u32le(p + 22);
	}
	if (len >= 34)
		d->payload_size = get_u32le(p + 26);
}

/* 一次协商尝试：probe 结构以 GET_DEF 设备默认打底（Windows UVC 驱动做法），只覆盖
 * bFormatIndex/bFrameIndex/dwFrameInterval 三项 → SET probe → GET probe（读回，
 * 失败重试 3 次间隔 200ms）→ SET probe（确认值）→ SET commit。
 * GET_DEF 拿不到回退历史手填版（黑场谜案：手填的 interval/maxframe/bmHint 可能把
 * 固件拐进黑场模式——设备按所请格式编出 CBR 黑帧，尺寸∝分辨率实锤）。
 * GET 3 次都挂就跳过 readback 直接 SET+commit 期望值（真机第四轮：拔插后这台卡
 * GET probe 永远 -1，但 SET 类 OUT 请求是收的；很多廉价设备 GET 本来就不稳）。
 * 返回 0 = 协商成功，d->frame_interval/frame_buf_size/payload_size 已填。 */
static int negotiate_try(struct uvc_dev *d, struct uvc_source *s)
{
	uint8_t probe[48];
	uint8_t back[48];
	uint8_t hand[48];
	const struct uvc_mode *m = &d->modes[d->chosen];
	UNUSED_PARAMETER(s); /* 参数全部走怪癖覆盖后的生效值（d->eff_*） */

	d->probe_len = probe_struct_len(d);
	d->payload_size = 0;
	memset(probe, 0, sizeof(probe));
	memset(back, 0, sizeof(back));

	/* 帧间隔一次算清：离散表吸最近档，连续区间夹取；请求的 fps 不在表里时
	 * 明说吸到了哪档（真机误解实录：这卡 YUYV 只有 166666 一档，请求 30fps
	 * 实际跑 60fps = 双倍带宽，"稳定"其实是冻死的最后一帧） */
	const uint32_t want_iv = pick_interval(m, d->eff_fps);
	if (d->eff_fps > 0 && want_iv != 10000000U / (uint32_t) d->eff_fps)
		blog(LOG_WARNING, "uvc: 请求 %d fps 设备不支持（%s），吸到最近档 ≈%.2f fps（interval %u → %u）",
		     d->eff_fps, m->n_intervals > 0 ? "离散帧率表没有这一档" : "超出连续区间，已夹取",
		     10000000.0 / want_iv, 10000000U / (uint32_t) d->eff_fps, (unsigned) want_iv);

	/* 手填版（历史行为，留作对照打印 + GET_DEF 失败时的兜底）。bmHint bit0 =
	 * dwFrameInterval 要紧；dwMaxVideoFrameSize 按规范填期望值：这台卡读回过
	 * 16384 的脏值（真帧 4 MB），很多设备不在乎这个字段，但填对更稳 */
	memset(hand, 0, sizeof(hand));
	put_u16le(hand, 0x0001);
	hand[2] = m->fmt_index;
	hand[3] = m->frame_index;
	put_u32le(hand + 4, want_iv);
	put_u32le(hand + 22, m->kind == MODE_MJPEG ? m->max_frame_size : raw_frame_bytes(m));

	int r = libusb_control_transfer(d->h, 0xA1, UVC_GET_DEF, UVC_VS_PROBE_CONTROL << 8,
					(uint16_t) d->vs_iface, probe, (uint16_t) d->probe_len, 1000);
	if (r == (int) d->probe_len) {
		char hex[48 * 3 + 1];
		for (uint32_t i = 0; i < d->probe_len; i++)
			snprintf(hex + i * 3, 4, "%02x ", probe[i]);
		blog(LOG_INFO, "uvc: GET_DEF probe（设备默认 %u 字节）：%s", (unsigned) d->probe_len, hex);
		blog(LOG_INFO,
		     "uvc: 设备默认 fmt=%u frame=%u interval=%u(≈%u fps) maxframe=%u payload=%u | "
		     "手填对照 interval=%u maxframe=%u",
		     (unsigned) probe[2], (unsigned) probe[3], (unsigned) get_u32le(probe + 4),
		     get_u32le(probe + 4) ? 10000000U / get_u32le(probe + 4) : 0U,
		     (unsigned) get_u32le(probe + 22),
		     d->probe_len >= 34 ? (unsigned) get_u32le(probe + 26) : 0U,
		     (unsigned) get_u32le(hand + 4), (unsigned) get_u32le(hand + 22));
		/* 只覆盖三项：bFormatIndex / bFrameIndex / dwFrameInterval（bmHint 等
		 * 其余字段保持设备默认，与 Windows 驱动一致） */
		probe[2] = m->fmt_index;
		probe[3] = m->frame_index;
		put_u32le(probe + 4, want_iv);
	} else {
		blog(LOG_WARNING, "uvc: GET_DEF probe 失败 r=%d，回退手填版", r);
		memcpy(probe, hand, sizeof(probe));
	}

	r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8, (uint16_t) d->vs_iface,
					probe, (uint16_t) d->probe_len, 1000);
	if (r != (int) d->probe_len) {
		blog(LOG_ERROR, "uvc: SET probe 失败 r=%d（想要 %u 字节）", r, (unsigned) d->probe_len);
		return r < 0 ? r : LIBUSB_ERROR_IO;
	}

	bool got = false;
	for (int t = 0; t < 3 && !got; t++) {
		if (t)
			os_sleep_ms(200);
		r = libusb_control_transfer(d->h, 0xA1, UVC_GET_CUR, UVC_VS_PROBE_CONTROL << 8,
					    (uint16_t) d->vs_iface, back, (uint16_t) d->probe_len, 1000);
		got = r == (int) d->probe_len;
		if (!got)
			blog(LOG_WARNING, "uvc: GET probe 第 %d/3 次失败 r=%d", t + 1, r);
	}

	if (got) {
		probe_readback(d, back, d->probe_len);
		/* 用设备读回的值再 SET 一次 probe，然后 commit（规范流程，部分设备缺这步会拒） */
		r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8,
					    (uint16_t) d->vs_iface, back, (uint16_t) d->probe_len, 1000);
		if (r != (int) d->probe_len)
			blog(LOG_WARNING, "uvc: 二次 SET probe 失败 r=%d，照样试 commit", r);
	} else {
		blog(LOG_WARNING, "uvc: GET probe 3 次都失败 —— 跳过 readback，直接 SET+commit 期望值");
		memcpy(back, probe, sizeof(back));
		d->frame_interval = want_iv;
		d->frame_buf_size = m->kind == MODE_MJPEG ? m->max_frame_size : raw_frame_bytes(m);
		r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8,
					    (uint16_t) d->vs_iface, back, (uint16_t) d->probe_len, 1000);
		if (r != (int) d->probe_len)
			blog(LOG_WARNING, "uvc: 跳过 readback 路径的二次 SET 失败 r=%d，照样试 commit", r);
	}

	r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL << 8, (uint16_t) d->vs_iface,
					back, (uint16_t) d->probe_len, 1000);
	if (r != (int) d->probe_len) {
		blog(LOG_ERROR, "uvc: SET commit 失败 r=%d", r);
		return r < 0 ? r : LIBUSB_ERROR_IO;
	}

	if (d->frame_interval == 0)
		d->frame_interval = m->def_interval;

	blog(LOG_INFO,
	     "uvc: 协商成功 —— %s %ux%u interval=%u(≈%u fps) maxframe=%u payload=%u（probe 结构 %u 字节，bcdUVC=0x%04X，readback %s）",
	     mode_kind_name(m->kind), (unsigned) m->w, (unsigned) m->h, (unsigned) d->frame_interval,
	     d->frame_interval ? 10000000U / d->frame_interval : 0U, (unsigned) d->frame_buf_size,
	     (unsigned) d->payload_size, (unsigned) d->probe_len, (unsigned) d->uvc_version,
	     got ? "成功" : "跳过");
	return 0;
}

/* 协商入口：先试一轮，失败就 SET_INTERFACE alt0→工作alt 踢一脚 VS 接口状态机再试一轮
 * （部分设备拔插后 VS 接口状态机卡住，踢一脚能缓过来）。每步打日志。 */
static int uvc_negotiate(struct uvc_dev *d, struct uvc_source *s)
{
	int r = negotiate_try(d, s);
	if (r == 0 || d->vs_iface < 0)
		return r;

	blog(LOG_WARNING, "uvc: 协商失败（%d），SET_INTERFACE alt0→alt%d 踢一脚再试", r, d->vs_alt);
	const int s0 = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
	os_sleep_ms(100);
	const int s1 = libusb_set_interface_alt_setting(d->h, d->vs_iface, d->vs_alt);
	blog(LOG_WARNING, "uvc: SET_INTERFACE alt0 r=%d，工作 alt%d r=%d", s0, d->vs_alt, s1);
	return negotiate_try(d, s);
}

/* ---------------- 帧送出 ---------------- */

static const char *tj_subsamp_name(int s)
{
	switch (s) {
	case TJSAMP_444: return "444";
	case TJSAMP_422: return "422";
	case TJSAMP_420: return "420";
	case TJSAMP_GRAY: return "gray";
	case TJSAMP_440: return "440";
	case TJSAMP_411: return "411";
	default: return "?";
	}
}

static const char *tj_cs_name(int c)
{
	switch (c) {
	case TJCS_RGB: return "RGB";
	case TJCS_YCbCr: return "YCbCr";
	case TJCS_GRAY: return "GRAY";
	case TJCS_CMYK: return "CMYK";
	case TJCS_YCCK: return "YCCK";
	default: return "?";
	}
}

/* 黑场判定（黑场自降级看门狗用）：步长 64 字节采样，任一采样点亮度/通道 ≥30 = 有内容。
 * bpp=4 → BGRA（B/G/R 任一）;bpp=1 → YUYV 的 Y（偶数字节，64 步长全落偶数）或
 * NV12 的 Y 平面（调用方把 len 限在 Y 平面内，别采到 UV 的 128 误判有内容） */
static bool frame_is_black(const uint8_t *buf, size_t len, int bpp)
{
	for (size_t p = 0; p < len; p += 64) {
		if (bpp == 4) {
			if (buf[p] >= 30 || (p + 1 < len && buf[p + 1] >= 30) || (p + 2 < len && buf[p + 2] >= 30))
				return false;
		} else if (buf[p] >= 30) {
			return false;
		}
	}
	return true;
}

static void push_frame_bgra(struct uvc_source *s, uint8_t *bgra, int w, int h)
{
	struct obs_source_frame out = {0};
	out.width = (uint32_t) w;
	out.height = (uint32_t) h;
	out.format = VIDEO_FORMAT_BGRA;
	out.data[0] = bgra;
	out.linesize[0] = (uint32_t) w * 4;
	out.full_range = true;
	out.timestamp = os_gettime_ns();
	/* YUV 着色器无条件读 color_matrix/range 上下限，不填就是纯黑（仓内先例 v4l2-input.c:128） */
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_FULL, VIDEO_FORMAT_BGRA,
					       out.color_matrix, out.color_range_min, out.color_range_max);
	obs_source_output_video(s->source, &out);

	/* 像素探针：WSL 复现已实锤——同一调用序列解 splash_raw.jpg 在四个解码器
	 * （libjpeg-turbo 2.1.5/3.1.0 同族、Pillow、ffmpeg 7/8）下都得 B=G=R=7，与
	 * Android 探针一致，即解码忠实、采集卡给的就是黑帧。这里升级成"中心 + 全帧
	 * 均值/峰值"：全帧峰值 < 30 = 卡出黑场（查 HDMI 源/卡状态）；有明显内容但
	 * 预览黑 = 前端显示问题 */
	if (s->frames_out < 3 || (s->frames_out + 1) % 300 == 0) {
		const int cw = w < 64 ? w : 64;
		const int ch = h < 64 ? h : 64;
		uint64_t sb = 0, sg = 0, sr = 0;
		for (int yy = 0; yy < ch; yy++) {
			const uint8_t *row =
				bgra + (size_t) (h / 2 - ch / 2 + yy) * (size_t) w * 4 + (size_t) (w / 2 - cw / 2) * 4;
			for (int xx = 0; xx < cw; xx++) {
				sb += row[xx * 4];
				sg += row[xx * 4 + 1];
				sr += row[xx * 4 + 2];
			}
		}
		const uint64_t n = (uint64_t) cw * (uint64_t) ch;
		uint64_t fb = 0, fg = 0, fr = 0;
		unsigned mb = 0, mg = 0, mr = 0;
		for (size_t p = 0; p < (size_t) w * (size_t) h; p++) {
			const uint8_t b = bgra[p * 4], g = bgra[p * 4 + 1], r = bgra[p * 4 + 2];
			fb += b;
			fg += g;
			fr += r;
			if (b > mb)
				mb = b;
			if (g > mg)
				mg = g;
			if (r > mr)
				mr = r;
		}
		const uint64_t fn = (uint64_t) w * (uint64_t) h;
		const bool black = mb < 30 && mg < 30 && mr < 30;
		blog(LOG_INFO,
		     "uvc: 像素探针 帧#%llu 中心均值 B=%llu G=%llu R=%llu | 全帧均值 B=%llu G=%llu R=%llu "
		     "全帧峰值 B=%u G=%u R=%u%s（showing=%d active=%d）",
		     (unsigned long long) (s->frames_out + 1), (unsigned long long) (sb / n),
		     (unsigned long long) (sg / n), (unsigned long long) (sr / n),
		     (unsigned long long) (fb / fn), (unsigned long long) (fg / fn),
		     (unsigned long long) (fr / fn), mb, mg, mr,
		     black ? " —— 全帧黑：采集卡出的就是黑场，查 HDMI 源/卡" : " —— 有内容",
		     obs_source_showing(s->source) ? 1 : 0, obs_source_active(s->source) ? 1 : 0);
	}
}

static void push_frame_yuv(struct uvc_source *s, const struct uvc_mode *m, const uint8_t *buf, uint32_t len)
{
	struct obs_source_frame out = {0};
	out.width = m->w;
	out.height = m->h;
	out.timestamp = os_gettime_ns();

	if (m->kind == MODE_YUYV) {
		if (len < (uint32_t) m->w * (uint32_t) m->h * 2) {
			s->frames_dropped++;
			return;
		}
		out.format = VIDEO_FORMAT_YUY2;
		out.data[0] = (uint8_t *) buf;
		out.linesize[0] = (uint32_t) m->w * 2;
	} else { /* NV12 */
		if (len < (uint32_t) m->w * (uint32_t) m->h * 3 / 2) {
			s->frames_dropped++;
			return;
		}
		out.format = VIDEO_FORMAT_NV12;
		out.data[0] = (uint8_t *) buf;
		out.linesize[0] = m->w;
		out.data[1] = (uint8_t *) buf + (size_t) m->w * (size_t) m->h;
		out.linesize[1] = m->w;
	}
	/* UVC 未压缩载荷按 limited range(16-235) 处理 */
	out.full_range = false;
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL, out.format,
					       out.color_matrix, out.color_range_min, out.color_range_max);
	obs_source_output_video(s->source, &out);
}

/* ---------------- bulk 字节流解析（同步锁定 + 帧重组） ---------------- */

static void sbuf_consume(struct uvc_stream *st, size_t n)
{
	if (n >= st->sbuf_len) {
		st->sbuf_len = 0;
		return;
	}
	memmove(st->sbuf, st->sbuf + n, st->sbuf_len - n);
	st->sbuf_len -= n;
}

/* UVC 规范里头长只能是 2(基)+4(PTS)+6(SCR) 的组合：2/6/8/12，其余（比如 5）不可能。
 * 上一版就死在把 JPEG 熵数据里 HLE=5、周期 196 字节的巧合模式当成了真同步。 */
static bool hle_valid(uint8_t hle)
{
	return hle == 2 || hle == 6 || hle == 8 || hle == 12;
}

/* 循环内日志统一限频：每秒最多放行 1 行（日志 IO 拖慢 bulk 读 → 设备缓冲溢出 →
 * 更多 ERR 的死亡螺旋，真机第五轮实录 7 万行） */
static bool log_allow(uint64_t *slot)
{
	const uint64_t now = os_gettime_ns();
	if (now - *slot < 1000000000ULL)
		return false;
	*slot = now;
	return true;
}

/* 从 scan_off 起找下一个候选头起点：HLE 是同步时学会的那个（未同步时认白名单），
 * BMH bit7(EOH)=1、bit4(保留)=0。找不到返回 SIZE_MAX。只扫不判——候选是不是真的头，
 * 由状态机用 FID/PTS 一致性裁定（随机数据装出这两字节的概率约 1/1024，防不住，
 * 但撞 PTS 约 2^-32，帧内 PTS 一致性才是真闸门）。
 * 未同步找锚点时额外要求 EOF=0 且 ERR=0（真机假锚实录：0xA3 带 EOF、0xCC 带 ERR，
 * 都不是正常帧中头，不能拿来学头长）。 */
static size_t scan_candidate(const struct uvc_stream *st)
{
	for (size_t i = st->scan_off; i + 2 <= st->sbuf_len; i++) {
		const uint8_t b0 = st->sbuf[i];
		const uint8_t b1 = st->sbuf[i + 1];
		if (!(st->hle ? b0 == st->hle : hle_valid(b0)) || (b1 & 0x90) != 0x80)
			continue;
		if (!st->hle && (b1 & (UVC_BMH_ERR | UVC_BMH_EOF)) != 0)
			continue;
		return i;
	}
	return SIZE_MAX;
}

/* 送帧校验用：在帧尾 FRAME_TAIL_SEARCH 窗口里往回找 FFD9（允许尾部少量填充），
 * 返回 EOI 后一字节的偏移；找不到返回 0。 */
static size_t find_eoi_tail(const uint8_t *p, size_t len)
{
	const size_t start = len > FRAME_TAIL_SEARCH ? len - FRAME_TAIL_SEARCH : 0;
	for (size_t q = len - 1; q > start; q--) {
		if (p[q - 1] == 0xFF && p[q] == 0xD9)
			return q + 1;
	}
	return 0;
}

/* 诊断 dump：流位置前 64 字节 + 当前帧缓冲开头 32 字节（看帧首是不是 FF D8，
 * 一眼判断数据对齐没有）。每轮采集只打第一次——上一版用"连续失步 N 次"触发，
 * 但假同步场景下每次失步后都能再假锁、计数器清零，永远到不了 N。 */
static void dump_diag(struct uvc_stream *st)
{
	if (st->diag_dumped)
		return;
	st->diag_dumped = true;

	const size_t n = st->sbuf_len < 64 ? st->sbuf_len : 64;
	char line[64 * 3 + 1];
	line[0] = '\0'; /* n==0 时 loop 不跑，没这行 blog("%s") 读未初始化栈 */
	for (size_t i = 0; i < n; i++)
		snprintf(line + i * 3, 4, "%02x ", st->sbuf[i]);
	blog(LOG_WARNING, "uvc: 失步诊断，流位置前 %zu 字节：%s", n, line);

	const size_t fn = st->frame_len < 32 ? st->frame_len : 32;
	char fline[32 * 3 + 1];
	fline[0] = '\0';
	for (size_t i = 0; i < fn; i++)
		snprintf(fline + i * 3, 4, "%02x ", st->frame_buf[i]);
	blog(LOG_WARNING, "uvc: 失步诊断，帧缓冲已攒 %zu 字节，开头 %zu 字节：%s%s", st->frame_len, fn,
	     fline, fn >= 2 && st->frame_buf[0] == 0xFF && st->frame_buf[1] == 0xD8 ? "（帧首 FFD8 ✓）"
										  : "（帧首非 FFD8 ✗）");
}

/* 重同步：作废当前半帧、扫描游标归零，但坚持已学会的会话头长（真机锁到过
 * HLE=8/BMH=0xA3、0xCC 的假锚，回退白名单会再次假锁）；连假 5 次才连头长一起重学 */
static void stream_resync(struct uvc_stream *st, const char *why)
{
	st->resync_streak++;
	const bool relearn = st->resync_streak % 5 == 0;
	if (log_allow(&st->ll_resync))
		blog(LOG_WARNING, "uvc: 重同步（%s），丢弃半帧（已攒 %zu 字节）%s", why, st->frame_len,
		     relearn ? "——连假 5 次，头长也重学" : "");
	if (relearn)
		st->hle = 0;
	st->scan_off = 0;
	st->assembling = false;
	st->frame_len = 0;
	st->has_pts = false;
	st->rejected = 0;
	if (st->resync_streak == RESYNC_DUMP_AFTER)
		dump_diag(st);
}

/* 重新协商/抢救后重置解析状态：设备重开流，但头长是设备属性（这台 = 12），保留；
 * 新会话 st = {0} 天然 hle = 0 从头学 */
static void stream_reset_parse(struct uvc_stream *st)
{
	st->sbuf_len = 0;
	st->scan_off = 0;
	st->sync_read = 0;
	st->assembling = false;
	st->frame_len = 0;
	st->has_pts = false;
	st->rejected = 0;
	st->bad_streak = 0;
}

/* BMH ERR 处理（对齐 Linux uvcvideo：静默丢帧、全速继续读 drain，绝不逐条日志，
 * 也绝不做 SET_INTERFACE/重新协商等动作——廉价 MS2130 类固件会被反复 alt 切换
 * 踢死（真机 1617 实录：踢脚后 IN 路径全 -1 猝死），设备通常自己缓过来）。
 * 持续超 10 秒拆会话重开（新会话协商失败时有 reset_device 兜底）。
 * 返回 true = 调用方必须立刻 break 出流循环拆会话 */
static bool handle_err_header(struct uvc_source *s, struct uvc_stream *st)
{
	st->err_count++;
	const uint64_t now = os_gettime_ns();
	if (!st->err_first_ns)
		st->err_first_ns = now;
	const uint64_t dur = now - st->err_first_ns;

	if (log_allow(&st->ll_err))
		blog(LOG_WARNING,
		     "uvc: 设备报流错误（BMH ERR），静默丢帧全速读中（累计 %llu 次，本段持续 %.1f 秒）",
		     (unsigned long long) st->err_count, dur / 1e9);

	if (dur >= 10000000000ULL) {
		blog(LOG_ERROR, "uvc: BMH ERR 持续超 10 秒，拆会话重开");
		set_status(s, "设备持续报错（BMH ERR），重开会话…");
		return true;
	}
	return false;
}

/* ---------------- 一轮完整采集（拿 fd → detach → 协商 → bulk 流循环） ---------------- */

/* 异步 bulk 流水线：XFER_IN_FLIGHT 个传输始终在飞。回调只把完成的传输挂上完成
 * 队列——回调在采集线程自己的 libusb_handle_events_timeout() 里跑，整条流水线
 * 单线程，队列无需锁；解析/解码绝不在回调里做。错误只记账，由采集线程走猝死恢复链 */
struct uvc_xfer_pipe {
	struct libusb_transfer *done[XFER_IN_FLIGHT];
	int done_head;
	int done_tail;
	struct libusb_transfer *err[XFER_IN_FLIGHT]; /* 完成但状态非 COMPLETED 的（状态读 x->status） */
	int err_head;
	int err_tail;
	int error; /* 不可恢复：NO_DEVICE 或错误率持续超标（拆会话原因码），0 = 正常 */

	/* 瞬时错误容错（Linux uvcvideo 语义）：滑动窗口错误率 + 重提交统计 */
	uint64_t win_start_ns;   /* 当前 1 秒窗口起点 */
	unsigned win_errs;       /* 本窗口内瞬时错误数 */
	unsigned bad_seconds;    /* 连续错误率超标的秒数 */
	uint64_t total_resub;    /* 本会话累计重提交数 */
	uint64_t err_kind[8];    /* 按 LIBUSB_TRANSFER_* 状态分布（1=IO 2=TO 4=STALL 6=OVF） */
};

static void LIBUSB_CALL uvc_bulk_cb(struct libusb_transfer *x)
{
	struct uvc_xfer_pipe *p = x->user_data;
	if (x->status == LIBUSB_TRANSFER_COMPLETED) {
		/* 每个传输在队里最多出现一次，队列永远不会溢出 */
		p->done[p->done_tail++ % XFER_IN_FLIGHT] = x;
	} else if (x->status == LIBUSB_TRANSFER_NO_DEVICE) {
		if (!p->error)
			p->error = x->status; /* 真拔线：立即拆会话 */
	} else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
		/* 瞬时错误（IO/STALL/OVERFLOW/TIMED_OUT）：不拆会话，挂错误环，
		 * 主循环原地重提交（平板 xHCI 路径丢字节是环境因素，软件治不了根，
		 * 靠容错把流撑住——真机：帧少几十字节、部分帧频出） */
		p->err[p->err_tail++ % XFER_IN_FLIGHT] = x;
	}
}

static bool uvc_run_once(struct uvc_source *s, bool *perm_requested)
{
	struct uvc_dev d = {0};
	struct uvc_stream st = {0};
	struct obs_android_usb_device devs[8];
	uint8_t *bgra = NULL;
	size_t bgra_size = 0;
	tjhandle tj = NULL;
	bool streaming = false;
	bool progress = false;
	struct uvc_xfer_pipe pipe = {0};
	struct libusb_transfer *xfers[XFER_IN_FLIGHT] = {0};
	d.chosen = -1;

	/* ---- 1. 总线清单里找选中的设备 / 授权 ---- */
	if (!s->usb_device[0]) {
		set_status(s, "未选择设备（在属性页里选一台 USB 摄像头）");
		return false;
	}
	const int ndev = obs_android_usb_enum_devices(devs, 8);
	if (ndev < 0) {
		set_status(s, "USB 总线未接通（宿主 App 没注册 obs_android_set_usb_host）");
		return false;
	}

	/* 精确路径匹配优先；失配按 vid:pid（label 前缀，如 "2b89:5348"）认同一台设备
	 * —— 拔插后内核可能换个节点号也可能复用，路径字符串靠不住（真机第四轮） */
	int sel = -1;
	for (int i = 0; i < ndev; i++) {
		if (strcmp(devs[i].name, s->usb_device) == 0) {
			sel = i;
			break;
		}
	}
	if (sel < 0 && s->usb_id[0]) {
		for (int i = 0; i < ndev; i++) {
			if (strncmp(devs[i].label, s->usb_id, strlen(s->usb_id)) == 0) {
				blog(LOG_INFO, "uvc: 路径 '%s' 不在清单，按 %s 认出同一台设备换到了 '%s'",
				     s->usb_device, s->usb_id, devs[i].name);
				sel = i;
				break;
			}
		}
	}
	if (sel < 0) {
		set_status(s, "设备 '%s' 不在 USB 清单里（已拔出？）", s->usb_device);
		return false;
	}
	/* 记下这台设备的 vid:pid（label 第一个空格前的 token），拔插换号后靠它认回来 */
	{
		const char *sp = strchr(devs[sel].label, ' ');
		const size_t idlen = sp ? (size_t) (sp - devs[sel].label) : strlen(devs[sel].label);
		if (idlen > 0 && idlen < sizeof(s->usb_id))
			snprintf(s->usb_id, sizeof(s->usb_id), "%.*s", (int) idlen, devs[sel].label);
	}
	const char *open_name = devs[sel].name;

	/* 怪癖表：命中后先算"生效参数"。设置还是出厂默认（MJPEG 1080p60 未动过）时，
	 * 按怪癖改有效默认（这台卡仅 YUYV 出图，720p60 是平板实测稳定档；1080p60 YUYV
	 * ≈250MB/s 平板喂不饱）；用户显式改过任何一项都尊重他的选择 */
	d.quirks = quirks_for(s->usb_id);
	d.eff_fmt = s->prefer_fmt;
	d.eff_w = s->width;
	d.eff_h = s->height;
	d.eff_fps = s->fps;
	if ((d.quirks & QUIRK_PREFER_YUYV) && s->prefer_fmt == MODE_MJPEG && s->width == 1920 &&
	    s->height == 1080 && s->fps == 60) {
		d.eff_fmt = MODE_YUYV;
		d.eff_w = 1280;
		d.eff_h = 720;
		d.eff_fps = 60;
		blog(LOG_WARNING,
		     "uvc: 怪癖命中 %s（仅 YUYV 出图），出厂默认已按怪癖改为 YUYV %dx%d@%d（属性页显示不变，可手改）",
		     s->usb_id, d.eff_w, d.eff_h, d.eff_fps);
	}
	if (!devs[sel].has_permission) {
		set_status(s, "设备 '%s' 未授权", open_name);
		if (!*perm_requested) {
			const int pr = obs_android_usb_request_permission(open_name);
			blog(LOG_INFO, "uvc: 设备未授权，已发起授权请求（返回 %d：%s）", pr,
			     pr == 1 ? "弹窗中" : pr == 0 ? "其实已有权限" : "失败");
			*perm_requested = true;
		}
		return false;
	}
	*perm_requested = false;

	/* ---- 2. 借 fd，包 libusb（nikon-source 真机验证的唯一可行模式） ---- */
	d.bridge_fd = obs_android_usb_open(open_name);
	if (d.bridge_fd <= 0) {
		set_status(s, "打开设备失败（fd=%d）—— 设备被拔或授权刚被收回", d.bridge_fd);
		return false;
	}
	set_status(s, "正在初始化 USB/UVC 会话…");

	/* 必须关设备发现：默认初始化要建 NETLINK_KOBJECT_UEVENT socket，Android 的 SELinux
	 * 不让应用建，libusb_init 直接 -1（真机实测，旧 libuvc 实现就死在这） */
	const struct libusb_init_option opts[] = {{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}}};
	int r = libusb_init_context(&d.ctx, opts, 1);
	if (r != 0) {
		blog(LOG_ERROR, "uvc: libusb_init_context 失败 %d", r);
		set_status(s, "libusb 初始化失败 %d", r);
		goto out;
	}
	r = libusb_wrap_sys_device(d.ctx, (intptr_t) d.bridge_fd, &d.h);
	if (r != 0 || !d.h) {
		blog(LOG_ERROR, "uvc: libusb_wrap_sys_device(fd=%d) 失败 %d", d.bridge_fd, r);
		set_status(s, "包装 USB fd 失败（%d）", r);
		d.h = NULL;
		goto out;
	}
	{
		/* 链路速率打点：max_packet=1024 已侧面确认 SS，这里拿 libusb 的确证 */
		libusb_device *ld = libusb_get_device(d.h);
		const int sp = ld ? libusb_get_device_speed(ld) : -1;
		blog(LOG_INFO, "uvc: 链路速率 speed=%d（%s）", sp,
		     sp == LIBUSB_SPEED_SUPER	     ? "SuperSpeed 5Gbps"
		     : sp == LIBUSB_SPEED_SUPER_PLUS ? "SuperSpeed+ 10Gbps"
		     : sp == LIBUSB_SPEED_HIGH	     ? "HighSpeed 480Mbps——不是 SS，带宽够呛"
		     : sp == LIBUSB_SPEED_FULL	     ? "FullSpeed 12Mbps"
		     : sp == LIBUSB_SPEED_LOW	     ? "LowSpeed 1.5Mbps"
						     : "未知");
	}

	/* ---- 3. 描述符扫描 + 模式选择 ---- */
	if (!uvc_scan_device(&d)) {
		set_status(s, "设备不是受支持的 UVC 设备（无 bulk 流接口）");
		goto out;
	}
	d.chosen = choose_mode(&d, s);
	if (d.chosen < 0) {
		set_status(s, "设备没报任何可用模式");
		goto out;
	}
	{
		const struct uvc_mode *m = &d.modes[d.chosen];
		blog(LOG_INFO,
		     "uvc: 选中模式 %s fmt=%u frame=%u %ux%u（设置：video_format='%s' %dx%d@%d，kind=%d）",
		     mode_kind_name(m->kind), (unsigned) m->fmt_index, (unsigned) m->frame_index,
		     (unsigned) m->w, (unsigned) m->h, s->fmt_setting, s->width, s->height, s->fps,
		     s->prefer_fmt);
	}

	/* ---- 4. 抢设备：detach 内核 uvcvideo 驱动，再 claim VS 接口 ----
	 * 平板内核会把采集卡绑到 uvcvideo（/dev/videoN），不 detach 的话 claim 撞 -6(BUSY)。
	 * 本机 ROM 实测放行 USBDEVFS_DISCONNECT，但仍按"尽力而为 + 清晰日志"写。 */
	r = libusb_set_auto_detach_kernel_driver(d.h, 1);
	blog(LOG_INFO, "uvc: set_auto_detach_kernel_driver r=%d%s", r,
	     r == 0 ? "" : "（不支持/失败，claim 时看 libusb 自己怎么处理）");
	if (d.vc_iface >= 0) {
		const int kd = libusb_kernel_driver_active(d.h, d.vc_iface);
		const int dr = libusb_detach_kernel_driver(d.h, d.vc_iface);
		blog(LOG_INFO, "uvc: VC 接口 %d 内核驱动=%d，显式 detach r=%d", d.vc_iface, kd, dr);
	}
	{
		const int kd = libusb_kernel_driver_active(d.h, d.vs_iface);
		const int dr = libusb_detach_kernel_driver(d.h, d.vs_iface);
		blog(LOG_INFO, "uvc: VS 接口 %d 内核驱动=%d，显式 detach r=%d", d.vs_iface, kd, dr);
	}

	r = libusb_claim_interface(d.h, d.vs_iface);
	if (r != 0) {
		blog(LOG_ERROR, "uvc: claim VS 接口 %d 失败 %d（-6=BUSY：内核 uvcvideo 还占着；"
				"-5=NOT_FOUND：设备掉了）",
		     d.vs_iface, r);
		set_status(s, "占用 USB 接口失败（%d）—— 内核 uvcvideo 驱动可能占着设备", r);
		goto out;
	}
	progress = true; /* 能 claim 上就算赢了一半（内核驱动 detach 成功） */

	/* ---- 4.5 开流前置 SET_INTERFACE（一次性正常开流步骤，不是 ERR 期的反复
	 * 踢脚——那个保持删除）：Windows UVC 驱动每次开流都切到工作 alt，有的固件
	 * 没这一步就只出黑场。先 GET_INTERFACE 打当前 alt，再 SET 并打返回值 */
	{
		uint8_t cur_alt = 0xFF;
		const int gr = libusb_control_transfer(d.h, 0x81, LIBUSB_REQUEST_GET_INTERFACE, 0,
						       (uint16_t) d.vs_iface, &cur_alt, 1, 1000);
		blog(LOG_INFO, "uvc: GET_INTERFACE r=%d 当前 alt=%u（工作 alt=%d）", gr,
		     gr == 1 ? (unsigned) cur_alt : 0xFFU, d.vs_alt);
		const int si = libusb_set_interface_alt_setting(d.h, d.vs_iface, d.vs_alt);
		blog(LOG_INFO, "uvc: 开流前置 SET_INTERFACE alt%d r=%d", d.vs_alt, si);
	}

	/* ---- 5. probe/commit 协商 ---- */
	r = uvc_negotiate(&d, s);
	if (r == LIBUSB_ERROR_IO) {
		/* GET probe 首轮就 -1 = IN 路径疑似猝死（OUT/描述符仍通的老毛病）：
		 * 对新开的句柄先 USBDEVFS_RESET 再重试协商——ROM 若放行，
		 * "猝死"无需物理拔插即可自愈。打 reset 返回值以便真机确认放行与否 */
		const int rr = libusb_reset_device(d.h);
		blog(LOG_WARNING, "uvc: 协商首轮 GET -1（IN 路径疑似猝死），reset_device r=%d，重试协商", rr);
		/* reset 后内核 uvcvideo 可能重新绑上，重新 detach + claim（原 claim 若随
		 * reset 保留，这里拿 -6/BUSY，只记日志，句柄状态两种结局都对） */
		if (d.vc_iface >= 0)
			libusb_detach_kernel_driver(d.h, d.vc_iface);
		libusb_detach_kernel_driver(d.h, d.vs_iface);
		const int cr = libusb_claim_interface(d.h, d.vs_iface);
		if (cr != 0)
			blog(LOG_WARNING, "uvc: reset 后重新 claim VS 接口 r=%d（-6=原 claim 仍在，无碍）", cr);
		r = uvc_negotiate(&d, s);
		blog(LOG_WARNING, "uvc: reset 后的重试协商结果 %d", r);
	}
	if (r != 0) {
		set_status(s, "UVC 协商失败（%d）", r);
		goto out;
	}

	/* ---- 6. 缓冲 ---- */
	const struct uvc_mode *mode = &d.modes[d.chosen];
	size_t frame_cap;
	if (mode->kind == MODE_MJPEG) {
		/* 读回值可能是脏值（这台读回过 16384），取"读回 / 描述符 / 1MB 下限"三者最大 */
		frame_cap = d.frame_buf_size;
		if (frame_cap < mode->max_frame_size)
			frame_cap = mode->max_frame_size;
		if (frame_cap < 1024 * 1024)
			frame_cap = 1024 * 1024;
		if (frame_cap > MAX_FRAME_BYTES)
			frame_cap = MAX_FRAME_BYTES;
	} else {
		frame_cap = raw_frame_bytes(mode); /* 未压缩帧长是死的 */
	}
	st.frame_cap = frame_cap;
	st.frame_total = raw_frame_bytes(mode); /* MJPEG = 0 */
	st.frame_buf = bmalloc(st.frame_cap);

	st.sbuf_cap = SBUF_HIGH_WATER + BULK_CHUNK_SIZE + 64; /* 高水位后仍留得下整个读块 */
	st.sbuf = bmalloc(st.sbuf_cap);

	if (!st.frame_buf || !st.sbuf) {
		blog(LOG_ERROR, "uvc: 缓冲分配失败（帧 %zu / 流 %zu 字节）", st.frame_cap, st.sbuf_cap);
		set_status(s, "内存不足，缓冲分配失败");
		goto out;
	}

	if (mode->kind == MODE_MJPEG) {
		tj = tjInitDecompress();
		if (!tj) {
			blog(LOG_ERROR, "uvc: tjInitDecompress 失败：%s", tjGetErrorStr());
			set_status(s, "JPEG 解码器初始化失败");
			goto out;
		}
	}

	/* ---- 6.5 异步流水线发车：XFER_IN_FLIGHT 个 256KB 读一次性全挂上，从此总线上
	 * 永远有读在飞；每完成一个，取走数据后立刻重新挂上（见流循环） */
	for (int i = 0; i < XFER_IN_FLIGHT; i++) {
		xfers[i] = libusb_alloc_transfer(0);
		uint8_t *buf = xfers[i] ? bmalloc(BULK_CHUNK_SIZE) : NULL;
		if (!xfers[i] || !buf) {
			blog(LOG_ERROR, "uvc: 分配 bulk 传输 %d/%d 失败", i, XFER_IN_FLIGHT);
			set_status(s, "内存不足，USB 传输分配失败");
			bfree(buf);
			if (xfers[i])
				libusb_free_transfer(xfers[i]);
			xfers[i] = NULL;
			goto out;
		}
		libusb_fill_bulk_transfer(xfers[i], d.h, d.ep_in, buf, BULK_CHUNK_SIZE, uvc_bulk_cb, &pipe,
					  0 /* 无超时：断流由停滞看门狗判，不靠传输超时 */);
		const int sr = libusb_submit_transfer(xfers[i]);
		if (sr != 0) {
			blog(LOG_ERROR, "uvc: 提交 bulk 传输 %d/%d 失败 %d，放弃本轮", i, XFER_IN_FLIGHT, sr);
			set_status(s, "USB 异步读提交失败（%d）", sr);
			goto out;
		}
	}

	streaming = true;
	set_status(s, "采集中：%s %ux%u", mode_kind_name(mode->kind), (unsigned) mode->w, (unsigned) mode->h);
	blog(LOG_INFO, "uvc: 进入 bulk 流循环（端点 0x%02X，%d 个 %uKB 读并发在飞，帧缓冲 %zu 字节）", d.ep_in,
	     XFER_IN_FLIGHT, (unsigned) (BULK_CHUNK_SIZE / 1024), st.frame_cap);

	/* ---- 7. bulk 流循环：攒流 → 同步锁定 → 按跨距走 payload 重组帧 ---- */
	uint64_t fps_window_start = os_gettime_ns();
	uint64_t fps_window_frames = 0;
	unsigned stall_reads = 0; /* 连续无数据读的次数（1 次 ≈ 1 秒 bulk 读超时） */
	bool first_read_logged = false;
	bool abort_stream = false;    /* BMH ERR 超 10 秒：解析循环置位，出循环后拆会话 */
	uint64_t last_frame_ns = 0;   /* 最近一次送出完整帧的时间（冻结看门狗，0 = 还没送过） */
	unsigned freeze_retries = 0;  /* 冻结看门狗已连续重协商次数 */
	uint64_t black_win_start = 0; /* 黑场自降级：窗口起点（0 = 未开始） */
	unsigned black_checked = 0;   /* 窗口内已检帧数 */
	unsigned black_frames = 0;    /* 窗口内全黑帧数 */
	bool yuyv_fallback_done = false; /* 黑场自降级一次性标志（含"有内容，收工"） */

	while (!os_atomic_load_long(&s->shutting_down)) {
		/* 传输级不可恢复错误（-4/NO_DEVICE 真拔线，或下方错误率超标升级）：
		 * 走猝死恢复链，拆会话重枚举 */
		if (pipe.error) {
			blog(LOG_ERROR, "uvc: bulk 传输状态 %d —— %s，拆会话重枚举", pipe.error,
			     pipe.error == LIBUSB_TRANSFER_NO_DEVICE ? "设备已断开（拔线？）" : "IN 路径异常");
			set_status(s, "设备已断开，等待重新插入…");
			break;
		}

		/* 瞬时错误原地重提交（Linux uvcvideo 语义）：STALL 先 clear_halt，
		 * 其余直接挂回总线；数据缺口由解析器的丢帧/重同步自己消化 */
		while (pipe.err_head < pipe.err_tail) {
			struct libusb_transfer *e = pipe.err[pipe.err_head++ % XFER_IN_FLIGHT];
			const int es = e->status;
			if (es >= 0 && es < 8)
				pipe.err_kind[es]++;
			pipe.win_errs++;
			pipe.total_resub++;
			if (es == LIBUSB_TRANSFER_STALL)
				libusb_clear_halt(d.h, d.ep_in);
			const int rr = libusb_submit_transfer(e);
			if (rr != 0) {
				blog(LOG_ERROR, "uvc: 错误传输重提交失败 %d（原状态 %d），拆会话", rr, es);
				pipe.error = es > 0 ? es : LIBUSB_TRANSFER_ERROR;
				break;
			}
		}
		if (pipe.error)
			continue; /* 回循环顶走拆会话分支 */
		if (pipe.err_head == pipe.err_tail)
			pipe.err_head = pipe.err_tail = 0; /* 队列空了顺手归零，游标不长跑 */

		/* 滑动窗口错误率：每秒结算一次，有重提交就打 1 行汇总（限频天然 1/s）；
		 * 连续 XFER_ERR_BAD_SECONDS 秒每秒超 XFER_ERR_RATE_LIMIT 才升级拆会话 */
		{
			const uint64_t now_ns = os_gettime_ns();
			if (now_ns - pipe.win_start_ns >= 1000000000ULL) {
				if (pipe.win_errs > XFER_ERR_RATE_LIMIT) {
					pipe.bad_seconds++;
					if (pipe.bad_seconds >= XFER_ERR_BAD_SECONDS) {
						blog(LOG_ERROR,
						     "uvc: 传输错误率连续 %u 秒超标（最近一秒 %u 个 > %u），升级拆会话",
						     pipe.bad_seconds, pipe.win_errs, XFER_ERR_RATE_LIMIT);
						pipe.error = LIBUSB_TRANSFER_ERROR;
						continue;
					}
				} else {
					pipe.bad_seconds = 0;
				}
				if (pipe.win_errs > 0)
					blog(LOG_WARNING,
					     "uvc: 传输瞬时错误重提交 %u 次/秒（IO=%llu TO=%llu STALL=%llu OVF=%llu，累计 %llu）",
					     pipe.win_errs, (unsigned long long) pipe.err_kind[1],
					     (unsigned long long) pipe.err_kind[2],
					     (unsigned long long) pipe.err_kind[4],
					     (unsigned long long) pipe.err_kind[6],
					     (unsigned long long) pipe.total_resub);
				pipe.win_errs = 0;
				pipe.win_start_ns = now_ns;
			}
		}

		/* 冻结看门狗：连续 FREEZE_WATCHDOG_NS 没送出任何完整帧（数据来不来都算）
		 * → 重新协商重进流；连续 FREEZE_MAX_RETRY 次无效 → 拆会话走重开链。
		 * 真机：1080p60 YUYV 喂不饱冻死，保帧机制把"流死了"显示成"稳定" */
		{
			const uint64_t fn = os_gettime_ns();
			if (!last_frame_ns) {
				last_frame_ns = fn; /* 首帧还没来：给一段起动时间 */
			} else if (fn - last_frame_ns >= FREEZE_WATCHDOG_NS) {
				if (freeze_retries >= FREEZE_MAX_RETRY) {
					blog(LOG_ERROR, "uvc: 冻结看门狗：连续 %u 次重协商仍无完整帧，拆会话重开",
					     FREEZE_MAX_RETRY);
					break;
				}
				freeze_retries++;
				blog(LOG_WARNING, "uvc: 冻结看门狗：%u 秒无完整帧（第 %u/%u 次），重新协商重进流",
				     (unsigned) (FREEZE_WATCHDOG_NS / 1000000000ULL), freeze_retries,
				     FREEZE_MAX_RETRY);
				const int nr = uvc_negotiate(&d, s);
				blog(LOG_WARNING, "uvc: 冻结后的重新协商结果 %d，解析状态已重置", nr);
				stream_reset_parse(&st);
				last_frame_ns = fn; /* 再给一段 */
			}
		}

		/* 取一个已完成传输；没货就泵事件 1 秒（完成回调就在本线程的
		 * handle_events_timeout 里跑，整条流水线单线程、无锁） */
		struct libusb_transfer *x = NULL;
		if (pipe.done_head < pipe.done_tail) {
			x = pipe.done[pipe.done_head++ % XFER_IN_FLIGHT];
		} else {
			pipe.done_head = pipe.done_tail = 0; /* 队列空了顺手归零，游标不长跑 */
			struct timeval tv = {BULK_TIMEOUT_MS / 1000, (BULK_TIMEOUT_MS % 1000) * 1000};
			libusb_handle_events_timeout(d.ctx, &tv);
			if (pipe.done_head < pipe.done_tail)
				continue; /* 来货了，下轮取 */
			/* 1 秒无任何完成 = 断流/拔线：逐级升级抢救（clear_halt → 重新协商 →
			 * reset_device → 拆会话重开），每步留日志 */
			if (++stall_reads == STALL_CLEAR_HALT) {
				blog(LOG_WARNING, "uvc: 流停滞约 %u 秒，clear_halt 试试", (unsigned) STALL_CLEAR_HALT);
				libusb_clear_halt(d.h, d.ep_in);
			} else if (stall_reads == STALL_RENEGOTIATE) {
				blog(LOG_WARNING, "uvc: 流停滞约 %u 秒，重新协商一次", (unsigned) STALL_RENEGOTIATE);
				const int nr = uvc_negotiate(&d, s);
				blog(LOG_WARNING, "uvc: 停滞后的重新协商结果 %d，解析状态已重置", nr);
				stream_reset_parse(&st);
			} else if (stall_reads == STALL_RESET_DEVICE) {
				const int rr = libusb_reset_device(d.h);
				blog(LOG_WARNING, "uvc: 流停滞约 %u 秒，reset_device r=%d", (unsigned) STALL_RESET_DEVICE,
				     rr);
			} else if (stall_reads >= STALL_TEARDOWN) {
				blog(LOG_ERROR, "uvc: 流停滞约 %u 秒，拆会话完整重开（关 fd → 重枚举 → 重开）",
				     (unsigned) STALL_TEARDOWN);
				break;
			}
			continue;
		}

		if (!first_read_logged) {
			/* 崩溃定位面包屑：断在这行之前 = 传输提交/事件泵；之后、"同步建立"
			 * 之前 = 解析；再往后 = 解码 */
			first_read_logged = true;
			blog(LOG_INFO, "uvc: 首个传输完成 actual=%d（%d 个并发在飞）", x->actual_length,
			     XFER_IN_FLIGHT);
		}
		stall_reads = 0;

		/* 数据追进流缓冲（高水位不变式保证 len ≤ cap - 读块，兜底防御仍留着），
		 * 然后立刻把传输挂回总线——FIFO 不溢出靠的就是读始终在飞 */
		const int got = x->actual_length;
		if (got > 0) {
			if (st.sbuf_len + (size_t) got > st.sbuf_cap) {
				blog(LOG_WARNING, "uvc: 流缓冲将溢出（%zu + %d > %zu），丢前半兜底", st.sbuf_len,
				     got, st.sbuf_cap);
				sbuf_consume(&st, st.sbuf_len / 2);
				st.scan_off = 0;
				st.assembling = false;
				st.frame_len = 0;
			}
			memcpy(st.sbuf + st.sbuf_len, x->buffer, (size_t) got);
			st.sbuf_len += (size_t) got;
			st.sync_read += (uint64_t) got;
		}
		const int sr = libusb_submit_transfer(x);
		if (sr != 0) {
			blog(LOG_ERROR, "uvc: 重新提交 bulk 传输失败 %d，拆会话", sr);
			break;
		}

		/* 纯字节扫描状态机：payload 不定长，两个头之间的字节全归前一个头。
		 * 帧边界只信 EOF（这台设备用空 payload 带 EOF 位收尾每帧，hex dump 实锤）；
		 * 误同步防护 = 帧内 FID 一致 + PTS 一致（随机数据撞 PTS 约 2^-32） */
		if (st.sbuf_len >= SBUF_HIGH_WATER) {
			if (log_allow(&st.ll_highwater))
				blog(LOG_WARNING, "uvc: 流缓冲到高水位（%zu 字节）还定不了下一个头，丢前半重扫",
				     st.sbuf_len);
			dump_diag(&st);
			sbuf_consume(&st, st.sbuf_len / 2);
			st.scan_off = 0;
			st.assembling = false;
			st.frame_len = 0;
		}

		for (;;) {
			const size_t H = scan_candidate(&st);
			if (H == SIZE_MAX) {
				/* 没有候选：assembling 时这些字节可能是数据（边界未定，先攒着）；
				 * 末尾 1 字节留着——它可能是跨读块的头起点 */
				st.scan_off = st.sbuf_len > 0 ? st.sbuf_len - 1 : 0;
				break;
			}
			const uint8_t hle = st.hle ? st.hle : st.sbuf[H];
			if (H + hle > st.sbuf_len) {
				st.scan_off = H; /* 头可能跨读块，留存待下块拼接 */
				break;
			}

			const uint8_t bmh = st.sbuf[H + 1];
			const uint8_t fid = (uint8_t) (bmh & UVC_BMH_FID);
			const bool eof = (bmh & UVC_BMH_EOF) != 0;
			const bool err = (bmh & UVC_BMH_ERR) != 0;
			const bool has_pts_field = (bmh & 0x04) != 0 && hle >= 6;
			const uint32_t pts = has_pts_field ? get_u32le(st.sbuf + H + 2) : 0;

			if (!st.hle) {
				/* 首个合法头即同步（没有"找不到就判不兼容"——继续扫就是） */
				st.hle = hle;
				blog(LOG_INFO,
				     "uvc: 同步建立：首个合法头在偏移 %zu（HLE=%u BMH=0x%02X），前面 %zu 字节垃圾已丢",
				     H, (unsigned) hle, (unsigned) bmh, H);
				st.sync_read = 0;
				sbuf_consume(&st, H);
				st.scan_off = 0;
				continue;
			}

			if (!st.assembling) {
				/* 期待帧头：EOF 头是上一帧的收尾标记（重同步后常见），跳过；
				 * 其余合法候选开新帧。帧间隙的字节是垃圾，随前缀一起丢 */
				sbuf_consume(&st, H + hle);
				st.scan_off = 0;
				if (eof)
					continue;
				if (err) {
					s->frames_dropped++;
					if (handle_err_header(s, &st)) {
						abort_stream = true;
						break;
					}
					continue;
				}
				st.assembling = true;
				st.frame_len = 0;
				st.last_fid = fid;
				st.has_pts = has_pts_field;
				st.frame_pts = pts;
				st.rejected = 0;
				st.err_first_ns = 0; /* 正常头来了：本段 ERR 算过去了 */
				continue;
			}

			if (err) {
				s->frames_dropped++;
				st.assembling = false;
				st.frame_len = 0;
				sbuf_consume(&st, H + hle);
				st.scan_off = 0;
				if (handle_err_header(s, &st)) {
					abort_stream = true;
					break;
				}
				continue;
			}

			/* 帧内关系：FID 一致 且（双方都有 PTS 时）PTS 相等 = 同帧 payload 边界。
			 * FID 翻 / PTS 变 = "边界声称"——帧边界只信 EOF，拒；连续拒到上限
			 * 说明 EOF 真丢了（丢包），重同步 */
			const bool same_frame =
				fid == st.last_fid && (!has_pts_field || !st.has_pts || pts == st.frame_pts);
			if (!same_frame) {
				if (++st.rejected >= REJECT_RESYNC) {
					stream_resync(&st, "边界声称连拒到上限，EOF 应该丢了");
					continue;
				}
				st.scan_off = H + 1; /* 伪头，这些字节是数据，继续往后扫 */
				continue;
			}
			st.rejected = 0;
			st.err_first_ns = 0; /* 同帧正常头：本段 ERR 算过去了 */
			if (has_pts_field && !st.has_pts) {
				st.has_pts = true;
				st.frame_pts = pts;
			}

			/* H 前的字节全是本帧数据 */
			if (st.frame_len + H > st.frame_cap) {
				if (log_allow(&st.ll_overflow))
					blog(LOG_WARNING, "uvc: 帧超过缓冲（%zu + %zu > %zu），丢弃本帧", st.frame_len, H,
					     st.frame_cap);
				s->frames_dropped++;
				st.assembling = false;
				st.frame_len = 0;
				sbuf_consume(&st, H + hle);
				st.scan_off = 0;
				continue;
			}
			/* MJPEG 帧首早检：首块数据不以 FFD8 开头 = 假帧头/丢了帧首，
			 * 立刻回期待态（比攒到帧尾再丢省得多） */
			if (st.frame_len == 0 && H >= 2 && mode->kind == MODE_MJPEG &&
			    (st.sbuf[0] != 0xFF || st.sbuf[1] != 0xD8)) {
				st.assembling = false;
				sbuf_consume(&st, H + hle);
				st.scan_off = 0;
				continue;
			}
			memcpy(st.frame_buf + st.frame_len, st.sbuf, H);
			st.frame_len += H;
			sbuf_consume(&st, H + hle);
			st.scan_off = 0;

			if (!eof)
				continue;

			/* ---- 一帧齐了（EOF：无论这个 payload 带不带数据，0 字节也合法） ---- */
			st.assembling = false;
			uint32_t flen = (uint32_t) st.frame_len;
			st.frame_len = 0;

			st.frames_done++;
			if (st.frames_done <= 10 || st.frames_done % 300 == 0)
				blog(LOG_INFO, "uvc: 帧完成 #%llu %u 字节，首 2 字节=%02X %02X%s",
				     (unsigned long long) st.frames_done, (unsigned) flen,
				     flen > 0 ? (unsigned) st.frame_buf[0] : 0U,
				     flen > 1 ? (unsigned) st.frame_buf[1] : 0U,
				     mode->kind == MODE_MJPEG ? "（MJPEG 应为 FF D8）" : "");

			/* 空帧（stray EOF 产物，flen == 0）：不算坏帧，不碰 bad_streak，直接跳过 */
			if (flen == 0) {
				s->frames_dropped++;
				if (log_allow(&st.ll_badframe))
					blog(LOG_INFO, "uvc: 空帧跳过（stray EOF 产物）");
				continue;
			}

			/* 送帧校验：MJPEG 必须 FFD8 开头、FFD9 在尾部填充窗内（找到就截断）；
			 * 未压缩必须够定长（多了截断）。不满足按坏帧丢，连续坏帧到上限重同步 */
			if (mode->kind == MODE_MJPEG) {
				const size_t eoi = flen >= 4 && st.frame_buf[0] == 0xFF &&
								   st.frame_buf[1] == 0xD8
							   ? find_eoi_tail(st.frame_buf, flen)
							   : 0;
				if (eoi == 0) {
					s->frames_dropped++;
					if (log_allow(&st.ll_badframe))
						blog(LOG_WARNING, "uvc: 坏帧丢弃（%u 字节，%s）", flen,
						     flen >= 2 && st.frame_buf[0] == 0xFF && st.frame_buf[1] == 0xD8
							     ? "帧尾找不到 FFD9"
							     : "帧首非 FFD8");
					if (++st.bad_streak >= BADFRAME_RESYNC) {
						stream_resync(&st, "连续坏帧到上限");
						st.bad_streak = 0;
					}
					continue;
				}
				flen = (uint32_t) eoi;
			} else {
				if (flen < st.frame_total) {
					s->frames_dropped++;
					if (log_allow(&st.ll_badframe))
						blog(LOG_WARNING, "uvc: 坏帧丢弃（%u 字节 < 定长 %u）", flen,
						     (unsigned) st.frame_total);
					if (++st.bad_streak >= BADFRAME_RESYNC) {
						stream_resync(&st, "连续坏帧到上限");
						st.bad_streak = 0;
					}
					continue;
				}
				flen = st.frame_total;
			}
			st.bad_streak = 0;
			st.resync_streak = 0; /* 出好帧了：重同步连击清零 */

			if (mode->kind == MODE_MJPEG) {
				int jw = 0, jh = 0, jsub = 0, jcs = 0;
				if (tjDecompressHeader3(tj, st.frame_buf, flen, &jw, &jh, &jsub, &jcs) != 0 ||
				    jw <= 0 || jh <= 0) {
					s->frames_dropped++;
					if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
						blog(LOG_WARNING, "uvc: MJPEG 头解析失败（%u 字节）：%s",
						     (unsigned) flen, tjGetErrorStr());
					continue;
				}
				/* 一次性诊断：首帧头解析结果（黑帧排查实锤过——解码忠实的
				 * 前提下，这组值用于确认卡给的是什么格式） */
				if (s->frames_out == 0)
					blog(LOG_INFO, "uvc: JPEG 头 %dx%d subsamp=%s(%d) colorspace=%s(%d)，载荷 %u 字节",
					     jw, jh, tj_subsamp_name(jsub), jsub, tj_cs_name(jcs), jcs,
					     (unsigned) flen);
				/* jw/jh 来自不可信的 JPEG 头：坏帧能给个 65535x65535（≈17 GB），
				 * 分配失败后 tjDecompress2 往 NULL 写就是段错误（1459 疑案头号嫌犯） */
				const size_t need = (size_t) jw * (size_t) jh * 4;
				if (jw > 8192 || jh > 8192 || need > MAX_FRAME_BYTES * 4) {
					s->frames_dropped++;
					if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
						blog(LOG_WARNING, "uvc: MJPEG 头尺寸 %dx%d 离谱，丢帧", jw, jh);
					continue;
				}
				if (need > bgra_size) {
					bfree(bgra);
					bgra = bmalloc(need);
					bgra_size = bgra ? need : 0;
				}
				if (!bgra) {
					s->frames_dropped++;
					blog(LOG_WARNING, "uvc: BGRA 缓冲分配失败（%zu 字节），丢帧", need);
					continue;
				}
				if (tjDecompress2(tj, st.frame_buf, flen, bgra, jw, jw * 4, jh, TJPF_BGRA,
						  0) != 0) {
					s->frames_dropped++;
					if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
						blog(LOG_WARNING, "uvc: MJPEG 解码失败：%s", tjGetErrorStr());
					continue;
				}
				if (s->frames_out == 0)
					blog(LOG_INFO, "uvc: 首帧 tjDecompress2 返回 0（%dx%d → BGRA %zu 字节）", jw,
					     jh, need);
				push_frame_bgra(s, bgra, jw, jh);
				s->cur_w = (uint32_t) jw;
				s->cur_h = (uint32_t) jh;
			} else {
				push_frame_yuv(s, mode, st.frame_buf, flen);
				s->cur_w = mode->w;
				s->cur_h = mode->h;
			}

			s->frames_out++;
			fps_window_frames++;
			last_frame_ns = os_gettime_ns();
			freeze_retries = 0;

			/* 黑场自降级看门狗（通用，一次性）：流起 3 秒推的全是黑帧（全帧峰值
			 * <30）且当前不是 YUYV → 自动改协商同分辨率 YUYV 重进流；
			 * 切了还黑 / 窗口内有内容，都收工不再横跳 */
			if (!yuyv_fallback_done && mode->kind != MODE_YUYV) {
				const uint8_t *fb = mode->kind == MODE_MJPEG ? bgra : st.frame_buf;
				const size_t fl = mode->kind == MODE_MJPEG ? (size_t) s->cur_w * s->cur_h * 4
					: mode->kind == MODE_NV12	   ? (size_t) mode->w * mode->h /* 只看 Y 平面 */
									   : (size_t) mode->w * mode->h * 2;
				if (!black_win_start)
					black_win_start = last_frame_ns;
				black_checked++;
				if (frame_is_black(fb, fl, mode->kind == MODE_MJPEG ? 4 : 1))
					black_frames++;
				if (last_frame_ns - black_win_start >= 3000000000ULL) {
					yuyv_fallback_done = true;
					if (black_frames == black_checked && black_checked >= 10) {
						int yi = -1;
						for (int i = 0; i < d.n_modes; i++) {
							if (d.modes[i].kind == MODE_YUYV &&
							    d.modes[i].w == mode->w && d.modes[i].h == mode->h) {
								yi = i;
								break;
							}
						}
						if (yi < 0) {
							blog(LOG_WARNING,
							     "uvc: 检测到黑场输出（3 秒 %u 帧全黑），但设备没有同分辨率 YUYV 档，无法自降级",
							     black_checked);
						} else {
							blog(LOG_WARNING,
							     "uvc: 检测到黑场输出（3 秒 %u 帧全黑），自动切换 %s → YUYV 并重新协商",
							     black_checked, mode_kind_name(mode->kind));
							d.chosen = yi;
							mode = &d.modes[yi];
							st.frame_total = raw_frame_bytes(mode);
							if (st.frame_total > st.frame_cap) {
								bfree(st.frame_buf);
								st.frame_cap = st.frame_total;
								st.frame_buf = bmalloc(st.frame_cap);
							}
							if (!st.frame_buf) {
								blog(LOG_ERROR, "uvc: 切 YUYV 重配帧缓冲失败（%zu 字节）",
								     st.frame_cap);
								abort_stream = true;
								break;
							}
							const int nr = uvc_negotiate(&d, s);
							blog(LOG_WARNING,
							     "uvc: 黑场自降级的重新协商结果 %d，解析状态已重置", nr);
							stream_reset_parse(&st);
							if (nr != 0) {
								abort_stream = true;
								break;
							}
							continue;
						}
					}
				}
			}

			const uint64_t now = os_gettime_ns();
			if (now - fps_window_start >= 1000000000ULL) {
				s->cur_fps_x100 = (uint32_t) (fps_window_frames * 100000000000ULL /
							      (now - fps_window_start));
				fps_window_start = now;
				fps_window_frames = 0;
				set_status(s, "采集中：%s %ux%u @ %.2f fps", mode_kind_name(mode->kind),
					   (unsigned) s->cur_w, (unsigned) s->cur_h, s->cur_fps_x100 / 100.0);
			}
			if (s->frames_out == 1 || s->frames_out % 300 == 0)
				blog(LOG_INFO, "uvc: 第 %llu 帧 %ux%u %s（载荷 %u 字节，累计丢弃 %llu）",
				     (unsigned long long) s->frames_out, (unsigned) s->cur_w,
				     (unsigned) s->cur_h, mode_kind_name(mode->kind), (unsigned) flen,
				     (unsigned long long) s->frames_dropped);
		}
		if (abort_stream)
			break; /* BMH ERR 超 10 秒：拆会话走重开链 */
	}

out:
	/* 传输必须先取消、回收、释放，再 release/close——否则在飞传输碰已关句柄 */
	for (int i = 0; i < XFER_IN_FLIGHT; i++)
		if (xfers[i])
			libusb_cancel_transfer(xfers[i]); /* 未提交过的返回 NOT_FOUND，无害 */
	if (xfers[0] && d.ctx) {
		/* 泵几轮把 CANCELLED 完成态收回来（回调在本线程跑，直接收） */
		struct timeval tv = {0, 50000};
		for (int k = 0; k < 20; k++)
			libusb_handle_events_timeout(d.ctx, &tv);
	}
	for (int i = 0; i < XFER_IN_FLIGHT; i++) {
		if (xfers[i]) {
			bfree(xfers[i]->buffer);
			libusb_free_transfer(xfers[i]);
		}
	}
	if (tj)
		tjDestroy(tj);
	if (d.h) {
		if (streaming)
			blog(LOG_INFO, "uvc: 离开 bulk 流循环");
		libusb_release_interface(d.h, d.vs_iface >= 0 ? d.vs_iface : 0);
		libusb_close(d.h); /* auto_detach 开着时内核驱动会在这里重新绑回去 */
	}
	if (d.ctx)
		libusb_exit(d.ctx);
	if (d.bridge_fd > 0)
		obs_android_usb_close(d.bridge_fd); /* fd 归 Java，只还不关 */
	if (st.sbuf)
		bfree(st.sbuf);
	if (st.frame_buf)
		bfree(st.frame_buf);
	if (bgra)
		bfree(bgra);

	blog(LOG_INFO, "uvc: 本轮采集结束（送出 %llu 帧，丢弃 %llu 帧）", (unsigned long long) s->frames_out,
	     (unsigned long long) s->frames_dropped);
	return progress;
}

static void *uvc_thread_main(void *arg)
{
	struct uvc_source *s = arg;
	uint64_t retry_ms = 0;
	unsigned dead_rounds = 0;
	bool perm_requested = false;

	blog(LOG_INFO, "uvc: 采集线程启动（设备 '%s'，设置 video_format='%s' %dx%d@%d）", s->usb_device,
	     s->fmt_setting, s->width, s->height, s->fps);

	while (!os_atomic_load_long(&s->shutting_down)) {
		/* 死活按"本轮有没有送出帧"判，不能按 progress：猝死的实例 claim/SET 仍通
		 * （progress=true），但 GET/bulk 全 -1、永远送不出帧（真机 1617 实录） */
		const uint64_t out_before = s->frames_out;
		const bool progress = uvc_run_once(s, &perm_requested);
		if (os_atomic_load_long(&s->shutting_down))
			break;
		if (s->frames_out > out_before) {
			dead_rounds = 0;
			retry_ms = RETRY_BACKOFF_MIN_MS;
		} else if (++dead_rounds >= DEAD_ROUND_LIMIT) {
			retry_ms = DEAD_ROUND_BACKOFF_MS;
			set_status(s, "设备无响应（已连续 %u 轮送不出帧），请拔插设备…", dead_rounds);
			blog(LOG_WARNING, "uvc: 连续 %u 轮送不出帧，实例疑似猝死，退避 %llu ms（建议拔插设备）",
			     dead_rounds, (unsigned long long) retry_ms);
		} else {
			/* 本轮有进展（claim 上了接口）就重置退避：真断流/拔线才让等待指数拉长 */
			retry_ms = progress ? RETRY_BACKOFF_MIN_MS
					    : retry_ms ? (retry_ms * 2 > RETRY_BACKOFF_MAX_MS ? RETRY_BACKOFF_MAX_MS
											      : retry_ms * 2)
						       : RETRY_BACKOFF_MIN_MS;
		}
		char prev[sizeof(s->status)];
		pthread_mutex_lock(&s->mtx);
		snprintf(prev, sizeof(prev), "%s", s->status);
		pthread_mutex_unlock(&s->mtx);
		blog(LOG_INFO, "uvc: %llu ms 后重试（上次状态：%s）", (unsigned long long) retry_ms, prev);
		sleep_check(s, (uint32_t) retry_ms);
	}

	blog(LOG_INFO, "uvc: 采集线程退出");
	return NULL;
}

/* ---------------- 线程启停（照 nikon-source） ---------------- */

static void uvc_start_thread(struct uvc_source *s)
{
	if (os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 0);
	if (pthread_create(&s->thread, NULL, uvc_thread_main, s) != 0) {
		blog(LOG_ERROR, "uvc: 采集线程创建失败");
		return;
	}
	os_atomic_set_long(&s->thread_started, 1);
}

static void uvc_stop_thread(struct uvc_source *s)
{
	if (!os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 1);
	/* join 最坏堵一个 bulk 超时（1 s）。落在 deactivate/destroy（UI/视频线程）上，
	 * 与 nikon-source 里"join 最坏堵一个 PTP 超时"是同一类取舍。 */
	pthread_join(s->thread, NULL);
	os_atomic_set_long(&s->thread_started, 0);
}

/* ---------------- 源接口 ---------------- */

static const char *uvc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "UVC 摄像头（Android USB）";
}

static void uvc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "usb_device", "");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "fps", 60);
	obs_data_set_default_string(settings, "video_format", "mjpeg");
}

static int fmt_string_to_kind(const char *fmt)
{
	if (fmt && strcmp(fmt, "mjpeg") == 0)
		return MODE_MJPEG;
	if (fmt && strcmp(fmt, "yuyv") == 0)
		return MODE_YUYV;
	if (fmt && strcmp(fmt, "nv12") == 0)
		return MODE_NV12;
	return -1; /* 自动 */
}

static void uvc_settings_apply(struct uvc_source *s, obs_data_t *settings)
{
	const char *dev = obs_data_get_string(settings, "usb_device");
	snprintf(s->usb_device, sizeof(s->usb_device), "%s", dev ? dev : "");

	s->width = (int) obs_data_get_int(settings, "width");
	s->height = (int) obs_data_get_int(settings, "height");
	s->fps = (int) obs_data_get_int(settings, "fps");

	const char *fmt = obs_data_get_string(settings, "video_format");
	snprintf(s->fmt_setting, sizeof(s->fmt_setting), "%s", fmt ? fmt : "");
	s->prefer_fmt = fmt_string_to_kind(fmt);

	if (s->width < 16 || s->width > 7680 || s->height < 16 || s->height > 4320) {
		blog(LOG_WARNING, "uvc: 分辨率 %dx%d 不合理，回落到 1920x1080", s->width, s->height);
		s->width = 1920;
		s->height = 1080;
	}
	/* 属性页是自由数字输入，步进 2 也能攒出奇数怪值（真机见过 1920x1082）——夹成偶数 */
	if ((s->width & 1) || (s->height & 1)) {
		blog(LOG_WARNING, "uvc: 分辨率 %dx%d 不是偶数，夹到 %dx%d", s->width, s->height, s->width & ~1,
		     s->height & ~1);
		s->width &= ~1;
		s->height &= ~1;
	}
	s->usb_id[0] = 0; /* 设置变了，旧设备的 vid:pid 记忆作废（防认错成上一台） */
	if (s->fps < 1 || s->fps > 240) {
		blog(LOG_WARNING, "uvc: 帧率 %d 不合理，回落到 60", s->fps);
		s->fps = 60;
	}
}

static void *uvc_create(obs_data_t *settings, obs_source_t *source)
{
	struct uvc_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init_value(&s->mtx);
	if (pthread_mutex_init(&s->mtx, NULL) != 0)
		blog(LOG_ERROR, "uvc: 建锁失败，状态文本没有互斥了");
	uvc_settings_apply(s, settings);
	return s;
}

static void uvc_destroy(void *data)
{
	struct uvc_source *s = data;
	if (!s)
		return;
	uvc_stop_thread(s);
	pthread_mutex_destroy(&s->mtx);
	bfree(s);
}

static void uvc_activate(void *data)
{
	uvc_start_thread(data);
}

static void uvc_deactivate(void *data)
{
	uvc_stop_thread(data);
}

static void uvc_update(void *data, obs_data_t *settings)
{
	struct uvc_source *s = data;
	const char *dev = obs_data_get_string(settings, "usb_device");
	const int new_w = (int) obs_data_get_int(settings, "width");
	const int new_h = (int) obs_data_get_int(settings, "height");
	const int new_fps = (int) obs_data_get_int(settings, "fps");
	const int new_fmt = fmt_string_to_kind(obs_data_get_string(settings, "video_format"));

	/* 只拿"设置项 vs 设置项"比，没变化就不重启（同 nikon-source 的判法） */
	const bool changed = strcmp(dev ? dev : "", s->usb_device) != 0 || new_w != s->width ||
			     new_h != s->height || new_fps != s->fps || new_fmt != s->prefer_fmt;
	if (!changed)
		return;

	uvc_stop_thread(s);
	uvc_settings_apply(s, settings);
	if (obs_source_active(s->source))
		uvc_start_thread(s);
}

/* 属性页【申请 USB 授权】：权限结果是异步的，返回 false 不刷属性页（同 nikon-source） */
static bool on_request_permission(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct uvc_source *s = data;

	if (!s || !s->usb_device[0]) {
		blog(LOG_INFO, "uvc: 属性页点【申请授权】但还没选设备，先选设备再点");
		return false;
	}
	const int r = obs_android_usb_request_permission(s->usb_device);
	blog(LOG_INFO, "uvc: 属性页对 '%s' 点【申请授权】，总线返回 %d（%s）", s->usb_device, r,
	     r == 1 ? "弹窗中，用户选完后重新激活本源生效" : r == 0 ? "其实已有权限" : "失败");
	return false;
}

static obs_properties_t *uvc_properties(void *data)
{
	struct uvc_source *s = data;
	obs_properties_t *props = obs_properties_create();

	/* 设备清单是打开属性页那一刻的快照（插拔后要重开对话框） */
	obs_property_t *dev_list = obs_properties_add_list(props, "usb_device", "USB 摄像头", OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);

	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	if (n < 0) {
		obs_property_list_add_string(dev_list, "（USB 总线未接通：宿主 App 没注册 obs_android_set_usb_host）", "");
		blog(LOG_WARNING, "uvc: 属性页打不开设备清单 —— USB 总线未注册");
	} else if (n == 0) {
		obs_property_list_add_string(dev_list, "（当前没有 USB 摄像头，插上后重新打开本属性页）", "");
	} else {
		for (int i = 0; i < n; i++) {
			char text[OBS_ANDROID_USB_LABEL_LEN + OBS_ANDROID_USB_NAME_LEN + 16];
			snprintf(text, sizeof(text), "%s [%s]%s", devs[i].label, devs[i].name,
				 devs[i].has_permission ? "" : "（未授权）");
			obs_property_list_add_string(dev_list, text, devs[i].name);
		}
		obs_property_list_add_string(dev_list, "（不使用 USB 摄像头）", "");
		blog(LOG_INFO, "uvc: 属性页列出 %d 个 USB 设备", n);
	}

	obs_properties_add_button2(props, "request_perm", "申请 USB 授权", on_request_permission, s);

	obs_properties_add_int(props, "width", "采集宽度", 16, 7680, 2);
	obs_properties_add_int(props, "height", "采集高度", 16, 4320, 2);
	obs_properties_add_int(props, "fps", "采集帧率", 1, 240, 1);

	obs_property_t *p = obs_properties_add_list(props, "video_format", "设备出流格式", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "MJPEG（采集卡内编，1080p 首选）", "mjpeg");
	obs_property_list_add_string(p, "YUYV（未压缩，画质稳但吃 USB 带宽）", "yuyv");
	obs_property_list_add_string(p, "NV12（未压缩半平面）", "nv12");
	obs_property_list_add_string(p, "（自动：优先 MJPEG 1080p）", "");

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

struct obs_source_info uvc_input_info = {
	.id = "android_video_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* 异步源：靠 obs_source_output_video 推帧，宽高由 libobs 从帧里推 */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = uvc_getname,
	.create = uvc_create,
	.destroy = uvc_destroy,
	.activate = uvc_activate,
	.deactivate = uvc_deactivate,
	.update = uvc_update,
	.get_defaults = uvc_get_defaults,
	.get_properties = uvc_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	const struct libusb_version *uv = libusb_get_version();

	/* obs_register_source 返回 void，注册被拒只会在 blog 里说一声，必须自己回读确认 */
	obs_register_source(&uvc_input_info);

	const uint32_t flags = obs_get_source_output_flags("android_video_input");
	if (!flags) {
		blog(LOG_ERROR, "android-capture: 'android_video_input' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO, "android-capture: 已注册源类型 'android_video_input'（flags=0x%x / 自写 UVC-over-libusb / "
		       "libusb %u.%u.%u.%u api=0x%x）",
	     (unsigned) flags, (unsigned) uv->major, (unsigned) uv->minor, (unsigned) uv->micro,
	     (unsigned) uv->nano, (unsigned) LIBUSB_API_VERSION);
	return true;
}
