// P-18 屏幕采集源（MediaProjection + AImageReader）。当前进度：a 授权与前台服务类型接线、
// b 取帧进预览、c 进白名单/装载清单、d 生命周期与交还（收回、尺寸变化重建、最后一颗源销毁
// 交还三支都在设备上量过）、e 系统内录（一路一份的泵，下面 P-18-e 那一段）—— **a~e 五条腿已闭**。
// e 在设备上量到的是"接得上、取得到、拆得对"（48000Hz×2、614 秒音频对 614 秒墙钟、拆除顺序合合同），
// 而**样本内容这台恒零**：MuMu 的 AudioFlinger 从来没往 REMOTE_SUBMIX 那条输出线程发过一帧
// （`0 Tracks / Total writes: 0`），同刻扬声器线程却有 1 条活跃轨 —— "听得见"这一格判给真机。
// 泵里那枚峰值表（`audio_peak_all`）就是为分开"接上了但全是 0"与"根本没接上"而加的，别拆掉。
// f（未闭）：停录制后保活复核把前台服务停了，系统随即收回投影令牌，录屏 0.8~1.5 秒后自断。
// 判据与排期见 plan.md §五 P-18-e 那条。
//
// 与相机那一路最根本的不同：**帧的来路不用我们自己驱动**。Camera2 要我们自己下发重复请求，
// 而投影面一旦建成，SurfaceFlinger 就按屏幕的刷新节奏往这块面上画，我们只是接。这一条决定了
// 下面两件事的形状（都是量出来的，不是照抄相机那一路的习惯）：
//   1) **不套相机的 capture_allowed 那一格**。相机是独占设备，窗口一停就必须撒手，否则下一次
//      进来就是 CAMERA_IN_USE；投影正相反 —— OBS 退到后台时继续录**别的应用**正是这功能的卖点，
//      照抄"不在前台就不采"等于让用户按个 Home 就把自己录停了。所以这一路不看应用状态，
//      只看两件事：令牌还在不在、屏幕尺寸还是不是当初那一块（video_tick 里那一拍一秒的轮询）。
//   2) **没有"长时间没帧就算故障"那种看门狗**。VirtualDisplay 的产出跟着屏幕内容走：实测弹窗
//      挂屏静止时 300 帧 / 292 秒 ≈ 1.03 fps，界面在动时 300 帧 / 20.000 秒 = 15 fps（b 腿）。
//      静止不是故障，判它故障就是自己给自己造报警。只有一条例外值得单列 ——
//      "面建好了却连第一帧都没见过"，那是真异常（下面 FIRST_FRAME_NS 那里）。
//
// 四条从 NDK 30 / libobs 头里量出来的事实（不是推定，写下来免得下次再翻一遍头）：
//   1) AIMAGE_FORMAT_RGBA_8888 的内存字节序是 R,G,B,A。NdkImage.h:57-74 把它对应到
//      AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 与 VK_FORMAT_R8G8B8A8_UNORM —— 后两个名字就是
//      按内存顺序写的。⇒ 递给 libobs 的必须是 VIDEO_FORMAT_RGBA，**不是 BGRA**（我立案时写的
//      就是 BGRA，是错的）：GS_BGRA 在 Android 上传前会被 gl_swap_rb_bgra 翻一次
//      （gl-subsystem.h:176-223 那段实测），标成 BGRA 等于让人白翻一遍，症状是红蓝互换。
//   2) 非 YUV 格式在 libobs 里走"直传"而不是着色器：get_convert_type 对 RGBA 给的是
//      `full_range ? CONVERT_NONE : CONVERT_RGB_LIMITED`（obs-source.c:1747-1751），只有
//      CONVERT_NONE 那一支才落到 gs_texture_set_image 直传（:2495-2499）。obs_source_frame 的
//      注释（obs.h:283-285）说非 YUV 一律按 full range 处理，但我还是显式写 full_range=true ——
//      走偏一半的后果是"本来对的画面再被压一档"，那与 P-19 那笔亮度账是同一族错。
//   3) 纹理格式跟的是 convert_video_format：VIDEO_FORMAT_RGBA → GS_RGBA（obs-internal.h:1082），
//      Android 侧 GS_RGBA 的内部格式是 GL_SRGB8_ALPHA8、上传 format=GL_RGBA、不交换通道 ——
//      与 1) 合起来正好对上。**注意这条链采样的是存储里的 alpha**：P-17-c 那第二半根因就是
//      "alpha 落地为 0，源被当全透明"。系统往投影面上写的 A 到底是不是 255，头里没写，
//      所以下面 push_frame 里那句 A 分量 min/max 是这一条腿的必备仪器，不是可选诊断。
//   4) window 归 reader 所有：AImageReader_getWindow 的头注释明令不许 ANativeWindow_release
//      （相机那边同一句写在 camera2-input.c:121），这里照办。
//
// 为什么令牌不进 native：它只在 Java 层活着（ObsProjectionHost 的那个静态字段），过界的是
// **目标面** —— 本文件把 AImageReader 的 ANativeWindow* 当不透明 void* 递给总线，前端拿
// ANativeWindow_toSurface 换成 Surface 交给 Java 去 createVirtualDisplay。总线形状与这套取舍
// 的理由写在 libobs/obs-android.h 的 P-18 段，注册方是前端 OBSAndroidPermissions.cpp。

#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

/* pthread 给泵那条线程与扇出表的互斥量 —— OBS 这一版没有 os_mutex_*，threading.h 自己就是
 * 直接 include <pthread.h> 再往上加 os_event_*；DARRAY 给"哪几颗源在要音"那张表。 */
#include <pthread.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h> /* os_atomic_*（拆除协议那两个计数）、os_event_*（泵的停止标志） */

#include <obs-android.h>

/* 缓冲数。与相机同一笔账：头里两处把这变成硬前提 —— maxImages 至少 2 才谈得上
 * acquireLatestImage 的"丢旧留新"，而 acquire 数一旦等于 maxImages 又不 AImage_delete，
 * 之后一律 AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED。4 = 2（latest 的余量）+ 2（在途）。 */
#define MAX_IMAGES 4

/* 开不起来时下一次尝试的间隔。为什么必须有这个数：activate 那一刻同意框可能还在用户手里
 * （甚至还没弹），而 video_tick 是 60Hz 一根 —— 不设闸就是每秒 60 次起同意框。
 * 1 秒是"用户点完到画面出来"的感知下限之内，又不至于让日志刷起来。 */
#define RETRY_INTERVAL_NS (1000ULL * 1000000ULL)

/* "开了面却迟迟见不到第一帧"的判定线，以及为什么它不扩成一条通用看门狗：见文件头第 2 条。
 * 3 秒不是拍的 —— b 腿实测第一帧到手是 101 ms（"采集已开"那行到第 1 帧那行），取它的三十倍，
 * 越过这条线就是面真建歪了（VirtualDisplay 报成功却没在往这块面上画）。
 * 与 RETRY_INTERVAL_NS 合用同一道闸（video_tick 里那一拍），所以实际精度是 ±1 秒。 */
#define FIRST_FRAME_NS (3000ULL * 1000000ULL)

/* 本模块此刻活着几颗源。为什么非得数这个数：令牌是**全设备一份**，而源可以有好几颗
 * （同一场景里复制一颗就有了），所以"把令牌交还系统"只能挂在最后一颗走掉的那一刻 ——
 * 早一步会把还在采的那颗打死，晚一步就是进程还活着、系统却一直以为在被录
 * （常驻"停止共享"通知 + 前台服务白占 mediaProjection 那一档，正是 P-18-a 量到的那种自相矛盾态）。
 * 用原子而不是普通 long：create 在 UI 线程上，destroy 可能在任何一颗源被回收的线程上。 */
static volatile long live_sources = 0;

struct screen_source {
	obs_source_t *source;

	/* 采集链：全部成员只在"开成"到"关掉"之间存活。所有权见文件头第 4 条 ——
	 * window 归 reader，所以 close 时只 delete reader、不碰 window。 */
	AImageReader *reader;
	ANativeWindow *window;
	uint32_t width;
	uint32_t height;

	/* 拆除协议那两个计数，形状抄相机：回调在 reader 自己的线程上跑，
	 * 而 AImageReader_delete 不承诺 join 那条线程。 */
	volatile long in_callback;
	volatile long shutting_down;

	uint64_t next_try_ns;   // 退避的那道闸，见 RETRY_INTERVAL_NS
	bool consent_asked;     // 一次 activate episode 只弹一次同意框，见 try_start 那段
	bool cb_thread_maybe_live;

	/* 第一帧那台仪器的三个读数，都在 open_capture 里起、在 video_tick 里判。
	 * frames_at_open 是"这一场开面那一刻的累计帧数"快照：frames_out 是跨场的累计数
	 * （属性页那一行报的就是它，不该被重建一次面就清零），所以判"这一场出过帧没有"
	 * 只能拿它跟快照比，不能跟 0 比。 */
	uint64_t opened_ns;
	uint64_t frames_at_open;
	bool no_frame_warned;

	uint64_t frames_out;
	uint64_t frames_dropped;
	char last_error[256];
};

static void set_error(struct screen_source *c, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(c->last_error, sizeof(c->last_error), fmt, args);
	va_end(args);

	blog(LOG_WARNING, "android-screen: %s", c->last_error);
}

static void drop_frame(struct screen_source *c, const char *why, int32_t a, int32_t b)
{
	c->frames_dropped++;
	if (c->frames_dropped == 1 || (c->frames_dropped % 300) == 0)
		blog(LOG_WARNING, "android-screen: 丢帧（%s）参数=%d,%d 已累计 %llu 帧", why, (int)a, (int)b,
		     (unsigned long long)c->frames_dropped);
}

/* 把一帧 RGBA 递给 libobs。
 * 与相机那次唯一的实质差别是"只有一个平面、四个字节一个像素"，所以这里的活几件：
 * 读元数据 → 夹裁剪矩形 → 递帧 → 数一下 alpha。 */
static void push_frame(struct screen_source *c, AImage *image)
{
	struct obs_source_frame out = {0};
	int32_t fw = 0, fh = 0, planes = 0, row = 0, px = 0, len = 0;
	uint8_t *buf = NULL;
	AImageCropRect crop = {0};

	const bool meta_ok = (AImage_getWidth(image, &fw) == AMEDIA_OK) &&
			     (AImage_getHeight(image, &fh) == AMEDIA_OK) &&
			     (AImage_getNumberOfPlanes(image, &planes) == AMEDIA_OK) &&
			     (AImage_getCropRect(image, &crop) == AMEDIA_OK) &&
			     (AImage_getPlaneData(image, 0, &buf, &len) == AMEDIA_OK) &&
			     (AImage_getPlaneRowStride(image, 0, &row) == AMEDIA_OK) &&
			     (AImage_getPlanePixelStride(image, 0, &px) == AMEDIA_OK);
	if (!meta_ok || planes != 1 || fw <= 0 || fh <= 0 || row <= 0) {
		drop_frame(c, "读 AImage 元数据失败或平面数不是 1", fw, planes);
		return;
	}
	/* RGBA_8888 的 pixelStride 必然是 4。真出别的值就是这台的行为与头不符，
	 * 与其按 4 硬算越界，不如停下来把数报出去。 */
	if (px != 4) {
		drop_frame(c, "RGBA 平面的 pixelStride 不是 4", px, row);
		return;
	}

	int32_t l = crop.left, t = crop.top;
	int32_t cw = crop.right - l, ch = crop.bottom - t;
	if (l < 0) {
		cw += l;
		l = 0;
	}
	if (t < 0) {
		ch += t;
		t = 0;
	}
	if (crop.right > fw)
		cw -= crop.right - fw;
	if (crop.bottom > fh)
		ch -= crop.bottom - fh;
	if (cw <= 0 || ch <= 0) {
		drop_frame(c, "裁剪矩形不成立", (int)crop.right, (int)crop.bottom);
		return;
	}

	const int32_t need = t * row + l * 4 + (ch - 1) * row + cw * 4;
	if (need > len) {
		drop_frame(c, "按裁剪后平面不够读", need, len);
		return;
	}

	out.format = VIDEO_FORMAT_RGBA; // 内存序的依据是文件头第 1 条，不是顺手挑的
	out.width = (uint32_t)cw;
	out.height = (uint32_t)ch;
	out.data[0] = buf + t * row + l * 4;
	out.linesize[0] = (uint32_t)row;
	out.timestamp = os_gettime_ns();
	/* 屏幕上的像素本来就是 full range 的 sRGB，没有"相机那套 8bit YUV 到底算不算 limited"
	 * 的悬案 —— 那件事的属性页下拉是给相机的，这里不问。依据见文件头第 2 条。 */
	out.full_range = true;

	/* 取样时机 = **每一场采集的第一帧** + 之后每 300 帧。按场而不按"这颗源的第一帧"，
	 * 是因为 d 腿在转屏/改分辨率时会重建一块面 —— 新面的 A 分量是一个新问题，
	 * 不该被上一场那块面的读数挡掉。
	 * 隔行隔点取样 —— 全扫一帧 1280x720 是 92 万次读，为这个数不值得。取到 0 就报，
	 * 取到 255 就闭嘴：只在这一帧里所有采样点都是 255 时才不吭声。 */
	if (c->frames_out == c->frames_at_open || (c->frames_out % 300) == 0) {
		uint8_t amin = 255, amax = 0;
		for (int32_t y = 0; y < ch; y += 16) {
			const uint8_t *rp = out.data[0] + (size_t)y * row;
			for (int32_t x = 0; x < cw; x += 16) {
				const uint8_t a = rp[x * 4 + 3];
				if (a < amin)
					amin = a;
				if (a > amax)
					amax = a;
			}
		}
		blog(LOG_INFO,
		     "android-screen: 第 %llu 帧 出图 %dx%d 行距=%d 裁剪=(%d,%d,%d,%d) 源给 %dx%d alpha=[%u,%u]",
		     (unsigned long long)(c->frames_out + 1), (int)cw, (int)ch, row, (int)crop.left, (int)crop.top,
		     (int)crop.right, (int)crop.bottom, fw, fh, (unsigned)amin, (unsigned)amax);
		if (amin == 0)
			blog(LOG_WARNING, "android-screen: 投影面写进来的 alpha 有 0 —— 预览很可能整幅透明，"
					  "这条链采样存储 alpha，见本函数上面那段");
	}

	obs_source_output_video(c->source, &out);

	c->frames_out++;
	if (c->frames_out == 1)
		obs_source_update_properties(c->source); // 属性页那句状态是"打开对话框那一刻"的快照
}

/* reader 那条回调线程。契约与相机一致：不许在这儿碰源以外的东西、
 * 递完帧就把 AImage 还掉（还不掉就是 maxImages 之后一律 acquire 失败）。 */
static void on_image_available(void *context, AImageReader *reader)
{
	struct screen_source *c = context;

	os_atomic_inc_long(&c->in_callback);

	if (os_atomic_load_long(&c->shutting_down) || !reader)
		goto out;

	AImage *image = NULL;
	const media_status_t st = AImageReader_acquireLatestImage(reader, &image);
	if (st != AMEDIA_OK || !image) {
		drop_frame(c, "acquireLatestImage 没拿到帧", (int)st, 0);
		goto out;
	}

	push_frame(c, image);
	AImage_delete(image);

out:
	os_atomic_dec_long(&c->in_callback);
}

/* ===================== P-18-e：系统内录（一路一份的泵） =====================
 *
 * 为什么泵是模块级的一路、不是每颗源一条：总线那头的 AudioPlaybackCapture 与视频那面一样是
 * **全设备一份**（一个令牌、一个环形缓冲），而 read_audio 是**取走**（drain）—— 两颗源各起一条线程
 * 去读得到的不是"两份完整的声音"，是把一段声音劈成两半，每颗都缺一半。所以这里数的是"几颗源要音"：
 * 第一颗把泵起起来，最后一颗走时停掉（与上面 live_sources 同一族理由，那一个数的是令牌、这一个数的是泵）。
 * 代价就是要把手里这一发 PCM 扇给每一颗源 —— audio_sinks 那张表加一把锁。
 *
 * 表里每一格**自带一份引用**（obs_source_get_ref / obs_source_release）：泵是在自己那条线程上调
 * obs_source_output_audio 的，不留引用就等于赌"源不会在我推的这一瞬被销毁"。留了引用，锁就只管表。
 *
 * 节拍交给消费方（这套取舍的原文写在 obs-android.h 那三格上方）：取空就睡 10 ms，不忙轮询。
 * 这一路也没有"多久没声音就算坏"的看门狗 —— 与视频那一路同样的理由：设备上安静一会儿不是故障。
 */

/* 一次最多取这么多帧：48k 下约 21 ms 的量。取空就睡 10 ms，所以正常情况每次都是"有一点就取一点"，
 * 这个数只是上限，不是要求攒够了才给（攒够了反倒是在自己给自己加延迟）。 */
#define AUDIO_READ_FRAMES 1024

static pthread_mutex_t audio_mx = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(obs_source_t *) audio_sinks;
static long audio_users = 0; /* 要音的源数；与 audio_sinks.num 同增同减，都在 audio_mx 里动 */
static pthread_t audio_thread;
static bool audio_thread_started = false;
static os_event_t *audio_stop = NULL;
static int16_t *audio_buf = NULL;
static uint32_t audio_rate = 0, audio_ch = 0;
static uint64_t audio_pushes = 0, audio_frames_total = 0;
/* 峰值这一格是**判"接上了但全是 0"和"根本没接上"的**：帧数只能证明有人在递缓冲，证明不了里面有声音。
 * 上一轮就吃过这个亏 —— 泵跑了 140 秒、推了 650 万帧，录出来的 mp4 整条音轨 -91 dB（纯零）。 */
static int audio_peak_win = 0;  /* 本记录窗口（200 发）内的最大 |样本| */
static int audio_peak_all = 0;  /* 这一场泵见过的最大 |样本| */
static bool audio_nonzero_logged = false;

static void *audio_pump_thread(void *param)
{
	struct obs_source_audio out = {0};
	uint64_t ts = os_gettime_ns();

	UNUSED_PARAMETER(param);

	/* 上面三个静态量都是 start 在锁内写好、pthread_create 之前发布的，这条线程天然看得到 */
	out.data[0] = (uint8_t *)audio_buf;
	out.format = AUDIO_FORMAT_16BIT;
	out.speakers = (audio_ch == 1) ? SPEAKERS_MONO : SPEAKERS_STEREO;
	out.samples_per_sec = audio_rate;

	while (os_event_try(audio_stop) != 0) {
		const uint32_t got = obs_android_screen_read_audio(audio_buf, AUDIO_READ_FRAMES);
		if (got == 0) {
			os_sleep_ms(10); // 取空不是错误，是"此刻还没有"（总线那三格的约定）
			continue;
		}

		out.frames = got;
		out.timestamp = ts;
		/* 时间戳自己按帧数往前推，不用 os_gettime_ns()：后者的抖动会被 libobs 当成
		 * 源自己在加/丢延迟。墙钟只用来起个起点（抄 aaudio-input.c:464-473 那一格）。 */
		ts += (uint64_t)got * 1000000000ULL / (uint64_t)audio_rate;

		int peak = 0;
		for (uint32_t i = 0; i < got * audio_ch; i++) {
			const int v = (audio_buf[i] < 0) ? -(int)audio_buf[i] : (int)audio_buf[i];
			if (v > peak)
				peak = v;
		}
		if (peak > audio_peak_win)
			audio_peak_win = peak;
		if (peak > audio_peak_all) {
			audio_peak_all = peak;
			if (!audio_nonzero_logged) {
				audio_nonzero_logged = true;
				blog(LOG_INFO, "android-screen: 内录第一次量到非零样本（峰值 %d/32767，第 %llu 发）", peak,
				     (unsigned long long)(audio_pushes + 1));
			}
		}

		pthread_mutex_lock(&audio_mx);
		for (size_t i = 0; i < audio_sinks.num; i++)
			obs_source_output_audio(audio_sinks.array[i], &out);
		pthread_mutex_unlock(&audio_mx);

		audio_frames_total += got;
		audio_pushes++;
		if (audio_pushes == 1 || (audio_pushes % 200) == 0) {
			blog(LOG_INFO,
			     "android-screen: 内录第 %llu 发（本批 %u 帧，累计 %llu 帧 ≈ %.2f 秒；这一窗峰值 %d，本场峰值 %d，满量程 32767）",
			     (unsigned long long)audio_pushes, got, (unsigned long long)audio_frames_total,
			     (double)audio_frames_total / (double)audio_rate, audio_peak_win, audio_peak_all);
			audio_peak_win = 0;
		}
	}
	return NULL;
}

/* 只在持锁时调。起不来只是"这一场没有内录"，视频那一路照旧 —— 所以它的失败不进 c->last_error：
 * 那本账记的是"采集开不起来"，混进来会把两件不相干的事说成一件。 */
static bool audio_pump_start(void)
{
	if (audio_thread_started)
		return true;

	uint32_t rate = 0, ch = 0;
	if (!obs_android_screen_start_audio(&rate, &ch)) {
		blog(LOG_WARNING, "android-screen: 系统内录没起来（原因见上一条总线日志），只影响伴音、不影响画面");
		return false;
	}
	if (ch != 1 && ch != 2) {
		/* 交织的 1/2 声道能直接映射进 obs 的 planar 之外的路（S16 交织 = 一个平面），
		 * 3 声道以上没有现成的 layout 可对应，硬按立体声解释就是把声相弄错。宁可不采。 */
		blog(LOG_WARNING, "android-screen: 内录实到 %u 声道，本插件只映射 1/2 声道，这一路不起", (unsigned)ch);
		obs_android_screen_stop_audio();
		return false;
	}

	audio_rate = rate;
	audio_ch = ch;
	audio_buf = bmalloc((size_t)AUDIO_READ_FRAMES * ch * sizeof(int16_t));
	if (os_event_init(&audio_stop, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "android-screen: os_event_init 失败，内录不起");
		goto fail;
	}
	if (pthread_create(&audio_thread, NULL, audio_pump_thread, NULL) != 0) {
		blog(LOG_ERROR, "android-screen: 创建内录线程失败");
		os_event_destroy(audio_stop);
		audio_stop = NULL;
		goto fail;
	}
	audio_thread_started = true;
	blog(LOG_INFO, "android-screen: 系统内录泵已起 %uHz×%u（一次最多取 %d 帧，前端复用缓冲）", (unsigned)rate,
	     (unsigned)ch, AUDIO_READ_FRAMES);
	return true;

fail:
	bfree(audio_buf);
	audio_buf = NULL;
	obs_android_screen_stop_audio(); // 总线那头已经起了 AudioRecord，得跟着一起收
	audio_rate = audio_ch = 0;
	return false;
}

/* 只在持锁时调，且**只由最后一颗源的 release 走到这里**。先 join 线程再动总线那一格：
 * 前端的 stopAudio 会销掉它复用的那枚 short[]（global ref），而读它的人就是这条线程 ——
 * 顺序反了便是"边读边拆"。这个顺序同时也是 OBSAndroidPermissions.cpp 里那句"不上锁"的前提。 */
static void audio_pump_stop(void)
{
	if (audio_thread_started) {
		os_event_signal(audio_stop);
		pthread_join(audio_thread, NULL);
		audio_thread_started = false;
		blog(LOG_INFO, "android-screen: 内录泵已停（共推送 %llu 次 / %llu 帧 ≈ %.2f 秒；本场峰值 %d/32767）",
		     (unsigned long long)audio_pushes, (unsigned long long)audio_frames_total,
		     (double)audio_frames_total / (double)(audio_rate ? audio_rate : 1), audio_peak_all);
	}
	if (audio_stop) {
		os_event_destroy(audio_stop);
		audio_stop = NULL;
	}
	bfree(audio_buf);
	audio_buf = NULL;
	obs_android_screen_stop_audio();
	audio_rate = audio_ch = 0;
	audio_pushes = 0;
	audio_frames_total = 0;
	audio_peak_win = 0;
	audio_peak_all = 0;
	audio_nonzero_logged = false;
}

/* 一颗源开始要音。在 open_capture 成功的那一刻调 —— 与面同生，因为它依附的是同一份令牌。 */
static void audio_pump_acquire(struct screen_source *c)
{
	obs_source_t *ref = obs_source_get_ref(c->source);
	if (!ref) // 源正在销毁：这一格本来就是可省的，不该当成错
		return;

	pthread_mutex_lock(&audio_mx);
	da_push_back(audio_sinks, &ref);
	audio_users++;
	audio_pump_start(); // 起不来就在里面报，这里不改返回值：视频那一路不欠它什么
	pthread_mutex_unlock(&audio_mx);
}

/* 一颗源不再要音。没登记过就是空转（close_capture 既可能从没 acquire 过，也可能被调两遍）。 */
static void audio_pump_release(struct screen_source *c)
{
	pthread_mutex_lock(&audio_mx);
	const size_t idx = da_find(audio_sinks, &c->source, 0);
	if (idx != DARRAY_INVALID) {
		obs_source_t *ref = audio_sinks.array[idx];
		da_erase(audio_sinks, idx);
		obs_source_release(ref);
		if (--audio_users == 0)
			audio_pump_stop();
	}
	pthread_mutex_unlock(&audio_mx);
}

/* 属性页那一行：伴音这条路通不通，用户只能从这里看见（ mixer 表在别的窗口里）。
 * 计数是快照，锁内格式化，不做原子——读的时候顺手把正在写的值读走一次不构成什么。 */
static void audio_state_line(char *out, size_t out_sz)
{
	pthread_mutex_lock(&audio_mx);
	snprintf(out, out_sz, "系统内录：%s，%ld 颗源在要，累计推送 %llu 帧 ≈ %.2f 秒，本场峰值 %d/32767%s。",
		 audio_thread_started ? "泵在跑" : "未起", audio_users, (unsigned long long)audio_frames_total,
		 (double)audio_frames_total / (double)(audio_rate ? audio_rate : 1), audio_peak_all,
		 (audio_thread_started && audio_peak_all == 0) ? "（峰值恒 0 = 系统没把任何声音给过来）" : "");
	pthread_mutex_unlock(&audio_mx);
}

static void close_capture(struct screen_source *c, const char *why)
{
	if (!c->reader)
		return;

	blog(LOG_INFO, "android-screen: 关掉采集（%s）", why);
	os_atomic_set_long(&c->shutting_down, 1);

	/* 第一件事是把自己从那路内录上摘下来（最后一颗时它会 join 泵线程并让前端销缓冲），
	 * 排在收面之前：泵读的是前端那枚复用缓冲，它必须先于缓冲的拆除停住。 */
	audio_pump_release(c);

	/* 先收面再拆 reader：反过来会有一段"令牌还在、面已死"的窗口，
	 * 而 Java 侧那个 display 字段就永远指着一块没主的面。总线那一格没接线时它是空转，安全。 */
	obs_android_screen_stop_display();

	if (c->reader) {
		AImageReader_setImageListener(c->reader, NULL);

		int waited_ms = 0;
		while (os_atomic_load_long(&c->in_callback) > 0 && waited_ms < 2000) {
			os_sleep_ms(1);
			waited_ms++;
		}
		if (os_atomic_load_long(&c->in_callback) > 0) {
			/* 宁漏一次 reader，也不在回调还在场的时候 delete 它。相机那边同样的话，
			 * 同样的理由：AImageReader_delete 不 join 它的回调线程，而回调的 context 就是本结构。 */
			c->cb_thread_maybe_live = true;
			blog(LOG_ERROR, "android-screen: 等 2 秒回调线程仍未退出 onImageAvailable，"
					"这次不 delete reader（宁可泄漏，不制造 use-after-free）");
		} else {
			c->cb_thread_maybe_live = false;
			AImageReader_delete(c->reader);
		}
		c->reader = NULL;
		c->window = NULL; // 归 reader 所有，跟着它一起没了
	}

	c->width = 0;
	c->height = 0;
	os_atomic_set_long(&c->shutting_down, 0);
}

static bool open_capture(struct screen_source *c)
{
	uint32_t w = 0, h = 0, dpi = 0;

	/* 仪器在这一场一开头就起，不起在成功返回前：那样会有一段"帧已经进来了、快照还没打"的窗口，
	 * 判据 frames_out == frames_at_open 会永久错过第一帧。3 秒这条线也从这里开始算 ——
	 * 要量的是"从我们开口要面，到第一帧到手"，建 reader 与接面那两段本来就该算在内。 */
	c->opened_ns = os_gettime_ns();
	c->frames_at_open = c->frames_out;
	c->no_frame_warned = false;

	if (!obs_android_screen_display_size(&w, &h, &dpi)) {
		set_error(c, "问不到屏幕尺寸（总线未接线，或 Java 侧读不到显示模式）");
		return false;
	}

	const media_status_t rst = AImageReader_new((int32_t)w, (int32_t)h, AIMAGE_FORMAT_RGBA_8888, MAX_IMAGES,
						    &c->reader);
	if (rst != AMEDIA_OK || !c->reader) {
		c->reader = NULL;
		set_error(c, "AImageReader_new(%u,%u,RGBA_8888,%d) 失败：media_status=%d", w, h, MAX_IMAGES,
			  (int)rst);
		return false;
	}

	AImageReader_ImageListener listener = {c, on_image_available};
	if (AImageReader_setImageListener(c->reader, &listener) != AMEDIA_OK) {
		set_error(c, "AImageReader_setImageListener 失败");
		close_capture(c, "监听没挂上");
		return false;
	}

	if (AImageReader_getWindow(c->reader, &c->window) != AMEDIA_OK || !c->window) {
		c->window = NULL;
		set_error(c, "拿不到 AImageReader 的 ANativeWindow");
		close_capture(c, "窗口拿不到");
		return false;
	}

	/* 把面接到令牌上。失败的具体原因在 Java 侧那本账里，前端这一腿已经把它打进日志
	 * （"接面 WxH@Ddpi 结果=0 Java 侧=..."），这里不重复抄一遍 —— 抄了也抄不准。 */
	if (!obs_android_screen_start_display(c->window)) {
		set_error(c, "建投影面失败（%ux%u），原因见上一条 '接面' 日志", w, h);
		close_capture(c, "投影面没建成");
		return false;
	}

	c->width = w;
	c->height = h;

	/* 面成了才要音：两者依附的是同一份令牌，反过来（先起 AudioRecord 再建面）就会出现
	 * "音频在录、画面还没接上"的半开态。伴音起不来不改这里的返回值 —— 视频那一路不欠它什么，
	 * 属性页那一行会把"没音"说清楚。 */
	audio_pump_acquire(c);

	blog(LOG_INFO, "android-screen: 采集已开 %ux%u（已向系统要的尺寸），等第一帧", w, h);
	return true;
}

/* 一次"要不要开、能不能开、开不起来就什么时候再试"的判断。activate 与 tick 共用它，
 * 这样同意框刚被用户点掉的那一拍（下一根 tick，最多 1 秒后）画面就自己出来了，
 * 不需要用户再把源关一次开一次。 */
static void try_start(struct screen_source *c)
{
	if (c->reader || os_atomic_load_long(&c->shutting_down))
		return;

	if (!obs_android_screen_ready()) {
		set_error(c, "屏幕总线没接线（前端没注册，或注册晚于建源）");
		return;
	}

	if (!obs_android_screen_has_consent()) {
		/* 一次 episode 只弹一次：这一格下面直接 startActivity 起代理 Activity，
		 * 而代理 Activity 自己**不判重**（它只在 onCreate 里置意图位）—— 不设这道闸，
		 * 60Hz 的 tick 就能把同意框起成一串。
		 * 之后系统把令牌收回（用户按"停止共享"）也不自动再弹：用户刚说过"停"，
		 * 追着弹是骚扰。要再采就把源切走再切回来，activate 会把这个位清掉。 */
		if (!c->consent_asked) {
			c->consent_asked = true;
			const int r = obs_android_screen_request_consent();
			if (r < 0)
				set_error(c, "起同意框失败（%d）—— 拿不到 Context 还是 Activity 起不来，见上一条 Java 侧状态",
					  r);
			else
				blog(LOG_INFO, "android-screen: 还没有投影令牌，已发起同意框（返回 %d）", r);
		}
		return;
	}

	if (!open_capture(c))
		return;

	c->consent_asked = false; // 开成了，下一次令牌丢失时允许再问一次
}

/* 已经在采的时候那一拍要盯的两件事。都在总线那三格里问得出，不用等任何回调：
 * 令牌还在不在、尺寸还是不是当初那一块。 */
static void check_live(struct screen_source *c)
{
	if (!obs_android_screen_has_consent()) {
		/* 令牌没了 —— 用户按了通知栏的"停止共享"，或系统自己收回（Java 侧 onStop 已把格子清空，
		 * 那份日志里能看到是谁收的）。面是建在令牌上的，令牌一走这面就成了无主的面，
		 * 继续挂着一块没人画的 reader 只是白占内存，立刻收。
		 * consent_asked 置上是不追着弹：用户刚当面说过"停"，再弹就是骚扰（与 try_start 里
		 * 那段同一个理由）。要再采就把源切走再切回来 —— activate 会把这个位清掉。 */
		close_capture(c, "令牌已不在（用户停止共享，或系统收回）");
		c->consent_asked = true;
		return;
	}

	uint32_t w = 0, h = 0, dpi = 0;
	if (!obs_android_screen_display_size(&w, &h, &dpi)) {
		/* 问不到尺寸就不动现在这块面：这一格在 Java 侧是读显示模式 + 按旋转档换算的活，
		 * 改分辨率那一瞬确实可能读到个不成立的数。现在的面还在出帧就是还在出帧，下一拍再问。 */
		return;
	}
	if (w == c->width && h == c->height)
		return;

	/* 屏幕这一档的宽高与建面那一刻不一样了。VirtualDisplay 的尺寸是 createVirtualDisplay
	 * 的入参、建成即定死，不会跟着屏幕自己变 —— 唯一的正确处置是按新尺寸重建一块面
	 * （转屏与 `wm size` 改分辨率是同一条路，都收敛到"尺寸对不上"这一个判据上）。
	 *
	 * "重建"这一趟在 35 档上**必然要过第二次同意框**，量出来的：22:37:30.602 这一行落下之后，
	 * 下一拍去 createVirtualDisplay 拿到的是 `SecurityException: … don't use a token that has
	 * timed out`，紧跟着 Java 侧 `系统收回了投影令牌（onStop）` —— 显示配置一变，系统就把令牌收了，
	 * 没有"换个尺寸静默续用同一份令牌"这条路。所以下面不 open_capture 是对的：交给 try_start，
	 * 它会发现没令牌 → 发起同意框 → 用户点头 → 按新尺寸建面（22:39:43 那一遍就是这么回来的）。 */
	blog(LOG_INFO, "android-screen: 屏幕尺寸变了 %ux%u → %ux%u，重建投影面", c->width, c->height, w, h);
	close_capture(c, "屏幕尺寸变化");
	/* 这里不直接 open_capture：这一拍已经把 reader 拆掉，下一拍 reader 为空自然走 try_start，
	 * 由它统一管"能不能开、开不起来什么时候再试"。少一条把失败处理抄两遍的路。 */
}

static const char *screen_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "屏幕采集（MediaProjection）";
}

static void *screen_create(obs_data_t *settings, obs_source_t *source)
{
	struct screen_source *c = bzalloc(sizeof(*c));

	c->source = source;
	os_atomic_inc_long(&live_sources);
	UNUSED_PARAMETER(settings); // 这一轮没有任何用户可调项：尺寸必然是整块屏幕
	return c;
}

static void screen_destroy(void *data)
{
	struct screen_source *c = data;

	close_capture(c, "源销毁");

	/* 最后一颗源走了才把令牌也交还。deactivate 只场面、留令牌，是为了切场景回来不再问用户一次；
	 * 而此刻再没有源会回来了，留着令牌就是替用户挂着一个他以为已经停掉的"正在共享屏幕"
	 * —— 通知栏那条常驻提示 + 前台服务白占 mediaProjection 那一档，两者都算用户看得见的谎。
	 * 这一格必须幂等（总线那侧 stopProjection 是无令牌就静默返回），因为退出时"最后一颗源销毁"
	 * 可能根本轮不到跑（进程直接被杀），那种情况下令牌随进程一起没，系统自己收回。
	 * 放在下面那个提前返回之前：泄漏 data 是一回事，把令牌留在手上是另一回事，别连带着漏。 */
	if (os_atomic_dec_long(&live_sources) == 0) {
		blog(LOG_INFO, "android-screen: 最后一颗采集源已销毁，把投影令牌交还系统");
		obs_android_screen_release_consent();
	}

	if (c->cb_thread_maybe_live) {
		/* reader 回调线程可能还握着 context=&c ⇒ data 不能放。相机那边同一格同样的处置
		 * （camera2-input.c:1425 那段）：故意泄漏，比随机崩在一个已被复用的堆块上便宜。
		 * 正常路径走不到这里 —— 等待环有 2 秒，而回调体里除了 obs_source_output_video 没有别的阻塞点。 */
		blog(LOG_ERROR, "android-screen: 源 '%s' 的回调线程可能还在场 —— 这次不释放 data（故意泄漏）",
		     obs_source_get_name(c->source));
		return;
	}

	bfree(c);
}

static void screen_activate(void *data)
{
	struct screen_source *c = data;

	c->consent_asked = false;
	try_start(c);
}

static void screen_deactivate(void *data)
{
	struct screen_source *c = data;

	close_capture(c, "源停用");
}

static void screen_video_tick(void *data, float seconds)
{
	struct screen_source *c = data;
	const uint64_t now = os_gettime_ns();

	UNUSED_PARAMETER(seconds);

	/* 一道闸管两件事：开不起来时的退避，与已经在采时那一拍一问的轮询。
	 * 轮询每拍两回总线往返（问令牌、问尺寸），尺寸那一格在 Java 侧还要现读一次显示模式；
	 * 按 60Hz 的 tick 跑就是每秒 120 趟，为两件一秒内不会变的事不值这一笔。 */
	if (now < c->next_try_ns)
		return;
	c->next_try_ns = now + RETRY_INTERVAL_NS;

	if (c->reader) {
		if (!c->no_frame_warned && c->frames_out == c->frames_at_open &&
		    (now - c->opened_ns) > FIRST_FRAME_NS) {
			c->no_frame_warned = true;
			blog(LOG_WARNING, "android-screen: 投影面建成 %llu ms 了一帧没收到（面 %ux%u，累计已出 %llu 帧）"
					  "—— 建面的每一步都报成功了，那只剩 SurfaceFlinger 没往这块面上画。"
					  "注意这条只管第一帧：屏幕静止时几秒不出帧是正常的（见文件头第 2 条）",
			     (unsigned long long)((now - c->opened_ns) / 1000000ULL), c->width, c->height,
			     (unsigned long long)c->frames_out);
		}
		check_live(c);
		return;
	}

	try_start(c);
}

static obs_properties_t *screen_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct screen_source *c = data;
	char state[512];
	char audio[192];

	if (c && c->reader)
		snprintf(state, sizeof(state),
			 "正在采集：%ux%u（向系统要的尺寸），已送出 %llu 帧，丢弃 %llu 帧"
			 " —— 这一行是打开本对话框时的快照，只在第一帧到手时刷新一次。",
			 c->width, c->height, (unsigned long long)c->frames_out,
			 (unsigned long long)c->frames_dropped);
	else if (c && c->last_error[0])
		snprintf(state, sizeof(state), "没在采集。最后一次失败原因：%s", c->last_error);
	else if (c)
		snprintf(state, sizeof(state), "没在采集（源未 activate，或还没拿到投影令牌）。已送出 %llu 帧，丢弃 %llu 帧",
			 (unsigned long long)c->frames_out, (unsigned long long)c->frames_dropped);
	else
		snprintf(state, sizeof(state), "总线未接线：拿不到投影令牌，源也没建起来");

	obs_properties_add_text(props, "cap_state", state, OBS_TEXT_INFO);

	/* 内录单独占一行而不是并到上面那句里：它是一路一份的（几颗源共享），把它的账记到
	 * 单颗源那一行会让人以为"这颗源的音频"是另一回事。 */
	audio_state_line(audio, sizeof(audio));
	obs_properties_add_text(props, "cap_audio", audio, OBS_TEXT_INFO);
	return props;
}

struct obs_source_info android_screen_capture_info = {
	.id = "android_screen_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* ASYNC：靠 obs_source_output_video 推裸帧的源必须带，否则 obs_register_source 会因为
	 * "非异步视频源得自己给 get_width/get_height"而拒掉（相机那一路同一条理由）。
	 * AUDIO：e 腿起真的会往 obs_source_output_audio 递帧了才带上 —— 这一格不是装饰：
	 * libobs 拿它决定给不给这颗源建音频通路、挂不挂静音/监听快捷键（obs-source.c:382、
	 * obs-audio.c:47 都读它），不带就是"递了也白递"，带了却没有帧则是混音器里一根不动的表。 */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = screen_getname,
	.create = screen_create,
	.destroy = screen_destroy,
	.activate = screen_activate,
	.deactivate = screen_deactivate,
	.video_tick = screen_video_tick,
	.get_properties = screen_properties,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	obs_register_source(&android_screen_capture_info);

	/* obs_register_source 是 void，被拒只会 blog 一声 ⇒ 自己回读，不然会写出
	 * "libobs 已拒绝、模块谎报成功"的假账（A2 实测过那条坑）。 */
	const uint32_t flags = obs_get_source_output_flags("android_screen_capture");
	if (!flags) {
		blog(LOG_ERROR, "android-screen: 'android_screen_capture' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO, "android-screen: 已注册源类型 'android_screen_capture'（flags=0x%x，总线 ready=%d）",
	     (unsigned)flags, (int)obs_android_screen_ready());
	return true;
}
