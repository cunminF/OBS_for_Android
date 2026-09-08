// 阶段 2 第 2 项：UVC 采集源（USB Webcam，libuvc + libusb 无 root 模式）。
//
// fd 从哪来：Android 上应用没有 /dev/bus/usb 的访问权，libusb 也不能自己 open。
// 唯一可行的路径是 plan.md §四 阶段 2 写的那条——Java 侧 UsbManager 拿到
// UsbDeviceConnection.getFileDescriptor()，把这个 int fd 传进 native，再由
// libusb_wrap_sys_device() 包住它。libuvc 0.0.7 已经把这一步打包成了 uvc_wrap()
// （device.c，受 LIBUSB_API_VERSION >= 0x01000107 保护，我们这份是 0x0100010A），
// 所以不需要给 libuvc 打补丁。uvc_wrap 的注释明确写了"fd 的所有权还在调用方，
// uvc_close() 不会关它"，正好对应"fd 由 Java 持有、Java 负责关"。
//
// 一个必须记住的坑：uvc_open_internal() 里起事件处理线程的条件是
//     if (dev->ctx->own_usb_ctx && dev->ctx->open_devices == NULL)
// 也就是说如果把自建 libusb_context 传给 uvc_init()，own_usb_ctx 为 0，
// 线程不起，libusb_handle_events 没人泵，采集回调永远不来。所以这里固定
// 用 uvc_init(&ctx, NULL) 让 libuvc 自己拥有并负责泵它的 usb 上下文。
//
// 帧缓冲不需要我们自己拷贝：obs_source_output_video() 走
// libobs/obs-source.c 的 cache_video() → copy_frame_data()，像素已经在 libobs
// 内部复制进 async 帧缓存了，所以可以直接把 libuvc 的 buffer 指针递过去。
// 回调里也不能调任何 uvc_* 函数（libuvc.h 里明确禁止）。

#include <libusb-1.0/libusb.h>
#include <libuvc/libuvc.h>

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>

/* obs.h 里的包含是 #ifdef __ANDROID__ 的，但这个头本身与平台无关，直接包进来可以让
 * 桌面侧的语法检查（clangd 不定义 __ANDROID__）也走同一条路，不必为了一个数组长度
 * 再抄一份结构体定义 —— 抄的那份会跟真的漂移。 */
#include <obs-android.h>

struct uvc_source {
	obs_source_t *source;

	uvc_context_t *ctx;
	uvc_device_handle_t *devh;
	uvc_stream_ctrl_t ctrl;

	/* MJPEG 解码用的 RGB24 中转缓冲，按当前分辨率分配 */
	uvc_frame_t rgb;
	uint8_t *rgb_buf;
	size_t rgb_size;

	/* RGB24 → BGRA 的目标缓冲（libobs 没有 24 位打包格式） */
	uint8_t *bgra_buf;
	size_t bgra_size;

	/* settings —— 只反映设置项本身，用来做 update 的变更检测 */
	int64_t usb_fd_setting;
	/* 阶段 2-3 加的：Java 枚举出来的设备名（UsbDevice.getDeviceName()）。
	 * usb_fd_setting 留空/为 0 且这里非空时，就通过 libobs 的 Android USB 总线去要
	 * 一个 fd，这样 UI 只需要选设备，不需要知道 fd 这种底层细节。 */
	char usb_device[OBS_ANDROID_USB_NAME_LEN];
	/* 从总线借来的 fd（0 = 没借）。不单独存"有效 fd"字段：每次 capture_start 现场算，
	 * 免得两份状态互相覆盖 —— 那会让 uvc_update 把"设置没变"误判成变了、反复重启采集。 */
	int bridge_fd;

	int width;
	int height;
	int fps;
	bool want_mjpeg;

	uint64_t frames_out;
	uint64_t frames_dropped;
	bool stop_requested;
};

static const char *uvc_frame_fmt_name(enum uvc_frame_format f)
{
	switch (f) {
	case UVC_FRAME_FORMAT_UNCOMPRESSED:
		return "uncompressed";
	case UVC_FRAME_FORMAT_YUYV:
		return "YUYV";
	case UVC_FRAME_FORMAT_UYVY:
		return "UYVY";
	case UVC_FRAME_FORMAT_MJPEG:
		return "MJPEG";
	case UVC_FRAME_FORMAT_GRAY8:
		return "GRAY8";
	case UVC_FRAME_FORMAT_H264:
		return "H264";
	default:
		return "other";
	}
}

static const char *uvc_subtype_name(uint8_t subtype)
{
	switch (subtype) {
	case 0x04:
		return "UNCOMPRESSED";
	case 0x06:
		return "MJPEG";
	case 0x10:
		return "FRAME_BASED";
	case 0x11:
		return "COLORFORMATS";
	default:
		return "OTHER";
	}
}

/* 设备被关掉：先关流，再关句柄，最后销毁 libuvc 上下文。
 * uvc_close 之后 fd 仍然属于 Java 侧，这里绝不 close(fd)。 */
static void uvc_capture_stop(struct uvc_source *data)
{
	const bool had_handle = data->devh || data->ctx;

	data->stop_requested = true;

	if (data->devh) {
		uvc_stop_streaming(data->devh);
		uvc_close(data->devh);
		data->devh = NULL;
	}
	if (data->ctx) {
		uvc_exit(data->ctx);
		data->ctx = NULL;
	}
	/* 借来的 fd 必须还：Java 侧那个 UsbDeviceConnection 不关就会一直占着设备，
	 * 下次 open 拿到的是同一个（可能已失效的）fd。 */
	if (data->bridge_fd > 0) {
		obs_android_usb_close(data->bridge_fd);
		data->bridge_fd = 0;
	}
	if (data->rgb_buf) {
		bfree(data->rgb_buf);
		data->rgb_buf = NULL;
		data->rgb_size = 0;
	}
	if (data->bgra_buf) {
		bfree(data->bgra_buf);
		data->bgra_buf = NULL;
		data->bgra_size = 0;
	}

	if (had_handle)
		blog(LOG_INFO, "uvc: 采集已停止（送出 %llu 帧，丢弃 %llu 帧）", (unsigned long long) data->frames_out,
		     (unsigned long long) data->frames_dropped);
}

/* 把设备支持的格式/分辨率打出来。选错分辨率时这是定位问题的主要手段，
 * 真机上第一次接摄像头也靠它确认设备到底能出什么。 */
static void uvc_log_supported_modes(struct uvc_source *data)
{
	const uvc_format_desc_t *fmt = uvc_get_format_descs(data->devh);

	if (!fmt) {
		blog(LOG_WARNING, "uvc: 设备没报任何格式描述符");
		return;
	}

	for (; fmt; fmt = fmt->next) {
		const uvc_frame_desc_t *fr = fmt->frame_descs;
		for (; fr; fr = fr->next) {
			blog(LOG_INFO, "uvc:   模式 fmt=%u/%s %ux%u 默认间隔=%u(100ns) bpp=%u 最大帧长=%u",
			     (unsigned) fmt->bFormatIndex, uvc_subtype_name(fmt->bDescriptorSubtype),
			     (unsigned) fr->wWidth, (unsigned) fr->wHeight, (unsigned) fr->dwDefaultFrameInterval,
			     (unsigned) fmt->bBitsPerPixel, (unsigned) fr->dwMaxVideoFrameBufferSize);
		}
	}
}

static void uvc_rgb_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height)
{
	const size_t line = (size_t) width * 4;

	for (int y = 0; y < height; y++) {
		const uint8_t *s = src + (size_t) y * (size_t) width * 3;
		uint8_t *d = dst + (size_t) y * line;
		for (int x = 0; x < width; x++) {
			d[0] = s[2]; /* B */
			d[1] = s[1]; /* G */
			d[2] = s[0]; /* R */
			d[3] = 0xFF;
			s += 3;
			d += 4;
		}
	}
}

/* 运行在 libuvc 的事件线程上。这里不调用任何 uvc_*（除了转换函数，
 * uvc_mjpeg2rgb 只碰传入的 frame，不碰设备）。 */
static void uvc_frame_cb(uvc_frame_t *frame, void *ptr)
{
	struct uvc_source *data = ptr;
	struct obs_source_frame out = {0};

	if (data->stop_requested || !frame || !frame->data)
		return;

	out.width = (uint32_t) frame->width;
	out.height = (uint32_t) frame->height;
	out.timestamp = os_gettime_ns();

	if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
		const size_t need = (size_t) frame->width * (size_t) frame->height * 3;
		if (need > data->rgb_size) {
			bfree(data->rgb_buf);
			data->rgb_buf = bmalloc(need);
			data->rgb_size = need;
		}
		data->rgb.data = data->rgb_buf;
		data->rgb.data_bytes = data->rgb_size;
		data->rgb.step = (size_t) frame->width * 3;
		data->rgb.library_owns_data = 0;

		uvc_error_t r = uvc_mjpeg2rgb(frame, &data->rgb);
		if (r != UVC_SUCCESS) {
			data->frames_dropped++;
			if (data->frames_dropped <= 5)
				blog(LOG_WARNING, "uvc: MJPEG 解码失败 %d (%s)，丢帧", (int) r, uvc_strerror(r));
			return;
		}

		/* BGRA 单平面，libobs 侧不需要 GPU 转换 */
		const size_t need_bgra = (size_t) frame->width * (size_t) frame->height * 4;
		if (need_bgra > data->bgra_size) {
			bfree(data->bgra_buf);
			data->bgra_buf = bmalloc(need_bgra);
			data->bgra_size = need_bgra;
		}
		uvc_rgb_to_bgra(data->rgb_buf, data->bgra_buf, (int) frame->width, (int) frame->height);

		out.format = VIDEO_FORMAT_BGRA;
		out.data[0] = data->bgra_buf;
		out.linesize[0] = (uint32_t) frame->width * 4;
		out.full_range = true;
	} else {
		/* UVC 未压缩载荷基本都是 YUYV 4:2:2 打包（也有 UYVY 的机器，
		 * 真机接上来对一下 step 就能确认） */
		if (frame->frame_format != UVC_FRAME_FORMAT_YUYV) {
			data->frames_dropped++;
			if (data->frames_dropped <= 5)
				blog(LOG_WARNING, "uvc: 收到本插件还没映射的格式 %d，丢帧", (int) frame->frame_format);
			return;
		}

		out.format = VIDEO_FORMAT_YUY2;
		out.data[0] = frame->data;
		out.linesize[0] = (uint32_t) frame->step;
		/* 未压缩 UVC 载荷按 limited range(16-235) 处理，真机确认后再定；
		 * 该问题与 plan.md 8.11 的编码像素色彩语义是同一个 */
		out.full_range = false;
	}

	/* 这一步不是可选项，也不属于"色彩调优"：libobs 给 YUV 转换着色器**无条件**喂
	 * frame->color_matrix（obs-source.c:2445-2453 那三行 vec4_set），而 full_range=false 时
	 * 连 color_range_min/max 也原样喂进去（:2454-2458），着色器第一句就是
	 * clamp(yuv, color_range_min, color_range_max)（data/format_conversion.effect:564）。
	 * 上面那个 out 是 {0} 起手的 ⇒ 全零上下限把每个分量钳成 0、再乘零矩阵 ⇒ **画面纯黑**。
	 * MJPEG 那一路出的是 BGRA、走不到 YUV_to_RGB，所以这条只在 YUYV 分支上成立 ——
	 * 也正是它到今天还没被撞见的原因（R-2 真机轮至今没跑过）。
	 * 写法与仓内先例同：linux-v4l2/v4l2-input.c:128。 */
	const enum video_range_type range = out.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, range, out.format, out.color_matrix,
					       out.color_range_min, out.color_range_max);

	obs_source_output_video(data->source, &out);

	data->frames_out++;
	if (data->frames_out == 1 || data->frames_out % 300 == 0)
		blog(LOG_INFO, "uvc: 第 %llu 帧 %ux%u fmt=%s linesize=%u", (unsigned long long) data->frames_out,
		     (unsigned) out.width, (unsigned) out.height, uvc_frame_fmt_name(frame->frame_format),
		     (unsigned) out.linesize[0]);
}

/* 设置里只选了设备（usb_device 非空）而没直接给 fd 时，走 libobs 的 Android USB 总线
 * 把 fd 要过来，返回"这次该用的 fd"。App 没注册宿主时 obs_android_usb_enum_devices()
 * 返回 -1，这里如实报出来 —— 否则症状会是"选了设备却没画面"，查不到是桥没接通。 */
static int64_t uvc_bridge_acquire(struct uvc_source *data)
{
	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);

	if (n < 0) {
		blog(LOG_WARNING, "uvc: 设置里选了设备 '%s'，但 USB 总线不可用（enum 返回 %d）—— 宿主 App 没调 obs_android_set_usb_host()",
		     data->usb_device, n);
		return 0;
	}

	for (int i = 0; i < n; i++) {
		if (strcmp(devs[i].name, data->usb_device) != 0)
			continue;

		if (!devs[i].has_permission) {
			const int pr = obs_android_usb_request_permission(devs[i].name);
			blog(LOG_INFO,
			     "uvc: 设备 '%s'（%s）还没有 USB 权限，已发起授权请求（返回 %d：%s）—— "
			     "用户点\"允许\"后重新激活本源即可取流，本源先保持空闲",
			     devs[i].name, devs[i].label, pr,
			     pr == 1 ? "弹窗中" : pr == 0 ? "其实已有权限" : "失败");
			return 0; /* 空闲，不算故障 */
		}

		const int fd = obs_android_usb_open(devs[i].name);
		if (fd > 0) {
			data->bridge_fd = fd;
			blog(LOG_INFO, "uvc: 已通过总线拿到 '%s' 的 fd=%d", devs[i].name, fd);
			return fd;
		}
		/* 有权限却拿不到 fd：设备刚被拔、或 Java 侧连接数超上限。
		 * 把坏值交回给 capture_start 的 <0 分支统一报，不另立一套说法。 */
		blog(LOG_WARNING, "uvc: 总线打开 '%s' 给出 fd=%d", devs[i].name, fd);
		return fd;
	}

	blog(LOG_WARNING, "uvc: 设置里的设备 '%s' 不在当前 USB 清单里（清单共 %d 项）—— 多半已经拔掉",
	     data->usb_device, n);
	/* 不伪造 fd，返回 0 → 按"空闲"处理，日志口径与 A2 的分支断言一致 */
	return 0;
}

static bool uvc_capture_start(struct uvc_source *data)
{
	uvc_error_t r;

	if (data->devh)
		return true;

	int64_t fd = data->usb_fd_setting;

	/* 阶段 2-3：UI 只选设备、不填 fd，所以先问总线要一个 */
	if (fd <= 0 && data->usb_device[0])
		fd = uvc_bridge_acquire(data);

	/* fd 的三种取值是三条不同的路，A2 冒烟就是逐条验它们：
	 *   0  —— 设置项的默认值，Java 侧还没给 fd，属于正常空闲态
	 *   <0 —— Java 明确给了个坏 fd。UsbDeviceConnection.getFileDescriptor() 在
	 *        openDevice 失败（用户拒了 USB 授权、设备已经拔掉）时就是返回 -1，
	 *        这和"还没给"是两回事，混在一起会把真实故障藏起来
	 *   >0 —— 交给 libusb_wrap_sys_device */
	if (fd == 0) {
		blog(LOG_INFO,
		     "uvc: 还没有 USB fd —— 等待 Java 侧 UsbManager 打开设备后下发；本源保持空闲，不采集");
		return false;
	}
	if (fd < 0) {
		blog(LOG_WARNING,
		     "uvc: 拿到的 fd 是 %lld，不是可用设备句柄 —— 多半是 openDevice 失败"
		     "（用户拒了 USB 授权，或设备在拿 fd 之前就被拔了），本源保持空闲",
		     (long long) fd);
		return false;
	}

	r = uvc_init(&data->ctx, NULL);
	if (r != UVC_SUCCESS) {
		blog(LOG_ERROR, "uvc: uvc_init 失败 %d (%s)：模拟器/无 usbfs 的设备上 libusb 可能根本没有可用后端",
		     (int) r, uvc_strerror(r));
		data->ctx = NULL;
		return false;
	}

	r = uvc_wrap((int) fd, data->ctx, &data->devh);
	if (r != UVC_SUCCESS || !data->devh) {
		blog(LOG_ERROR, "uvc: uvc_wrap(fd=%lld) 失败 %d (%s)", (long long) fd, (int) r, uvc_strerror(r));
		data->devh = NULL;
		uvc_exit(data->ctx);
		data->ctx = NULL;
		return false;
	}

	r = uvc_get_stream_ctrl_format_size(data->devh, &data->ctrl,
					    data->want_mjpeg ? UVC_FRAME_FORMAT_MJPEG : UVC_FRAME_FORMAT_YUYV,
					    data->width, data->height, data->fps);
	if (r != UVC_SUCCESS) {
		blog(LOG_ERROR, "uvc: 协商 %dx%d@%d %s 失败 %d (%s)，设备支持的模式如下：", data->width,
		     data->height, data->fps, data->want_mjpeg ? "MJPEG" : "YUYV", (int) r, uvc_strerror(r));
		uvc_log_supported_modes(data);
		uvc_capture_stop(data);
		return false;
	}

	data->stop_requested = false;
	r = uvc_start_streaming(data->devh, &data->ctrl, uvc_frame_cb, data, 0);
	if (r != UVC_SUCCESS) {
		blog(LOG_ERROR, "uvc: uvc_start_streaming 失败 %d (%s)：bFormatIndex=%u bFrameIndex=%u "
				"dwFrameInterval=%u dwMaxVideoFrameSize=%u dwMaxPayloadTransferSize=%u",
		     (int) r, uvc_strerror(r), (unsigned) data->ctrl.bFormatIndex, (unsigned) data->ctrl.bFrameIndex,
		     (unsigned) data->ctrl.dwFrameInterval, (unsigned) data->ctrl.dwMaxVideoFrameSize,
		     (unsigned) data->ctrl.dwMaxPayloadTransferSize);
		uvc_capture_stop(data);
		return false;
	}

	blog(LOG_INFO,
	     "uvc: 采集已启动 —— fd=%lld(%s) %dx%d@%d %s / 协商结果 fmt=%u frame=%u interval=%u maxframe=%u payload=%u",
	     (long long) fd, data->bridge_fd > 0 ? "总线/设备名" : "设置项", data->width, data->height, data->fps,
	     data->want_mjpeg ? "MJPEG" : "YUYV", (unsigned) data->ctrl.bFormatIndex, (unsigned) data->ctrl.bFrameIndex,
	     (unsigned) data->ctrl.dwFrameInterval, (unsigned) data->ctrl.dwMaxVideoFrameSize,
	     (unsigned) data->ctrl.dwMaxPayloadTransferSize);
	return true;
}

static void uvc_settings_apply(struct uvc_source *data, obs_data_t *settings)
{
	data->usb_fd_setting = obs_data_get_int(settings, "usb_fd");

	const char *dev = obs_data_get_string(settings, "usb_device");
	if (dev && dev[0]) {
		if (strlen(dev) >= sizeof(data->usb_device))
			blog(LOG_WARNING, "uvc: 设备名 '%s' 超过 %zu 字节，已截断", dev, sizeof(data->usb_device) - 1);
		snprintf(data->usb_device, sizeof(data->usb_device), "%s", dev);
	} else {
		data->usb_device[0] = 0;
	}

	data->width = (int) obs_data_get_int(settings, "width");
	data->height = (int) obs_data_get_int(settings, "height");
	data->fps = (int) obs_data_get_int(settings, "fps");

	const char *fmt = obs_data_get_string(settings, "video_format");
	data->want_mjpeg = fmt && strcmp(fmt, "mjpeg") == 0;

	if (data->width < 16 || data->width > 7680 || data->height < 16 || data->height > 4320) {
		blog(LOG_WARNING, "uvc: 分辨率 %dx%d 不合理，回落到 1280x720", data->width, data->height);
		data->width = 1280;
		data->height = 720;
	}
	if (data->fps < 1 || data->fps > 240) {
		blog(LOG_WARNING, "uvc: 帧率 %d 不合理，回落到 30", data->fps);
		data->fps = 30;
	}
}

static const char *uvc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "UVC 摄像头（Android USB）";
}

static void *uvc_create(obs_data_t *settings, obs_source_t *source)
{
	struct uvc_source *data = bzalloc(sizeof(*data));
	data->source = source;
	data->rgb.library_owns_data = 0;
	uvc_settings_apply(data, settings);
	return data;
}

static void uvc_destroy(void *ptr)
{
	struct uvc_source *data = ptr;
	if (!data)
		return;

	uvc_capture_stop(data);
	bfree(data);
}

static void uvc_activate(void *ptr)
{
	struct uvc_source *data = ptr;
	uvc_capture_start(data);
}

static void uvc_deactivate(void *ptr)
{
	struct uvc_source *data = ptr;
	uvc_capture_stop(data);
}

static void uvc_update(void *ptr, obs_data_t *settings)
{
	struct uvc_source *data = ptr;
	const char *fs = obs_data_get_string(settings, "video_format");
	const char *dev = obs_data_get_string(settings, "usb_device");

	/* 一律拿"设置项 vs 设置项"比：data->bridge_fd 是运行期借来的活 fd，
	 * 拿它跟设置里的 0 比会让每次 update 都判成"变了"、反复重启采集。 */
	const int64_t new_fd = obs_data_get_int(settings, "usb_fd");
	const int new_w = (int) obs_data_get_int(settings, "width");
	const int new_h = (int) obs_data_get_int(settings, "height");
	const int new_fps = (int) obs_data_get_int(settings, "fps");
	const bool new_mjpeg = fs && strcmp(fs, "mjpeg") == 0;
	/* settings_apply 已把 NULL 和空串都归一成"usb_device 首字节为 0"，所以直接比字符串
	 * 就行；拿指针真假去比会把"空 vs 空"误判成变更，从而每帧重启采集。 */
	const bool dev_changed = strcmp(dev ? dev : "", data->usb_device) != 0;

	if (new_fd == data->usb_fd_setting && !dev_changed && new_w == data->width && new_h == data->height &&
	    new_fps == data->fps && new_mjpeg == data->want_mjpeg)
		return;

	/* 即使没在跑，也可能已经借了一个 fd（比如协商失败），换设备前必须先还。
	 * capture_stop 内部对 devh/ctx/buffer/桥接 fd 全都判空，未在跑时是干净的。 */
	uvc_capture_stop(data);

	uvc_settings_apply(data, settings);

	/* 三条 fd 分支的日志统一由 uvc_capture_start 产生（fd<=0 时它自己会打"空闲/坏 fd"
	 * 并返回 false），这里不再另说一种说法 */
	if (obs_source_active(data->source))
		uvc_capture_start(data);
}

static void uvc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "usb_fd", 0);
	obs_data_set_default_string(settings, "usb_device", "");
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_string(settings, "video_format", "yuyv");
}

static obs_properties_t *uvc_properties(void *unused)
{
	obs_properties_t *props = obs_properties_create();

	UNUSED_PARAMETER(unused);

	/* usb_fd 不放进属性页：它只在运行期由 Java 侧下发，写进配置文件会在
	 * 下次启动时变成一个已经失效的 fd */

	/* 设备清单是打开属性页那一刻的快照 —— OBS 的属性页没有"自动刷新"机制，
	 * 插上/拔掉设备后要重开这个对话框才看得到（热插拔广播负责的是"源自己停流 +
	 * 日志提示"，见 ObsUsbHost 的 receiver）。 */
	obs_property_t *dev_list = obs_properties_add_list(props, "usb_device", "USB 摄像头", OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);

	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	if (n < 0) {
		/* 桥没接通是宿主 App 的接线故障，不是"用户没插摄像头"，两件事必须说两种话，
		 * 否则现场会把配置问题当成硬件问题 */
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
		/* "" 这一项留着，是为了让"从某台设备切回不采集"在 UI 上做得到 */
		obs_property_list_add_string(dev_list, "（不使用 USB 摄像头）", "");
		blog(LOG_INFO, "uvc: 属性页列出 %d 个 USB 设备", n);
	}

	obs_properties_add_int(props, "width", "采集宽度", 16, 7680, 2);
	obs_properties_add_int(props, "height", "采集高度", 16, 4320, 2);
	obs_properties_add_int(props, "fps", "采集帧率", 1, 240, 1);

	obs_property_t *p = obs_properties_add_list(props, "video_format", "设备出流格式", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "YUYV（未压缩，画质稳但吃 USB 带宽）", "yuyv");
	obs_property_list_add_string(p, "MJPEG（摄像头内编，省带宽，需解码）", "mjpeg");

	return props;
}

struct obs_source_info uvc_input_info = {
	.id = "android_video_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* 必须带 ASYNC：这是靠 obs_source_output_video 推裸帧的异步源，宽高由 libobs 从帧里推。
	 * 只写 VIDEO 会被 obs_register_source 拒掉（obs-module.c:1032 要求非异步的视频源
	 * 自己提供 get_width/get_height），A2 首轮实测就是这么挂的。 */
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

	/* obs_register_source 是返回 void 的（obs-source.h:568），注册被拒只会在 blog 里说一声，
	 * 必须自己回读确认，否则就会出现"libobs 已拒绝、模块却谎报加载成功"（A2 首轮实测）。 */
	obs_register_source(&uvc_input_info);

	const uint32_t flags = obs_get_source_output_flags("android_video_input");
	if (!flags) {
		blog(LOG_ERROR, "android-capture: 'android_video_input' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO,
	     "android-capture: 已注册源类型 'android_video_input'（flags=0x%x / libuvc %s / libusb %u.%u.%u.%u api=0x%x）",
	     (unsigned) flags, LIBUVC_VERSION_STR, (unsigned) uv->major, (unsigned) uv->minor, (unsigned) uv->micro,
	     (unsigned) uv->nano, (unsigned) LIBUSB_API_VERSION);
	return true;
}
