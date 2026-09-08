// P-17 内置摄像头采集源（Camera2 NDK）。当前进度：a 权限总线、b 枚举相机、c 取帧进预览
// （AImageReader + capture session → obs_source_output_video）、d 分辨率/FPS 属性（照设备
// 自己那两张表挑档）、e 主界面一键前后置、f 生命周期与释放（退后台交还 + 失联重开，见
// camera2_video_tick）六条腿都在。欠的是真机那侧的判据：色彩对不对、NV12 半平面支、
// 会话参数支、以及"被别的应用抢走相机"这条真实路径（模拟器上撞不出来）。
//
// 五条从 NDK 30 的头注释里量出来的事实（不是推定，写下来免得下次再翻一遍头）：
//   1) 两个 tag 的类型不一样：ACAMERA_LENS_FACING 是 **byte**（头里那行原话
//      "byte (acamera_metadata_enum_android_lens_facing_t)"，NdkCameraMetadataTags.h:3244），
//      ACAMERA_SENSOR_ORIENTATION 是 **int32**（同文件 :5477）。所以读的时候要分两个口：
//      拿 u8 去读 int32 在小端下对"小于 256 的转角"碰巧数值相同 —— 那种能蒙对的代码不留。
//   2) ACameraManager_getCameraIdList 的头注释明说：返回的清单**可能是 Java 侧
//      CameraManager#getCameraIdList 的子集**，"as the NDK API does not support some legacy
//      camera hardware"。⇒ 这里数出来的台数比系统相机 App 能选的少是**设计如此**，
//      不是采集失败，报文案时别说成"没有相机"。
//   3) ACameraManager_openCamera 的 @return 文档里有 ACAMERA_ERROR_PERMISSION_DENIED 这一档
//      ⇒ 没授予 CAMERA 时 open 会明确报权限，不必靠猜。
//   4) 两个 close 的**同步性不一样**：ACameraDevice_close 原话 "Close the connection and free
//      this ACameraDevice synchronously … block until all capture requests … is complete"
//      （NdkCameraDevice.h:196-210），而 ACameraCaptureSession_close 是**异步**的 ——
//      "once all captures have completed and the session has been torn down, onClosed callback
//      will be called and the session will be removed from memory"（NdkCameraCaptureSession.h:440-443），
//      且 onClosed 之后"all access to this ACameraCaptureSession object will cause a crash"（:80）。
//      ⇒ 拆除顺序必须是 session.close() → device.close()（它把在途采集等干净）→ 再别碰 session；
//        并且 session 的回调**不能带 source 指针**（见下面 kSessionCallbacks 那段注释）。
//   5) d 腿量出来的一条：会话参数不是塞 ACameraMetadata，而是塞一个 ACaptureRequest ——
//      ACameraDevice_createCaptureSessionWithSessionParameters 的第 3 个实参原文是
//      `const ACaptureRequest* sessionParameters`（NdkCameraDevice.h:791-796）。所以那对帧率
//      只能走 ACameraDevice_createCaptureRequest + ACaptureRequest_setEntry_i32
//      （NdkCaptureRequest.h:218）；照记忆递一个 metadata 对象进去是编不过的。
//
// 为什么权限要绕一条 libobs 总线、而不是插件自己问：插件 .so 只链 libobs、没有 JavaVM，
// 而 checkSelfPermission 只长在 Java 的 Context 上。总线定义见 libobs/obs-android.h 的
// P-17 那段，注册方是前端的 OBSAndroidPermissions.cpp（不是 android-shell —— 那套 USB/音频
// 宿主至今没进前端，见 plan.md §五 S2 那条待办）。

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>

/* c 腿取帧用：AImageReader（自带一条专用回调线程）与 AImage（平面指针/行距/裁剪矩形）。
   NdkImageReader.h 会带进 ANativeWindow 的声明，不需要单独 include android/native_window.h。 */
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

/* set_error 是 va_list + vsnprintf：stdarg.h 归 va_*，stdio.h 归 vsnprintf。
   两个都不靠 obs-module.h 的间接包含 —— 仓里它带得到、别处换套头就没带，那种"现在能编"不留。 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 用 pthread_mutex_t 就要自己 include：util/threading.h 只在 _WIN32 分支里拉 pthread.h
 * （移植层包在 threading-windows.h 内），POSIX 那一支不拉。 */
#include <pthread.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h> /* os_atomic_*（拆除协议那两个计数靠它）+ pthread_mutex_init_value */

#include <obs-android.h>

#define CAMERA_ID_LEN 64
#define MAX_CAMERAS 16

/* AImageReader 的缓冲数。头里两处把这变成一个硬前提：maxImages 至少 2，
 * acquireLatestImage 才谈得上"丢掉旧的只留最新"（NdkImageReader.h:264-268），
 * 而 acquire 数一旦等于 maxImages 又不 AImage_delete，之后一律 AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED。
 * 4 = 2（latest 的余量）+ 2（在途），不是猜的魔法数，d 腿要加第二路输出时再评估。 */
#define MAX_IMAGES 4

/* “自动”那一档向设备要的尺寸上限。超过它的尺寸多数机型也出得来，但那是要在 CPU 侧整帧拷贝
 * + 着色器转换的像素量，4K 一路预览在手机上会把视频线程拖住。d 腿之后这只是**自动档**的边界：
 * 属性里显式挑了一档（那一档必然是设备自己表里的）就照挑的走，不再拿它夹。 */
#define CAP_MAX_WIDTH  1920
#define CAP_MAX_HEIGHT 1080

/* f 腿那两个时间。
 * FRAME_STALL：一次会话连续这么久没吐出新帧就认定"这条路已经死了"。正常出帧是 33 ms 一帧，
 *   2 秒 = 60 帧的余量，误判不了。而它不是"多加的一道保险"，是**唯一的恢复出口**：
 *   onDisconnected/onError 那两条回调跑在相机框架的线程上、不许碰源（见回调上面那段注释），
 *   所以"设备被收走了"这件事只能由视频线程从这个计数上量出来 —— 失联后的恢复延迟就等于这一个数。
 * RETRY_BACKOFF：重开失败（最常见是别的应用正占着 = ACAMERA_ERROR_CAMERA_IN_USE）之后下一次
 *   尝试的间隔，1 s 起翻倍、封顶 5 s。不给退避的话 tick 会以帧率反复 openCamera：相机是独占设备，
 *   这样敲门既抢不过别人，又会把日志刷爆（plan.md 里 P-8 那笔"日志量"的账不该由我再加一条）。 */
#define FRAME_STALL_NS (2000ULL * 1000000ULL)
#define RETRY_BACKOFF_MIN_MS 1000ULL
#define RETRY_BACKOFF_MAX_MS 5000ULL

/* 一张表能记多少条。真机量到的量级：YUV_420_888 的输出尺寸十档上下、AE 帧率区间三五条，
 * 所以这两个数是宽裕的。超出的条目**丢掉并 WARNING**，不越界写 —— 设备给一张 200 档的表
 * 也只会让属性页变长，不值得为它动态分配。 */
#define MAX_SIZES 32
#define MAX_FPS_RANGES 16

/* 一台相机的能力快照：一次 ACameraManager_getCameraCharacteristics 读全，读完就 free 掉
 * 那份 metadata。建源/开采集/刷属性页三条路都要读，所以只读一次、别一个 tag 一次 get。 */
struct camera2_caps {
	int32_t w[MAX_SIZES];  // 受支持的 YUV_420_888 **输出**尺寸，按面积从大到小
	int32_t h[MAX_SIZES];
	int num_sizes;

	int32_t fps_lo[MAX_FPS_RANGES];  // ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES 拆成的对
	int32_t fps_hi[MAX_FPS_RANGES];
	int num_fps;

	bool fps_is_session_key;  // AE_TARGET_FPS_RANGE 在不在 REQUEST_AVAILABLE_SESSION_KEYS 里
};

struct camera2_info {
	char id[CAMERA_ID_LEN];   // 洗过的：进日志、进属性文本、进 settings，见 scrub_id()
	char raw[CAMERA_ID_LEN];  // 原样：只拿去 ACameraManager_openCamera / getCameraCharacteristics
	int facing;               // ACAMERA_LENS_FACING_*，取不到 -1
	int orientation;          // ACAMERA_SENSOR_ORIENTATION，取不到 -1
};

/* 采集链：全部成员只在 activate 到 deactivate 之间存活（open_capture 建、close_capture 拆）。
 * 谁 owns 谁按头注释写死在下面，别"顺手"多释放一次：
 *   window 归 reader 所有（AImageReader_getWindow 的注释明令不许 ANativeWindow_release）
 *   output/container/target/request 各归本结构，逐个用自己的 free 函数收
 *   device 由 ACameraDevice_close 释放；session 由 ACameraCaptureSession_close 释放（异步）*/
struct camera2_stream {
	AImageReader *reader;
	ANativeWindow *window;
	ACameraDevice *device;
	ACameraCaptureSession *session;
	ACaptureSessionOutput *output;
	ACaptureSessionOutputContainer *container;
	ACameraOutputTarget *target;
	ACaptureRequest *request;
	/* 只给 createCaptureSessionWithSessionParameters 用的那个请求（帧率对当会话参数传进去）。
	 * 头里没写这个 const ACaptureRequest* 的所有权、也没说会话建好后还读不读它，所以按最坏情形
	 * 处理：建会话期间一直留着，等 session/device 都拆干净了再 free（见 close_capture_locked 尾部）。
	 * 没配帧率偏好时它是 NULL。 */
	ACaptureRequest *session_params;

	int32_t width, height; // 问设备要的尺寸（设备可能圆整，实际以每帧 AImage 为准）
	int32_t fps_lo, fps_hi; // 这次实际下发给 AE 的帧率对，0/0 = 没下发（用设备默认）
};

struct camera2_source {
	obs_source_t *source;
	ACameraManager *manager;

	char camera_id[CAMERA_ID_LEN];  // "" = 交给 facing_pref 挑第一台
	int facing_pref;                // -1 不挑 / 0 前 / 1 后
	int color_space_pref;           // enum video_colorspace

	/* d 腿的四件偏好（0 = 自动）。它们只是**意图**：真正下发前一定拿设备自己那张表验一遍，
	 * 表外的一律退回自动档并 WARNING —— 从别的机型拷过来的场景 JSON、或者换了一台相机，
	 * 都会让这里存着一个本机没有的档位，而那不该是"开不起来"。 */
	int32_t cap_w_pref, cap_h_pref;
	int32_t fps_lo_pref, fps_hi_pref;

	/* 打开/拆除互斥。**线程地图（这次逐条量过，别照记忆改）**：
	 *  - update：UI 线程调 obs_source_update 时对视频源只做一件事 —— defer_update_count++
	 *    （obs-source.c:1112-1113），本文件那个 camera2_update 真正跑起来是在
	 *    obs_source_deferred_update（:1093-1101）里，而它由 obs_source_video_tick 调
	 *    （:1410-1411）⇒ **视频线程**。（早先这里写的"update 在 UI 线程"是错的，
	 *    错在把"谁按下的按钮"当成"谁执行的函数"。）
	 *  - activate/deactivate：同一次 obs_source_video_tick 里按 activate_refs 的**跳变**调
	 *    （obs-source.c:1440-1461，那句英文原话 "call activate/deactivate if the reference changed"）
	 *    ⇒ 也是视频线程，且 MAIN_VIEW/AUX_VIEW 两个 view 不会让我开两遍。
	 *  - video_tick（f 腿）：同一根线程、就在上面那两个判定的后面一行（:1463）。
	 *  - destroy：**最后引用在谁手里就由谁执行**，通常是 UI 线程（从场景里删源、关程序）。
	 * ⇒ 锁要防的是"视频线程正在开/拆，另一根线程进来 destroy"这一对，
	 *   而不是 update 与 activate 互撞（那两个天生同线程）。不锁的最坏情形仍是
	 *   "两条线程各自 session 已建、其中一条把另一条的设备关掉"。
	 * 顺带把 f 腿那次要办的记在这儿（已定，别再"顺手"改）：既然拆除在视频线程上，那
	 * close_capture 里那两处阻塞（ACameraDevice_close 原话要等在途采集收尾、外加我自己那个
	 * 2000 ms 等待环）停的就是整个渲染循环。**f 腿量的结果是 133 ms**（b106：改设置那次
	 * 14.789 "采集设置变了" → 14.922 "采集已拆除（update）" → 14.930 重开），
	 * 而拆的触发点只有三个：deactivate、update（含 e 腿那颗按钮）、面已离开（f 腿）。
	 * 三个都是人手点的、或本来就已经在后台的，133 ms = 超出 30 fps 那一帧预算约 4 帧。
	 * ⇒ **不换 win-dshow 那种设备任务线程**：那要多一条线程 + 一套"任务投递 + 关掉时等它退场"的
	 *   协议，换来的只是把这 133 ms 从视频线程挪走，而挪不挪没人看得出。真要翻案的条件写死：
	 *   真机上这三条里有哪条的拆除时间到了几百 ms 量级（尤其 2000 ms 那个等待环被撞上），
	 *   那时候再上设备线程，并且要连同"谁 owns 那次关闭"一起重新算，不是照抄 dshow。
	 * 用 pthread_mutex 而不是"os_mutex" —— 后者在这棵树里根本不存在（util/ 下 grep 不到），
	 * 仓内 C 插件的统一做法就是 pthread_mutex + threading.h:45 的 pthread_mutex_init_value。 */
	pthread_mutex_t mtx;

	struct camera2_stream s;

	/* 拆除协议（c 腿的线程安全就靠这两个）：
	 * shutting_down 置 1 之后回调不再 acquire 新帧；in_callback 是"正在回调里"的计数，
	 * close_capture 等它归零才 AImageReader_delete —— 头里对 delete 的原话是
	 * "Do NOT access the reader object or any of those data pointers after this method returns"
	 * （NdkImageReader.h:110-114），而它**没承诺**会 join 那条回调线程。 */
	volatile long shutting_down;
	volatile long in_callback;

	/* 上面那次"等回调线程退出"超时了就置 true。置起来就不能再 free data —— 那条线程的
	 * context 就是 &c，free 掉之后它下一次进 on_image_available 就是往已释放内存里写。
	 * 后续某次拆除等到过 in_callback==0 就清回 false（那时才是真的安全）。 */
	bool cb_thread_maybe_live;

	uint64_t frames_out;
	uint64_t frames_dropped;

	/* f 腿的失联看门狗。除 frame_seq 外**只有视频线程读写**（tick / activate / deactivate /
	 * update 全在 obs-video.c:82 那根线程上，与 struct 顶上 mtx 那段注释量出来的是同一根），
	 * 所以那几个数既不原子也不上锁。
	 *   frame_seq     —— 唯一跨线程的那个：相机回调线程在 push_frame 里 +1，视频线程在 tick 里读，
	 *                    所以走 os_atomic_*。不复用 frames_out 是因为它是普通 uint64_t、
	 *                    只够写属性页那句文案（读到旧值无妨），而这个数要拿来做"拆了重开"的决定。
	 *   last_seq / last_alive_ns —— 上一次看到 seq 变化（或刚开好一次会话）的时间，看门狗的基准。
	 *   next_retry_ns / retry_ms —— 开不起来时的退避到期时间与当前间隔；都为 0 = 没背着。
	 *   stall_count           —— 连着判了几次失联，只用来给日志限量。 */
	volatile long frame_seq;
	long last_seq;
	uint64_t last_alive_ns;
	uint64_t next_retry_ns;
	uint64_t retry_ms;
	uint32_t stall_count;

	/* 属性页要把"为什么没画面"讲成人话。只写一次一条：多个失败点串起来反而看不出主因。 */
	char last_error[192];
};

static const char *facing_name(int facing)
{
	switch (facing) {
	case ACAMERA_LENS_FACING_FRONT:
		return "前置";
	case ACAMERA_LENS_FACING_BACK:
		return "后置";
	case ACAMERA_LENS_FACING_EXTERNAL:
		return "外接";
	default:
		return "朝向未知";
	}
}

/* 取不到就返回 -1，不猜默认值 —— 属性页里"朝向未知"是有效信息，猜成"后置"会把一台读不到
 * 特性的相机伪装成正常相机。两个口按头注释的类型分开，理由见文件头第 1 条。 */
static int get_byte_tag(const ACameraMetadata *md, uint32_t tag)
{
	ACameraMetadata_const_entry entry;
	if (ACameraMetadata_getConstEntry(md, tag, &entry) != ACAMERA_OK)
		return -1;
	if (entry.count < 1 || !entry.data.u8)
		return -1;
	return (int) entry.data.u8[0];
}

static int get_int32_tag(const ACameraMetadata *md, uint32_t tag)
{
	ACameraMetadata_const_entry entry;
	if (ACameraMetadata_getConstEntry(md, tag, &entry) != ACAMERA_OK)
		return -1;
	if (entry.count < 1 || !entry.data.i32)
		return -1;
	return (int) entry.data.i32[0];
}

/* 相机 id 来自 Framework，是不受控输入：进日志/属性文本前先挡掉控制字符，
 * 否则一条日志能伪造出另一条（本项目对设备侧字符串的统一做法）。 */
static void scrub_id(char *dst, size_t dst_len, const char *src)
{
	size_t i = 0;
	for (; src && src[i] && i + 1 < dst_len; i++)
		dst[i] = (unsigned char) src[i] < 0x20 ? '?' : src[i];
	dst[i] = 0;
}

/* 枚举 + 逐台读静态特性。返回台数，<0 = 清单都没拿到。 */
static int enum_cameras(ACameraManager *manager, struct camera2_info *out, int max)
{
	if (!manager || !out || max <= 0)
		return -1;

	ACameraIdList *ids = NULL;
	const camera_status_t st = ACameraManager_getCameraIdList(manager, &ids);
	if (st != ACAMERA_OK || !ids) {
		blog(LOG_WARNING, "android-camera: 取相机清单失败（camera_status=%d）", (int) st);
		return -1;
	}

	int n = 0;
	for (int i = 0; i < ids->numCameras && n < max; i++) {
		const char *raw_id = ids->cameraIds[i];
		char safe_id[CAMERA_ID_LEN];
		scrub_id(safe_id, sizeof(safe_id), raw_id ? raw_id : "");

		ACameraMetadata *md = NULL;
		if (ACameraManager_getCameraCharacteristics(manager, raw_id, &md) != ACAMERA_OK || !md) {
			blog(LOG_WARNING, "android-camera: 相机 '%s' 读不到静态特性，仍列进清单（朝向按未知）", safe_id);
		}

		struct camera2_info *c = &out[n];
		memcpy(c->id, safe_id, sizeof(c->id));
		if (raw_id) {
			memcpy(c->raw, raw_id, sizeof(c->raw));
			c->raw[sizeof(c->raw) - 1] = 0;
			if (strlen(raw_id) >= sizeof(c->raw))
				blog(LOG_WARNING,
				     "android-camera: 相机 id 长度超过 %d，被截断 —— 这个 id 开不了设备，"
				     "只能换台相机（CAMERA_ID_LEN 加大即可，不是设备问题）", CAMERA_ID_LEN);
		} else {
			c->raw[0] = 0;
		}
		c->facing = md ? get_byte_tag(md, ACAMERA_LENS_FACING) : -1;
		c->orientation = md ? get_int32_tag(md, ACAMERA_SENSOR_ORIENTATION) : -1;

		blog(LOG_INFO, "android-camera: 相机[%d] id='%s' %s（LENS_FACING=%d）传感器转角=%d°", n, c->id,
		     facing_name(c->facing), c->facing, c->orientation);

		if (md)
			ACameraMetadata_free(md);
		n++;
	}

	ACameraManager_deleteCameraIdList(ids);
	return n;
}

static void log_camera_bus_state(const char *when)
{
	if (!obs_android_camera_ready()) {
		blog(LOG_WARNING,
		     "android-camera: %s —— 相机权限总线未注册（宿主 App 没调 obs_android_set_camera_host），"
		     "所以问不到 CAMERA 授予态",
		     when);
		return;
	}
	blog(LOG_INFO, "android-camera: %s CAMERA 权限=%s", when,
	     obs_android_camera_has_permission() ? "已授予" : "未授予");
}

/* ============================ c 腿：开设备、取帧、交回 ============================ */

static void set_error(struct camera2_source *c, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(c->last_error, sizeof(c->last_error), fmt, args);
	va_end(args);
}

/* 打开/配置失败时要能报出人话。属性页里只写一个数字的话，用户分不清"没权限""被系统相机占了"
 * 和"这台机的 NDK 不支持这个尺寸" —— 而那三种的下一步动作完全不同。
 * 这张表是照 NdkCameraError.h:50-142 逐项抄下来的（枚举一共 15 档）。第一稿在这里凭空写了
 * ACAMERA_ERROR_INVALID_DATA 和 ACAMERA_ERROR_TIMEOUT 两个名字 —— 编译器直接拒绝（这份头里没有），
 * 同时把我漏掉的那档 NOT_ENOUGH_MEMORY 暴露出来：手机上它恰恰是最常见的一档（大尺寸 + maxImages
 * 配多了就是这个）。 ⇒ 结论：枚举值一律抄头，不照别的平台的名字推。 */
static const char *camera_status_text(camera_status_t st)
{
	switch ((int) st) {
	case ACAMERA_OK:
		return "成功";
	case ACAMERA_ERROR_INVALID_PARAMETER:
		return "参数不合法（NDK 侧的 bug，不是设备问题）";
	case ACAMERA_ERROR_CAMERA_DISCONNECTED:
		return "相机已断开";
	case ACAMERA_ERROR_METADATA_NOT_FOUND:
		return "读不到元数据";
	case ACAMERA_ERROR_SESSION_CLOSED:
		return "会话已关闭";
	case ACAMERA_ERROR_INVALID_OPERATION:
		return "当前状态不允许这个操作（会话还没起来？）";
	case ACAMERA_ERROR_STREAM_CONFIGURE_FAIL:
		return "流配置被拒（尺寸/格式这台机不支持这么配）";
	case ACAMERA_ERROR_CAMERA_IN_USE:
		return "相机正被别的应用占用";
	case ACAMERA_ERROR_MAX_CAMERA_IN_USE:
		return "系统里同时在用的相机已达上限";
	case ACAMERA_ERROR_CAMERA_DISABLED:
		return "相机被设备策略禁用（企业策略/家长控制）";
	case ACAMERA_ERROR_PERMISSION_DENIED:
		return "没给 CAMERA 权限";
	case ACAMERA_ERROR_UNSUPPORTED_OPERATION:
		return "这台机的相机 HAL 不支持这个调用";
	case ACAMERA_ERROR_NOT_ENOUGH_MEMORY:
		return "相机侧内存不够（尺寸/缓冲数配大了，降一档再看）";
	case ACAMERA_ERROR_CAMERA_DEVICE:
		return "相机 HAL 报了致命错误";
	case ACAMERA_ERROR_CAMERA_SERVICE:
		return "相机服务报了致命错误";
	case ACAMERA_ERROR_UNKNOWN:
	default:
		return "未知错误";
	}
}

/* 一次读全一台相机的能力（尺寸表 + AE 帧率表 + 帧率算不算会话参数）。三条头注释级别的
 * 事实决定了这段的形状：
 *   ① 尺寸表的元组顺序是 (format, width, height, isInput)：声明行
 *      NdkCameraMetadataTags.h:4380 就写着 int32[n*4]，注释原话 "The configurations are
 *      listed as (format, width, height, input?) tuples"。隔壁
 *      ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS 是 int32[n*5] 且顺序
 *      **不一样**（示例伪代码是 width,height,format,isInput,usecase，:4554-4558）
 *      —— 照那边的偏移读这张表，会得到一组张冠李戴的尺寸。
 *   ② AImageReader 的尺寸要是不在这张表里，设备会自己圆整到 1080p 以下
 *      （NdkCameraDevice.h:500-502）。所以"写死 1280x720"在部分机型上根本出不来 720p，
 *      症状还是"选了大的却出个小的"这种比报错难查的。⇒ 这张表既是候选清单也是**校验表**：
 *      d 腿的属性只从表里列档，开采集前再拿表验一遍，表外的一律退回自动。
 *   ③ 帧率表是 int32[2*n]（NdkCameraMetadataTags.h:1403）。头里承诺 LIMITED 以上、非 NIR
 *      滤色阵列的机型一定给 (min,max) 和 (max,max) 两条，且 min<=15、max=最大 YUV 档的
 *      帧率上限（同文件 :1389-1401）—— 那是**设备**的义务，我们照样只照读到的列，
 *      不硬编 30/60。 */
static void caps_add_size(struct camera2_caps *caps, int32_t w, int32_t h)
{
	for (int i = 0; i < caps->num_sizes; i++)
		if (caps->w[i] == w && caps->h[i] == h)
			return; // 同一档会被多个 format 重复列出来，去重后属性页才不是一长串重复
	if (caps->num_sizes >= MAX_SIZES)
		return;
	caps->w[caps->num_sizes] = w;
	caps->h[caps->num_sizes] = h;
	caps->num_sizes++;
}

static void caps_add_fps(struct camera2_caps *caps, int32_t lo, int32_t hi)
{
	if (lo <= 0 || hi <= 0 || lo > hi)
		return; // 怪值不进清单：宁可少一档，也不给一档"挑上就必然配不出会话"的
	for (int i = 0; i < caps->num_fps; i++)
		if (caps->fps_lo[i] == lo && caps->fps_hi[i] == hi)
			return;
	if (caps->num_fps >= MAX_FPS_RANGES)
		return;
	caps->fps_lo[caps->num_fps] = lo;
	caps->fps_hi[caps->num_fps] = hi;
	caps->num_fps++;
}

static bool read_camera_caps(ACameraManager *manager, const struct camera2_info *cam, struct camera2_caps *caps)
{
	memset(caps, 0, sizeof(*caps));
	if (!manager || !cam || !cam->raw[0])
		return false;

	ACameraMetadata *md = NULL;
	if (ACameraManager_getCameraCharacteristics(manager, cam->raw, &md) != ACAMERA_OK || !md) {
		blog(LOG_WARNING, "android-camera: 相机 '%s' 读不到静态特性，尺寸/帧率两张表都拿不到", cam->id);
		return false;
	}

	ACameraMetadata_const_entry e;
	if (ACameraMetadata_getConstEntry(md, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
		if (e.type == ACAMERA_TYPE_INT32 && e.data.i32 && e.count >= 4 && (e.count % 4) == 0) {
			int overflowed = 0;
			for (uint32_t i = 0; i + 3 < e.count; i += 4) {
				if (e.data.i32[i + 0] != AIMAGE_FORMAT_YUV_420_888)
					continue;
				if (e.data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
					continue;
				const int32_t w = e.data.i32[i + 1];
				const int32_t h = e.data.i32[i + 2];
				if (w <= 0 || h <= 0)
					continue; // 头里说 (0,0) 是合法占位项："All non-(0,0) sizes will have non-zero..."
				if (caps->num_sizes >= MAX_SIZES)
					overflowed++;
				else
					caps_add_size(caps, w, h);
			}
			if (overflowed)
				blog(LOG_WARNING, "android-camera: 相机 '%s' 有 %d 档尺寸超出 MAX_SIZES=%d，只列出前 %d 档",
				     cam->id, overflowed, MAX_SIZES, MAX_SIZES);
		} else {
			blog(LOG_WARNING, "android-camera: 相机 '%s' 的流配置类型不对（type=%d count=%u）", cam->id, (int) e.type,
			     (unsigned) e.count);
		}
	} else {
		blog(LOG_WARNING, "android-camera: 相机 '%s' 没有 SCALER_AVAILABLE_STREAM_CONFIGURATIONS", cam->id);
	}

	/* 面积从大到小排：自动档要的就是"最大的那一档"，属性页把高像素放前面也符合直觉。
	 * 表最多 32 条，插入排序足够，不为此引一个 qsort 的比较函数。 */
	for (int i = 1; i < caps->num_sizes; i++) {
		const int32_t w = caps->w[i], h = caps->h[i];
		const int64_t area = (int64_t) w * (int64_t) h;
		int j = i - 1;
		while (j >= 0 && (int64_t) caps->w[j] * (int64_t) caps->h[j] < area) {
			caps->w[j + 1] = caps->w[j];
			caps->h[j + 1] = caps->h[j];
			j--;
		}
		caps->w[j + 1] = w;
		caps->h[j + 1] = h;
	}

	if (ACameraMetadata_getConstEntry(md, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) == ACAMERA_OK) {
		if (e.type == ACAMERA_TYPE_INT32 && e.data.i32 && e.count >= 2 && (e.count % 2) == 0) {
			for (uint32_t i = 0; i + 1 < e.count; i += 2)
				caps_add_fps(caps, e.data.i32[i], e.data.i32[i + 1]);
		} else {
			blog(LOG_WARNING, "android-camera: 相机 '%s' 的 AE 帧率表类型不对（type=%d count=%u）", cam->id,
			     (int) e.type, (unsigned) e.count);
		}
	}

	/* 区间按 (hi, lo) 从小到大排：属性页里这就是一列"从慢到快"，而不是各机型自己给的那个乱序。 */
	for (int i = 1; i < caps->num_fps; i++) {
		const int32_t lo = caps->fps_lo[i], hi = caps->fps_hi[i];
		int j = i - 1;
		while (j >= 0 && (caps->fps_hi[j] > hi || (caps->fps_hi[j] == hi && caps->fps_lo[j] > lo))) {
			caps->fps_hi[j + 1] = caps->fps_hi[j];
			caps->fps_lo[j + 1] = caps->fps_lo[j];
			j--;
		}
		caps->fps_hi[j + 1] = hi;
		caps->fps_lo[j + 1] = lo;
	}

	/* 帧率能不能当**会话参数**下发，只看设备自己给的这张清单（int32[n]，:4079）：
	 * 清单里没有还硬塞，createCaptureSessionWithSessionParameters 就是白失败一次。 */
	if (ACameraMetadata_getConstEntry(md, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, &e) == ACAMERA_OK &&
	    e.type == ACAMERA_TYPE_INT32 && e.data.i32) {
		for (uint32_t i = 0; i < e.count; i++)
			if (e.data.i32[i] == (int32_t) ACAMERA_CONTROL_AE_TARGET_FPS_RANGE)
				caps->fps_is_session_key = true;
	}

	ACameraMetadata_free(md);

	blog(LOG_INFO, "android-camera: 相机 '%s' 能力：%d 档 YUV_420_888 尺寸（最大 %dx%d）、%d 条 AE 帧率区间，"
		       "AE_TARGET_FPS_RANGE %s会话参数清单",
	     cam->id, caps->num_sizes, caps->num_sizes ? (int) caps->w[0] : 0, caps->num_sizes ? (int) caps->h[0] : 0,
	     caps->num_fps, caps->fps_is_session_key ? "在" : "不在");
	return true;
}

/* 自动档的尺寸：<= CAP_MAX_* 里面积最大的一档；一档都没有就退回表里最大的那条并 WARNING。
 * 表已按面积排好，所以第一个满足上限的就是答案。 */
static bool caps_pick_auto_size(const struct camera2_info *cam, const struct camera2_caps *caps, int32_t *out_w,
				int32_t *out_h)
{
	for (int i = 0; i < caps->num_sizes; i++) {
		if (caps->w[i] <= CAP_MAX_WIDTH && caps->h[i] <= CAP_MAX_HEIGHT) {
			*out_w = caps->w[i];
			*out_h = caps->h[i];
			return true;
		}
	}
	if (caps->num_sizes > 0) {
		blog(LOG_WARNING, "android-camera: 相机 '%s' 在 %dx%d 以内没有受支持的 YUV_420_888 尺寸，改用表里最大的 %dx%d",
		     cam->id, CAP_MAX_WIDTH, CAP_MAX_HEIGHT, (int) caps->w[0], (int) caps->h[0]);
		*out_w = caps->w[0];
		*out_h = caps->h[0];
		return true;
	}
	blog(LOG_WARNING, "android-camera: 相机 '%s' 的表里一个 YUV_420_888 输出尺寸都没有", cam->id);
	return false;
}

/* 自动档的帧率：hi 最高的一条，hi 相同再比 lo。为什么 lo 也要比 —— (30,30) 和 (10,30) 的 hi
 * 都是 30，但前者是定帧、后者让 AE 自己往 10 掉，用户看到"30"想要的是前者。 */
static bool caps_pick_auto_fps(const struct camera2_caps *caps, int32_t *out_lo, int32_t *out_hi)
{
	int best = -1;
	for (int i = 0; i < caps->num_fps; i++) {
		if (best < 0 || caps->fps_hi[i] > caps->fps_hi[best] ||
		    (caps->fps_hi[i] == caps->fps_hi[best] && caps->fps_lo[i] > caps->fps_lo[best]))
			best = i;
	}
	if (best < 0)
		return false;
	*out_lo = caps->fps_lo[best];
	*out_hi = caps->fps_hi[best];
	return true;
}

static bool caps_has_size(const struct camera2_caps *caps, int32_t w, int32_t h)
{
	for (int i = 0; i < caps->num_sizes; i++)
		if (caps->w[i] == w && caps->h[i] == h)
			return true;
	return false;
}

static bool caps_has_fps(const struct camera2_caps *caps, int32_t lo, int32_t hi)
{
	for (int i = 0; i < caps->num_fps; i++)
		if (caps->fps_lo[i] == lo && caps->fps_hi[i] == hi)
			return true;
	return false;
}

/* 把"用户想要哪档"落成"这次真下发哪档"。两处的退回理由是同一条：偏好是磁盘上的值、
 * 也可能上一台相机留下的，而这张表是此刻这台相机的 —— 对不上时"开起来"比"开不起来"好，
 * 但一定要留一条 WARNING，否则用户只会觉得"我明明选了 4K"。 */
static bool resolve_capture_size(const struct camera2_info *cam, const struct camera2_caps *caps, int32_t want_w,
				 int32_t want_h, int32_t *out_w, int32_t *out_h)
{
	if (want_w > 0 && want_h > 0) {
		if (caps_has_size(caps, want_w, want_h)) {
			*out_w = want_w;
			*out_h = want_h;
			return true;
		}
		blog(LOG_WARNING, "android-camera: 属性里挑的 %dx%d 不在相机 '%s' 这张表里（换过相机、或场景是从别的机型拷来的）"
				  "，改用自动档",
		     (int) want_w, (int) want_h, cam->id);
	}
	return caps_pick_auto_size(cam, caps, out_w, out_h);
}

static bool resolve_fps_range(const struct camera2_info *cam, const struct camera2_caps *caps, int32_t want_lo,
			      int32_t want_hi, int32_t *out_lo, int32_t *out_hi)
{
	if (want_lo > 0 && want_hi > 0) {
		if (caps_has_fps(caps, want_lo, want_hi)) {
			*out_lo = want_lo;
			*out_hi = want_hi;
			return true;
		}
		blog(LOG_WARNING, "android-camera: 属性里挑的 %d-%d fps 不在相机 '%s' 那张 AE 帧率表里，改用自动档",
		     (int) want_lo, (int) want_hi, cam->id);
	}
	return caps_pick_auto_fps(caps, out_lo, out_hi);
}

/* 定下这次要开哪台。设置里存的是**洗过的** id（属性下拉的值），所以要拿它去对清单里的 id，
 * 再用配上的那条 raw 去开设备 —— 不这么绕一刀的话，要么开着个不存在的 id，要么把
 * 不受控字符串直接递进 NDK。 */
static bool choose_camera(struct camera2_source *c, struct camera2_info *out)
{
	struct camera2_info cams[MAX_CAMERAS];
	const int n = enum_cameras(c->manager, cams, MAX_CAMERAS);
	if (n <= 0) {
		set_error(c, "NDK 侧数不到相机（没授权，或这台只有 NDK 不支持的 legacy 硬件）");
		return false;
	}

	if (c->camera_id[0]) {
		for (int i = 0; i < n; i++) {
			if (strcmp(cams[i].id, c->camera_id) == 0) {
				*out = cams[i];
				return true;
			}
		}
		blog(LOG_WARNING, "android-camera: 设置里的相机 '%s' 不在当前 NDK 清单里（换过设备，"
				  "或它被归成了 NDK 不支持的 legacy 硬件），改用朝向偏好",
		     c->camera_id);
	}

	for (int i = 0; i < n; i++) {
		if (c->facing_pref < 0 || cams[i].facing == c->facing_pref) {
			*out = cams[i];
			return true;
		}
	}

	blog(LOG_WARNING, "android-camera: 清单里没有朝向=%d 的相机，退回第一台 '%s'", c->facing_pref, cams[0].id);
	*out = cams[0];
	return true;
}

static void drop_frame(struct camera2_source *c, const char *why, int32_t v1, int32_t v2)
{
	c->frames_dropped++;
	if (c->frames_dropped <= 5)
		blog(LOG_WARNING, "android-camera: 丢帧：%s（%d,%d）", why, (int) v1, (int) v2);
}

/* 扫一个平面（或平面里的一路色度）的 min/max/avg。px 是 pixelStride：Y 与平面式色度取 1，
 * 交织 UV 里取 2（U 在偶字节、V 在奇字节，起点差 1）。
 * 这件事只为一条判据服务：预览全黑时，"设备给的就是纯色帧"和"我们把有内容的帧转换错了"
 * 在日志里长得一模一样，只能靠字节统计分开 —— 前者 Y 的 min==max，后者 Y 有分布而画面仍黑。
 * 只在打里程碑日志的那几帧跑。 */
static void scan_plane(const uint8_t *p, int32_t row, int32_t w, int32_t h, int32_t px, uint8_t *mn, uint8_t *mx,
		       double *avg)
{
	uint8_t lo = 255, hi = 0;
	unsigned long long sum = 0;
	unsigned long long n = 0;

	for (int32_t r = 0; r < h; r++) {
		const uint8_t *q = p + (size_t) r * (size_t) row;
		for (int32_t x = 0; x < w; x++) {
			const uint8_t v = q[(size_t) x * (size_t) px];
			if (v < lo)
				lo = v;
			if (v > hi)
				hi = v;
			sum += v;
			n++;
		}
	}
	*mn = lo;
	*mx = hi;
	*avg = n ? (double) sum / (double) n : 0.0;
}

/* 一帧 YUV_420_888 → obs_source_output_video。三处必须照设备给的值算，不能想当然：
 *   - 行距 rowStride 可以大于宽度（对齐后的值），所以每行的起点是 y*rowStride；
 *   - U/V 的 pixelStride **不保证是 1**：=2 时是半平面（NV21/NV12 那一族，色度交织）。
 *     这一档直接按 libobs 的 NV12 交出去、**不写反交织循环**，依据是头里那两条保证而不是"这台吐的是 NV12"：
 *     NdkImage.h:162-163 "plane #1 is always U (Cb), plane #2 is always V (Cr)"，NdkImage.h:168-170
 *     "U/V planes are guaranteed to have the same row stride and pixel stride" ⇒ 从**U 平面那个指针**
 *     按 stride 2 读出来必然是 Cb,Cr,Cb,Cr… 这台 HAL 自己存的是 NV12 还是 NV21 都一样（NV21 时系统给
 *     的 U 指针本就落在交织区第 2 个字节上）。**别照"NV21 所以要换 U/V"去'修'它** —— 换了才是色度反相。
 *     少一次全帧遍历拷贝，也少一处只有真机才撞得到的分支；
 *   - 裁剪矩形 right/bottom 是**开区间**，且平面指针加上裁剪偏移后要重新按 dataLength 核边界
 *     （越界读在手机上表现成"预览偶发花一格"，最难查）。
 * 色彩矩阵这一段不是可选的：obs-source.c:2445-2453 无条件把 frame->color_matrix 塞进转换着色器，
 * full_range=false 时连 color_range_min/max 也一起喂（:2454-2458），而着色器第一句就是
 * clamp(yuv, color_range_min, color_range_max)（data/format_conversion.effect:564）
 * ⇒ 全零的 frame 会把每一钳到 0 再乘零矩阵，**画面纯黑**。
 * （struct obs_source_frame out = {0} 这种写法在仓里另有一处：android-capture 的 YUYV 分支，
 *  本轮已照 linux-v4l2/v4l2-input.c:128 那个先例一并修掉，见 plan.md 那条产品账。） */
static void push_frame(struct camera2_source *c, AImage *image)
{
	struct obs_source_frame out = {0};
	int32_t fw = 0, fh = 0, planes = 0;
	int32_t y_row = 0, u_row = 0, v_row = 0, u_px = 0, v_px = 0;
	uint8_t *y = NULL, *u = NULL, *v = NULL;
	int y_len = 0, u_len = 0, v_len = 0;
	AImageCropRect crop = {0};

	bool meta_ok = (AImage_getWidth(image, &fw) == AMEDIA_OK) && (AImage_getHeight(image, &fh) == AMEDIA_OK) &&
		       (AImage_getNumberOfPlanes(image, &planes) == AMEDIA_OK) && (AImage_getCropRect(image, &crop) == AMEDIA_OK) &&
		       (AImage_getPlaneData(image, 0, &y, &y_len) == AMEDIA_OK) &&
		       (AImage_getPlaneRowStride(image, 0, &y_row) == AMEDIA_OK) &&
		       (AImage_getPlaneData(image, 1, &u, &u_len) == AMEDIA_OK) &&
		       (AImage_getPlaneRowStride(image, 1, &u_row) == AMEDIA_OK) &&
		       (AImage_getPlanePixelStride(image, 1, &u_px) == AMEDIA_OK);
	if (!meta_ok || planes != 3 || fw <= 0 || fh <= 0 || y_row <= 0 || u_row <= 0) {
		drop_frame(c, "读 AImage 元数据失败或平面数不是 3", fw, planes);
		return;
	}

	/* 裁剪矩形夹进真实尺寸，并保证宽高为偶（YUV 4:2:0 + libobs 的 NV12 纹理都要求 2 对齐，
	 * graphics.c:2921 那句报错就是给这件事兜底的）。 */
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
	if (l + cw > fw)
		cw = fw - l;
	if (t + ch > fh)
		ch = fh - t;
	cw &= ~1;
	ch &= ~1;
	if (cw <= 0 || ch <= 0) {
		drop_frame(c, "裁剪矩形与帧尺寸不相交", crop.right, crop.bottom);
		return;
	}

	const int32_t half_cw = cw / 2, half_ch = ch / 2;
	const int32_t y_base = l + t * y_row;
	if (y_base + (ch - 1) * y_row + cw > y_len) {
		drop_frame(c, "Y 平面按裁剪后不够读", y_base, y_len);
		return;
	}

	const bool planar = (u_px == 1);
	if (planar) {
		if (AImage_getPlaneData(image, 2, &v, &v_len) != AMEDIA_OK ||
		    AImage_getPlaneRowStride(image, 2, &v_row) != AMEDIA_OK ||
		    AImage_getPlanePixelStride(image, 2, &v_px) != AMEDIA_OK || v_px != 1 || v_row <= 0) {
			drop_frame(c, "V 平面读不到或不是全平面（pixelStride!=1）", u_px, v_px);
			return;
		}
		const int32_t u_base = (l / 2) + (t / 2) * u_row;
		const int32_t v_base = (l / 2) + (t / 2) * v_row;
		if (u_base + (half_ch - 1) * u_row + half_cw > u_len ||
		    v_base + (half_ch - 1) * v_row + half_cw > v_len) {
			drop_frame(c, "色度平面按裁剪后不够读", u_len, v_len);
			return;
		}

		out.format = VIDEO_FORMAT_I420;
		out.data[0] = y + y_base;
		out.data[1] = u + u_base;
		out.data[2] = v + v_base;
		out.linesize[0] = (uint32_t) y_row;
		out.linesize[1] = (uint32_t) u_row;
		out.linesize[2] = (uint32_t) v_row;
	} else if (u_px == 2) {
		/* 半平面：一个交织的 UV 平面，一行是 half_cw 个采样点 × 2 字节 = cw 字节。
		 * 边界只核到最后一个 **U 字节**（uv_base + (half_ch-1)*u_row + 2*(half_cw-1)），
		 * 不把末尾的 V 字节算进 u_len：真机实测（HA2Q0SW1，1920x1080）U 平面报的
		 * dataLength = 宽*高/2 - 1 = 1036799，比"按 U 指针读满整帧"正好少 1 ——
		 * 交织缓冲的最后一个字节是 V 平面的，长度各报各的。按 NdkImage.h 的保证
		 * （U/V 同 row/pixel stride、pixelStride=2 即同一块交织分配），那个字节就在
		 * 同一块内存里，读它不越界；把 1 字节之差当成"不够读"会把每一帧都丢掉
		 * （症状：送出 0 帧、看门狗每 2 秒按失联重开）。 */
		const int32_t uv_base = 2 * (l / 2) + (t / 2) * u_row;
		if (uv_base + (half_ch - 1) * u_row + 2 * (half_cw - 1) >= u_len) {
			drop_frame(c, "交织 UV 平面按裁剪后不够读", uv_base, u_len);
			return;
		}

		out.format = VIDEO_FORMAT_NV12;
		out.data[0] = y + y_base;
		out.data[1] = u + uv_base;
		out.linesize[0] = (uint32_t) y_row;
		out.linesize[1] = (uint32_t) u_row;
	} else {
		drop_frame(c, "色度 pixelStride 既不是 1 也不是 2", u_px, planes);
		return;
	}

	out.width = (uint32_t) cw;
	out.height = (uint32_t) ch;
	out.timestamp = os_gettime_ns();
	/* Android 相机给的 8bit YUV 按 limited(16-235) 处理；真机上若出现系统性色偏，
	 * 属性页那个"色彩矩阵"下拉就是留给这件事的（改档不用重打包）。
	 * 这与 plan.md §五 uvc-input.c 那条同源，也与 §六 的编码像素色彩语义是同一笔账。 */
	out.full_range = false;
	if (!video_format_get_parameters_for_format((enum video_colorspace) c->color_space_pref, VIDEO_RANGE_PARTIAL,
						    out.format, out.color_matrix, out.color_range_min,
						    out.color_range_max))
		blog(LOG_WARNING, "android-camera: 色彩矩阵取不到（cs=%d fmt=0x%x），这一次按 601 兜底",
		     c->color_space_pref, (unsigned) out.format);

	obs_source_output_video(c->source, &out);

	c->frames_out++;
	os_atomic_inc_long(&c->frame_seq); // f 腿看门狗读它，见 struct 上那段
	if (c->frames_out == 1)
		/* 属性页那句状态是"打开对话框那一刻"的快照，改完档位它自己不会变。第一帧到手时主动刷一次，
		 * 用户当场就看得到"我挑的那档真出来了没有"。就这一次，不租定时器：get_properties 要重数
		 * 相机、重读能力表。这里是 reader 那条回调线程，而该函数只是发一个信号
		 * （obs-source.c:1129-1135），真去建属性对象的是 UI 线程；仓内先例是 linux-v4l2 在
		 * 设备热插拔回调里刷（v4l2-input.c:739、:760）。 */
		obs_source_update_properties(c->source);

	if (c->frames_out == 1 || (c->frames_out % 300) == 0) {
		uint8_t ymn, ymx, umn, umx, vmn, vmx;
		double yavg, uavg, vavg;
		blog(LOG_INFO, "android-camera: 第 %llu 帧 出图 %dx%d fmt=%s 行距 Y=%d U=%d 裁剪=(%d,%d,%d,%d) "
			       "源给 %dx%d",
		     (unsigned long long) c->frames_out, (unsigned) cw, (unsigned) ch,
		     planar ? "I420" : "NV12", y_row, u_row, crop.left, crop.top, crop.right, crop.bottom, fw, fh);

		/* 统计的是交出去的那块区域（裁剪后、按各自行距与 pixelStride 走），
		 * 跟 obs_source_output_video 看到的是同一批字节。半平面时 V 落在 U 起点 +1。 */
		scan_plane(out.data[0], y_row, cw, ch, 1, &ymn, &ymx, &yavg);
		scan_plane(out.data[1], u_row, half_cw, half_ch, planar ? 1 : 2, &umn, &umx, &uavg);
		scan_plane(planar ? out.data[2] : out.data[1] + 1, planar ? v_row : u_row, half_cw, half_ch,
			   planar ? 1 : 2, &vmn, &vmx, &vavg);
		blog(LOG_INFO, "android-camera: 第 %llu 帧 内容 Y %u~%u 均%.1f | U %u~%u 均%.1f | V %u~%u 均%.1f"
			       "（Y 的 min==max 就是这台设备给的是纯色帧，不是转换的问题）",
		     (unsigned long long) c->frames_out, (unsigned) ymn, (unsigned) ymx, yavg, (unsigned) umn,
		     (unsigned) umx, uavg, (unsigned) vmn, (unsigned) vmx, vavg);
	}
}

/* AImageReader 的 onImageAvailable。头里两处关于线程与"拿不到帧"的话直接照办：
 *   "The callback happens on one dedicated thread per AImageReader instance. It is okay to use
 *    AImageReader_* and AImage_* methods within the callback."（NdkImageReader.h:316-319）
 *    ⇒ 不再自己建 ALooper，回调里就地 acquire/转换。
 *   回调里 acquire 返回 AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE 是**正常可能**的
 *    （:320-324：多帧排队时前一条回调已把帧取走）⇒ 这一档不报警，否则日志会被刷满。
 * 用 acquireLatestImage 而不是 NextImage：它会把更旧的帧直接退回系统，所以"一条回调取一帧"
 * 就能跟得上生产；用 NextImage 的话积压会长成"越 delay 越大直到整条流卡死"（:206-211 原话）。*/
static void on_image_available(void *context, AImageReader *reader)
{
	struct camera2_source *c = context;

	os_atomic_inc_long(&c->in_callback);

	if (os_atomic_load_long(&c->shutting_down))
		goto out;

	AImage *image = NULL;
	const media_status_t st = AImageReader_acquireLatestImage(reader, &image);
	if (st != AMEDIA_OK || !image) {
		if (st != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
			blog(LOG_WARNING, "android-camera: acquireLatestImage 失败 media_status=%d", (int) st);
		goto out;
	}

	push_frame(c, image);
	AImage_delete(image);

out:
	os_atomic_dec_long(&c->in_callback);
}

/* 设备与会话的回调：c 腿一律**不碰 source 指针**，context 传 NULL，只留一条日志。
 * 原因不是洁癖 —— 这些回调跑在相机框架的线程上，而 deactivate/destroy 可能在同一瞬间
 * 走完 close 并 bfree(data)。把 c 带进回调就等于自己造一个 use-after-free。
 * f 腿落地的答案是"**回调仍然只报日志，恢复由视频线程那侧量出来**"（见 camera2_video_tick 里
 * 那段失联看门狗）：头里没有一个 unregister 接口，能依靠的只有"ACameraDevice_close 返回后不该再有
 * 回调"这一条，而那要是不成立就是崩在一个已复用的堆块上 —— 拿日志换稳定性是划算的。
 * 代价说清楚：撞上失联后最多 FRAME_STALL_NS（这里定的 2 秒）才恢复，不是即时。 */
static void on_device_disconnected(void *context, ACameraDevice *device)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(device);
	blog(LOG_WARNING, "android-camera: 相机设备已断开（被别的应用抢走/策略变化）—— 本次采集作废，"
			  "失联看门狗会在 %u ms 内自己重开", (unsigned)(FRAME_STALL_NS / 1000000ULL));
}

/* error 的取值：NdkCameraDevice.h:119-125 只给了五个 @see，指向 Java 的
 * CameraDevice.StateCallback.ERROR_CAMERA_IN_USE / MAX_CAMERAS_IN_USE / CAMERA_DISABLED /
 * CAMERA_DEVICE / CAMERA_SERVICE（NDK 侧没有对应枚举，数值沿用 Java 那边：1/2/3/4/5）。
 * 所以这串数字是"照 Java 文档解释"，不是从这份 NDK 头里读出来的 —— 真机上第一次撞上时要以日志为准复核。 */
static void on_device_error(void *context, ACameraDevice *device, int error)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(device);
	blog(LOG_WARNING, "android-camera: 相机设备报错 error=%d（1=被占 2=数量上限 3=被禁用 "
			  "4=设备致命 5=服务致命）", error);
}

static void on_session_closed(void *context, ACameraCaptureSession *session)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(session);
	blog(LOG_INFO, "android-camera: 采集会话已关闭");
}

static void on_session_ready(void *context, ACameraCaptureSession *session)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(session);
}

static void on_session_active(void *context, ACameraCaptureSession *session)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(session);
}

/* 这次采集到底开没开。只问那三个句柄、不另立一个"我开着呢"的布尔位：多一个标志就是多一处
 * 会和 close_capture_locked 说不同话的地方（而 open_capture_locked 的失败分支自己走
 * close_capture_locked 收尾，"半开"这种状态留不到外面来）。 */
static bool capture_is_open(const struct camera2_source *c)
{
	return c->s.reader || c->s.device || c->s.session || c->s.request;
}

/* 看门狗重新计时。开好一次会话就挂一次 —— 首帧本来就比后续慢（AE 起来、缓冲填满），
 * 不挂的话刚开好的那次会被下一拍当成失联，白拆一遍。 */
static void arm_watchdog(struct camera2_source *c)
{
	c->last_seq = os_atomic_load_long(&c->frame_seq);
	c->last_alive_ns = os_gettime_ns();
}

static void close_capture_locked(struct camera2_source *c, const char *why)
{
	struct camera2_stream *s = &c->s;

	if (!s->reader && !s->device && !s->session && !s->request)
		return;

	os_atomic_set_long(&c->shutting_down, 1);

	if (s->session) {
		const camera_status_t st = ACameraCaptureSession_stopRepeating(s->session);
		if (st != ACAMERA_OK)
			blog(LOG_WARNING, "android-camera: stopRepeating 返回 %d（%s）", (int) st, camera_status_text(st));
		/* 异步：这里返回后框架还在收尾，之后**不许再碰 session 指针**（onClosed 之后
		 * 访问它会直接崩，NdkCameraCaptureSession.h:80）。 */
		ACameraCaptureSession_close(s->session);
		s->session = NULL;
	}

	if (s->device) {
		const camera_status_t st = ACameraDevice_close(s->device);
		if (st != ACAMERA_OK)
			blog(LOG_WARNING, "android-camera: ACameraDevice_close 返回 %d（%s）", (int) st,
			     camera_status_text(st));
		s->device = NULL;
	}

	if (s->reader) {
		AImageReader_setImageListener(s->reader, NULL);

		int waited_ms = 0;
		while (os_atomic_load_long(&c->in_callback) > 0 && waited_ms < 2000) {
			os_sleep_ms(1);
			waited_ms++;
		}
		if (os_atomic_load_long(&c->in_callback) > 0) {
			/* 宁漏一次 reader，也不在回调还在场的时候 delete 它 —— AImageReader_delete
			 * 不承诺 join 它那条回调线程（见 struct camera2_source 上那段）。
			 * 泄漏一次远比一次随机崩溃便宜。
			 * 而且这次连带把 data 也标成"不许 free"：回调的 context 就是 &c。 */
			c->cb_thread_maybe_live = true;
			blog(LOG_ERROR, "android-camera: 等 2 秒回调线程仍未退出 onImageAvailable，"
					"这次不 delete reader（宁可泄漏，不制造 use-after-free）");
		} else {
			c->cb_thread_maybe_live = false;
			AImageReader_delete(s->reader);
		}
		s->reader = NULL;
		s->window = NULL; // 归 reader 所有，跟着它一起没了
	}

	if (s->request) {
		ACaptureRequest_free(s->request);
		s->request = NULL;
	}
	if (s->session_params) {
		/* 放在 session/device 之后：头里没说建好会话之后还读不读这个 const ACaptureRequest*，
		 * 那就按"可能还在读"留到最后一步再 free。 */
		ACaptureRequest_free(s->session_params);
		s->session_params = NULL;
	}
	if (s->target) {
		ACameraOutputTarget_free(s->target);
		s->target = NULL;
	}
	if (s->output) {
		ACaptureSessionOutput_free(s->output);
		s->output = NULL;
	}
	if (s->container) {
		ACaptureSessionOutputContainer_free(s->container);
		s->container = NULL;
	}

	/* f 腿：走到这里是一次**有人要求的**关闭（deactivate / update / 面已离开 / destroy /
	 * 打开失败回退），而退避是给"开不起来"准备的。不在这儿清干净的话，
	 * 上一轮抢不过别的应用攒下的 5 秒会原样带到"回前台要重开"那一次上。 */
	c->next_retry_ns = 0;
	c->retry_ms = 0;

	blog(LOG_INFO, "android-camera: 采集已拆除（%s）—— 送出 %llu 帧，丢弃 %llu 帧", why,
	     (unsigned long long) c->frames_out, (unsigned long long) c->frames_dropped);
}

static bool open_capture_locked(struct camera2_source *c)
{
	struct camera2_stream *s = &c->s;
	struct camera2_info cam;

	if (!c->manager) {
		set_error(c, "没有 ACameraManager（这台机没有 Camera2，或创建失败）");
		return false;
	}
	if (!choose_camera(c, &cam))
		return false;
	if (!cam.raw[0]) {
		/* enum_cameras 允许"清单里有这一台、但 Framework 给的 id 指针为 NULL"这种项（它照样列出来，
		 * 只是 raw 是空串）。放在建 reader 之前，return false 才不会漏掉已创建的 AImageReader。 */
		set_error(c, "相机 '%s' 的原始 id 是空的（Framework 没给 id），开不了设备", cam.id);
		return false;
	}

	if (obs_android_camera_ready() && !obs_android_camera_has_permission()) {
		set_error(c, "未授予 CAMERA 权限，开不了相机");
		blog(LOG_WARNING, "android-camera: %s", c->last_error);
		return false;
	}

	c->last_error[0] = 0;
	os_atomic_set_long(&c->shutting_down, 0);

	/* 一次读全这台相机的两张表（尺寸 + AE 帧率），下面挑档和校验都只照着它来。
	 * 读不到表就不硬开：表外的尺寸设备会静默圆整到 1080p 以下，症状是"选了大的却出个小的"，
	 * 比直接报错难查得多。 */
	struct camera2_caps caps;
	if (!read_camera_caps(c->manager, &cam, &caps)) {
		set_error(c, "相机 '%s' 读不到能力表，配不出采集尺寸", cam.id);
		return false;
	}

	int32_t w = 0, h = 0;
	if (!resolve_capture_size(&cam, &caps, c->cap_w_pref, c->cap_h_pref, &w, &h)) {
		set_error(c, "相机 '%s' 没给出可用的 YUV_420_888 输出尺寸", cam.id);
		return false;
	}

	/* 帧率那张表可能压根没有（LEGACY 级设备允许不给）：那就**不下发**、用模板请求自带的默认档，
	 * 而不是把源开不起来 —— 少一帧率偏好照样能出画面。 */
	int32_t flo = 0, fhi = 0;
	const bool have_fps = resolve_fps_range(&cam, &caps, c->fps_lo_pref, c->fps_hi_pref, &flo, &fhi);
	if (!have_fps)
		blog(LOG_WARNING, "android-camera: 相机 '%s' 没给 AE 帧率表，这次不下发 CONTROL_AE_TARGET_FPS_RANGE，"
				  "按设备默认帧率跑", cam.id);

	bool ok = true;
	ACameraDevice_StateCallbacks dev_cb = {NULL, on_device_disconnected, on_device_error, NULL};
	/* 上面这个初始化按 ACameraDevice_StateCallbacks 的字段顺序写：
	 * context / onDisconnected / onError / onClientSharedAccessPriorityChanged。
	 * 最后一个只有共享模式（API 36）才会用到，给 NULL 是对的。 */
	AImageReader_ImageListener listener = {c, on_image_available};
	ACameraCaptureSession_stateCallbacks sess_cb = {
		NULL, on_session_closed, on_session_ready, on_session_active};

	ok = (AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, MAX_IMAGES, &s->reader) == AMEDIA_OK) && s->reader;
	if (!ok) {
		set_error(c, "AImageReader_new(%d,%d) 失败", (int) w, (int) h);
		goto fail;
	}
	if (AImageReader_setImageListener(s->reader, &listener) != AMEDIA_OK) {
		set_error(c, "AImageReader_setImageListener 失败");
		ok = false;
		goto fail;
	}
	if (AImageReader_getWindow(s->reader, &s->window) != AMEDIA_OK || !s->window) {
		set_error(c, "拿不到 AImageReader 的 ANativeWindow");
		ok = false;
		goto fail;
	}

	const camera_status_t ost = ACameraManager_openCamera(c->manager, cam.raw, &dev_cb, &s->device);
	if (ost != ACAMERA_OK || !s->device) {
		/* 只调这一次：openCamera 的返回值就是"为什么开不了"的唯一出处。
		 * 之前这里写成"链式判断丢掉返回码、失败后再调一次拿码"—— 那等于真发了两次打开请求，
		 * 第二次可能成功却把设备指针落进一个局部变量里泄漏掉（相机是独占设备，代价是别人拿不到）。 */
		set_error(c, "开相机 '%s' 失败：%s（camera_status=%d）", cam.id, camera_status_text(ost), (int) ost);
		blog(LOG_ERROR, "android-camera: %s", c->last_error);
		ok = false;
		goto fail;
	}

	if (ACaptureSessionOutputContainer_create(&s->container) != ACAMERA_OK) {
		set_error(c, "建输出容器失败");
		ok = false;
		goto fail;
	}
	if (ACaptureSessionOutput_create(s->window, &s->output) != ACAMERA_OK) {
		set_error(c, "把窗口包成 session output 失败");
		ok = false;
		goto fail;
	}
	if (ACaptureSessionOutputContainer_add(s->container, s->output) != ACAMERA_OK) {
		set_error(c, "窗口加不进容器");
		ok = false;
		goto fail;
	}

	/* 帧率这一档官方**强烈建议**在建会话时就给："the application is strongly recommended to call
	 * ACameraDevice_createCaptureSessionWithSessionParameters with the target fps range before
	 * creating the capture session … helps avoid session reconfiguration delays in cases like 60fps"
	 * （NdkCameraMetadataTags.h:723-730）。但那句话的前提是这把 key 在设备的
	 * REQUEST_AVAILABLE_SESSION_KEYS 清单里，所以两头都满足才走这条路。传进去的是一个只装了这对值、
	 * 不带任何 target 的 ACaptureRequest —— 第 3 实参的类型原文是 const ACaptureRequest*
	 * （NdkCameraDevice.h:791-796），不是 ACameraMetadata。 */
	if (have_fps && caps.fps_is_session_key) {
		const int32_t init_pair[2] = {flo, fhi};
		if (ACameraDevice_createCaptureRequest(s->device, TEMPLATE_RECORD, &s->session_params) == ACAMERA_OK &&
		    s->session_params) {
			if (ACaptureRequest_setEntry_i32(s->session_params, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, init_pair) !=
			    ACAMERA_OK) {
				blog(LOG_WARNING, "android-camera: 把 %d-%d fps 写进会话参数失败，这次只当普通请求下发",
					 (int) flo, (int) fhi);
				ACaptureRequest_free(s->session_params);
				s->session_params = NULL;
			}
		} else {
			s->session_params = NULL;
			blog(LOG_WARNING, "android-camera: 建会话参数用的请求失败，这次只当普通请求下发帧率");
		}
	}

	/* 会话配置失败是**同步**从这个调用返回 STREAM_CONFIGURE_FAIL 的（stateCallbacks 里
	 * 压根没有 onConfigureFailure 可挂），所以错误码一定要接住并翻成人话。两条路的错误码同源，
	 * 所以把"走的哪条"写进下面那句 set_error，而不是写进两条分支各报一次。 */
	camera_status_t sst;
	if (s->session_params)
		sst = ACameraDevice_createCaptureSessionWithSessionParameters(s->device, s->container, s->session_params,
											&sess_cb, &s->session);
	else
		sst = ACameraDevice_createCaptureSession(s->device, s->container, &sess_cb, &s->session);
	if (sst != ACAMERA_OK || !s->session) {
		set_error(c, "建采集会话失败：%s（camera_status=%d，%dx%d + %d-%dfps 这组配不出来%s）",
			  camera_status_text(sst), (int) sst, (int) w, (int) h, (int) flo, (int) fhi,
			  s->session_params ? "，帧率是按会话参数下发的" : "");
		ok = false;
		goto fail;
	}

	if (ACameraDevice_createCaptureRequest(s->device, TEMPLATE_RECORD, &s->request) != ACAMERA_OK || !s->request) {
		set_error(c, "建 TEMPLATE_RECORD 请求失败");
		ok = false;
		goto fail;
	}
	/* 同一个窗口要在两边各登记一次：session 那边收的是 ACaptureSessionOutput，
	 * request 这边要另建一个 ACameraOutputTarget。少了后者不会有报错，只会一直不出帧
	 * （NdkCaptureRequest.h:67-71：request 的 target 必须是 session 窗口清单的子集）。 */
	if (ACameraOutputTarget_create(s->window, &s->target) != ACAMERA_OK || !s->target) {
		set_error(c, "建 ACameraOutputTarget 失败");
		ok = false;
		goto fail;
	}
	if (ACaptureRequest_addTarget(s->request, s->target) != ACAMERA_OK) {
		set_error(c, "把窗口加进请求失败");
		ok = false;
		goto fail;
	}

	/* 帧率最终一定要出现在**重复请求**里：会话参数只影响"建会话时先按哪档配"，之后每帧的 AE
	 * 约束看的是请求，而 TEMPLATE_RECORD 自带的那一档不一定是我们挑的。
	 * 下发失败不算"开不起来"：只会退回默认帧率、画面照样出，所以只 WARNING 不 goto fail。
	 * 也别把这档当承诺 —— 头里明说实际可达帧率 = min(aeTargetFpsRange.maxFps, 1/各流 minFrameDuration)
	 * （NdkCameraMetadataTags.h:714-720），"选了 60 却只有 30"多半是那档尺寸本身出不了 60。 */
	if (have_fps) {
		const int32_t pair[2] = {flo, fhi};
		const camera_status_t fst =
			ACaptureRequest_setEntry_i32(s->request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, pair);
		if (fst != ACAMERA_OK)
			blog(LOG_WARNING, "android-camera: 往重复请求里下发 %d-%d fps 失败：%s（这次按设备默认帧率跑）",
				 (int) flo, (int) fhi, camera_status_text(fst));
	}

	ACaptureRequest *reqs[1] = {s->request};
	const camera_status_t rst = ACameraCaptureSession_setRepeatingRequest(s->session, NULL, 1, reqs, NULL);
	if (rst != ACAMERA_OK) {
		set_error(c, "发起循环采集失败：%s（camera_status=%d）", camera_status_text(rst), (int) rst);
		ok = false;
		goto fail;
	}

	s->width = w;
	s->height = h;
	s->fps_lo = have_fps ? flo : 0;
	s->fps_hi = have_fps ? fhi : 0;
	c->frames_out = 0;
	c->frames_dropped = 0;
	arm_watchdog(c); // 首帧慢是常态（AE 起来、缓冲填满），基准必须跟着这次重开走

	/* 把"这次真的在用哪颗镜头"回写进源的 facing。理由：camera_id 允许留空走自动档，所以"当前是前置
	 * 还是后置"只有这里知道，而主界面那颗一键前后置（P-17-e）必须先知道现状才翻得对 —— 与其让前端
	 * 另存一份朝向、两边各说各话，不如把这份真相写回设置这一个地方。
	 * 只在真的不一样时才写：自动档解析出来的结果通常就等于 facing_pref，没必要每次开相机都脏一次存档。
	 * 收敛性：下一次 update 读到的 facing 就是这颗，camera_id 仍为空 ⇒ 按朝向挑，挑到同一颗。
	 * 直接改 obs_source_get_settings 拿到的那个对象（不是副本）有上游先例：win-dshow.cpp:1074 的
	 * DShowInput::SetActive 就是这么把 active 写回自己源的设置的。 */
	obs_data_t *live = obs_source_get_settings(c->source);
	if (live) {
		if (obs_data_get_int(live, "facing") != (long long)cam.facing)
			obs_data_set_int(live, "facing", cam.facing);
		obs_data_release(live);
	}

	/* "走没走会话参数"要能在日志里看出来：同一档帧率走两条路，撞上的症状（配不出会话、
	 * 起来头几帧掉帧）归因不一样，而属性页上这两条路看起来是完全一样的。 */
	char fps_text[64];
	if (!have_fps)
		snprintf(fps_text, sizeof(fps_text), "不下发（设备默认）");
	else
		snprintf(fps_text, sizeof(fps_text), "%d-%d（%s）", (int) flo, (int) fhi,
			 s->session_params ? "会话参数 + 重复请求" : "只重复请求");

	blog(LOG_INFO, "android-camera: 源 '%s' 开始采集 —— 相机='%s' %s 转角=%d° 尺寸=%dx%d 帧率=%s "
			       "格式=YUV_420_888 缓冲 maxImages=%d",
	     obs_source_get_name(c->source), cam.id, facing_name(cam.facing), cam.orientation, (int) w, (int) h,
	     fps_text, MAX_IMAGES);
	return true;

fail:
	close_capture_locked(c, "打开失败回退");
	return false;
}

/* 四个调用点（activate / deactivate / update / destroy）都走这两个外壳。
 * 锁的粒度就是"一次完整的开或拆"，不在锁里跑采集回调 —— 那条线程从来不拿这把锁。 */
static bool open_capture(struct camera2_source *c)
{
	pthread_mutex_lock(&c->mtx);
	const bool ok = open_capture_locked(c);
	pthread_mutex_unlock(&c->mtx);
	return ok;
}

static void close_capture(struct camera2_source *c, const char *why)
{
	pthread_mutex_lock(&c->mtx);
	close_capture_locked(c, why);
	pthread_mutex_unlock(&c->mtx);
}

/* ---------------------------- f 腿：生命周期 ---------------------------- */
/* 三条规矩，都是量出来的：
 *   1) **"此刻该不该占着相机"不由本文件判断**。答案在相机总线那格 capture_allowed 里，
 *      写它的是前端的 Qt applicationStateChanged（libobs/obs-android.h 的 P-17 段 +
 *      frontend/widgets/OBSAndroidPermissions.cpp）。本文件只按答案开/拆，不自己记一份
 *      "现在是前台还是后台"—— 与 e 腿那颗前后置按钮同一个形状：真相一份，别处声明。
 *      为什么必须交还：Activity 停到后台后窗口没了而设备还占着，下次进应用就是
 *      ACAMERA_ERROR_CAMERA_IN_USE（本条是 plan.md 里 P-17 立案时点名的硬约束）。
 *   2) **这一段和 activate/deactivate/update 在同一根线程上**：obs_source_video_tick 里
 *      "activate_refs 跳变就调 activate/deactivate"（obs-source.c:1440-1461）与末尾那句
 *      video_tick 是一次调用的前后脚，而它整体跑在 obs-video.c 的视频线程上。
 *      ⇒ 从这里开/拆不引入新的锁序，也不会和 update() 抢；与 c 腿那两条相机框架线程的
 *      区别正在这里（那两条不许碰源，见上面的回调注释）。
 *   3) **重开只在源 active 时做**。obs-video.c:66-71 那个循环把 tick 发给**所有**源，
 *      不看 active 也不看 showing，所以这一条得自己判 —— 漏了就会长出"没在预览里却占着相机"
 *      这种谁也看不见的占用，而 camera2_update 里躲的正是同一种状态。 */
static void camera2_video_tick(void *data, float seconds)
{
	struct camera2_source *c = data;
	const uint64_t now = os_gettime_ns();
	const bool allowed = obs_android_camera_capture_allowed();

	UNUSED_PARAMETER(seconds);

	/* 1) 交还：HOME / 切应用 / 锁屏。没开着的时候 close_capture 自己空转，所以不必先判 open。 */
	if (!allowed && capture_is_open(c)) {
		close_capture(c, "面已离开");
		set_error(c, "应用退到后台，已把相机交还（回前台自动重开）");
		return;
	}

	/* 2) 认活：这一拍比上一拍多出了帧，就把基准、退避、失联计数一起清零。 */
	const long seq = os_atomic_load_long(&c->frame_seq);
	if (seq != c->last_seq) {
		c->last_seq = seq;
		c->last_alive_ns = now;
		c->stall_count = 0;
		c->retry_ms = 0;
		c->next_retry_ns = 0;
	}

	/* 3) 失联：开着却连续 FRAME_STALL_NS 不出帧就拆掉，重开交给下面第 4 支（这一拍就走到它）。
	 *    这是 onDisconnected/onError 唯一的出口 —— 那两个回调不许碰源，只能由这里量。
	 *    限量：连着失联只报前三次，之后一句收尾，免得真坏了就把日志刷满。 */
	if (capture_is_open(c) && now - c->last_alive_ns > FRAME_STALL_NS) {
		const unsigned silent_ms = (unsigned)((now - c->last_alive_ns) / 1000000ULL);
		if (c->stall_count < 3)
			blog(LOG_WARNING, "android-camera: 源 '%s' 已 %u ms 没吐新帧（第 %u 次），按失联拆掉重开",
			     obs_source_get_name(c->source), silent_ms, c->stall_count + 1);
		else if (c->stall_count == 3)
			blog(LOG_WARNING, "android-camera: 源 '%s' 还在同一个失联循环里，后面不再一条条报",
			     obs_source_get_name(c->source));
		c->stall_count++;
		close_capture(c, "失联");
		set_error(c, "%u ms 没出帧，已按失联拆掉重开", silent_ms);
		/* 不回 return：接着让第 4 支试一次重开。arm_watchdog 在开成功时才挂，
		 * 所以这里不会当场又判一次失联（那时 capture_is_open 已经是 false）。 */
	}

	/* 4) 重开：该采、没开、源在预览里、且退避到期。 */
	if (allowed && !capture_is_open(c) && obs_source_active(c->source) && now >= c->next_retry_ns) {
		if (open_capture(c)) {
			c->retry_ms = 0;
			c->next_retry_ns = 0;
		} else {
			c->retry_ms = c->retry_ms ? (c->retry_ms * 2ULL > RETRY_BACKOFF_MAX_MS ? RETRY_BACKOFF_MAX_MS
											       : c->retry_ms * 2ULL)
						  : RETRY_BACKOFF_MIN_MS;
			c->next_retry_ns = now + c->retry_ms * 1000000ULL;
			blog(LOG_INFO, "android-camera: 源 '%s' 开不起来（%s），%llu ms 后再试",
			     obs_source_get_name(c->source), c->last_error[0] ? c->last_error : "(没记下原因)",
			     (unsigned long long) c->retry_ms);
		}
	}
}

/* ------------------------------- 源接口 ------------------------------- */

static const char *camera2_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "内置摄像头（Camera2）";
}

static void camera2_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "camera_id", "");
	obs_data_set_default_int(settings, "facing", -1);
	obs_data_set_default_int(settings, "colorspace", VIDEO_CS_DEFAULT);
	/* d 腿：这两把都是**串**而不是拆成四个 int —— 属性下拉是一把 key 对一个控件，而串本身人眼可读
	 * （"1280x720" / "10-30"）。空串 = 自动。 */
	obs_data_set_default_string(settings, "cap_size", "");
	obs_data_set_default_string(settings, "fps", "");
}

static void camera2_load_settings(struct camera2_source *data, obs_data_t *settings)
{
	scrub_id(data->camera_id, sizeof(data->camera_id), obs_data_get_string(settings, "camera_id"));

	const long long f = obs_data_get_int(settings, "facing");
	data->facing_pref = (f == ACAMERA_LENS_FACING_FRONT || f == ACAMERA_LENS_FACING_BACK) ? (int) f : -1;

	/* settings 是磁盘上可被手改的输入：越界的 colorspace 会让 push_frame 里那次
	 * video_format_get_parameters_for_format 走到没有分支的 default、拿不到矩阵，
	 * 症状正是文件头说的"画面纯黑"。所以按枚举边界夹回 DEFAULT，不照单全收。 */
	const long long cs = obs_data_get_int(settings, "colorspace");
	data->color_space_pref = (cs >= VIDEO_CS_DEFAULT && cs <= VIDEO_CS_2100_HLG) ? (int) cs : (int) VIDEO_CS_DEFAULT;

	/* 分辨率/帧率两档同样是被手改过的磁盘输入：解析不出来（空串 = 自动、写了"auto"、只写了一个数）
	 * 一律归 0/0 = 自动档，而不是报错。至于"这一档这台相机到底有没有"，那是 resolve_* 拿着设备表
	 * 去校验的事 —— 走到这里都还没定这次要开哪台。 */
	int cw = 0, ch = 0;
	if (sscanf(obs_data_get_string(settings, "cap_size"), "%dx%d", &cw, &ch) != 2 || cw <= 0 || ch <= 0)
		cw = ch = 0;
	data->cap_w_pref = cw;
	data->cap_h_pref = ch;

	int flo = 0, fhi = 0;
	if (sscanf(obs_data_get_string(settings, "fps"), "%d-%d", &flo, &fhi) != 2 || flo <= 0 || fhi <= 0 || flo > fhi)
		flo = fhi = 0;
	data->fps_lo_pref = flo;
	data->fps_hi_pref = fhi;
}

static void *camera2_create(obs_data_t *settings, obs_source_t *source)
{
	struct camera2_source *data = bzalloc(sizeof(*data));
	data->source = source;
	data->facing_pref = -1;
	/* 不用 PTHREAD_MUTEX_INITIALIZER 赋初值：那是 windows 的 pthread 移植层
	 * 过不了初始化器（threading.h 就是为这件事提供 init_value 的）。 */
	pthread_mutex_init_value(&data->mtx);
	if (pthread_mutex_init(&data->mtx, NULL) != 0)
		blog(LOG_ERROR, "android-camera: 建锁失败，开/拆采集就没有互斥了");
	camera2_load_settings(data, settings);

	data->manager = ACameraManager_create();
	if (!data->manager) {
		blog(LOG_ERROR, "android-camera: ACameraManager_create() 失败 —— 这台没有 Camera2？");
		return data;
	}

	/* 建源就数一遍：b 腿的全部产品行为就是"数得出、报得清"，属性页也拿这份清单。 */
	struct camera2_info cams[MAX_CAMERAS];
	const int n = enum_cameras(data->manager, cams, MAX_CAMERAS);
	blog(LOG_INFO, "android-camera: 源 '%s' 建起来，NDK 侧数到 %d 台相机", obs_source_get_name(source), n);

	log_camera_bus_state("建源");
	return data;
}

static void camera2_destroy(void *data)
{
	struct camera2_source *c = data;
	if (!c)
		return;
	/* 顺序是有讲究的：设备是从 manager 上开的，所以先把采集拆干净（session → device → reader），
	 * 再关 manager。反过来的话这里就是拿一个已释放的 manager 去关设备。
	 * deactivate 正常走完时这里是空转（close_capture 见什么都没建就直接返回）。 */
	close_capture(c, "destroy");
	if (c->cb_thread_maybe_live) {
		/* 那条 reader 回调线程可能还握着 context=&c ⇒ data、锁、manager 一样都不能放。
		 * 这是故意泄漏：一台机上一次、每次撞上都会有这条 ERROR 日志，比随机崩在
		 * 一个已被复用的堆块上便宜得多。（正常路径永远走不到这里：等待环只有 2 秒，
		 * 而回调体里除了 obs_source_output_video 没有别的阻塞点。） */
		blog(LOG_ERROR, "android-camera: 源 '%s' 的回调线程可能还在场 —— 这次不释放 data/锁/manager（故意泄漏）",
		     obs_source_get_name(c->source));
		return;
	}
	if (c->manager)
		ACameraManager_delete(c->manager);
	pthread_mutex_destroy(&c->mtx);
	bfree(c);
}

static void camera2_activate(void *data)
{
	struct camera2_source *c = data;
	blog(LOG_INFO, "android-camera: 源 '%s' activate，选中相机='%s'（朝向偏好=%d）",
	     obs_source_get_name(c->source), c->camera_id[0] ? c->camera_id : "(未指定)", c->facing_pref);
	log_camera_bus_state("activate");

	if (!open_capture(c))
		blog(LOG_ERROR, "android-camera: 源 '%s' activate 后开不起来：%s", obs_source_get_name(c->source),
		     c->last_error[0] ? c->last_error : "(没记下原因)");
}

static void camera2_deactivate(void *data)
{
	struct camera2_source *c = data;
	blog(LOG_INFO, "android-camera: 源 '%s' deactivate", obs_source_get_name(c->source));
	close_capture(c, "deactivate");
}

static void camera2_update(void *data, obs_data_t *settings)
{
	struct camera2_source *c = data;
	char old_id[CAMERA_ID_LEN];
	const int old_facing = c->facing_pref;
	const int32_t old_w = c->cap_w_pref, old_h = c->cap_h_pref;
	const int32_t old_flo = c->fps_lo_pref, old_fhi = c->fps_hi_pref;

	memcpy(old_id, c->camera_id, sizeof(old_id));
	camera2_load_settings(c, settings);

	const bool id_changed = strcmp(old_id, c->camera_id) != 0;
	const bool facing_changed = old_facing != c->facing_pref;
	const bool size_changed = old_w != c->cap_w_pref || old_h != c->cap_h_pref;
	const bool fps_changed = old_flo != c->fps_lo_pref || old_fhi != c->fps_hi_pref;

	if (!id_changed && !facing_changed && !size_changed && !fps_changed)
		return; // 色彩偏好是 push_frame 每帧读的，不用重建会话

	/* 换相机要换那台独占设备，换尺寸要重配流（reader 和 session 都是按尺寸建的），换帧率虽然在
	 * 重复请求里就能改、但**会话参数**只有重开会话才谈得上 —— 三件并成一条"拆了重开"，日志里那句
	 * 变了什么写全，别只报相机名。 */
	blog(LOG_INFO, "android-camera: 采集设置变了：相机 '%s'→'%s' 朝向 %d→%d 尺寸 %dx%d→%dx%d 帧率 %d-%d→%d-%d"
		       "（0/空 = 自动）", old_id, c->camera_id, old_facing, c->facing_pref, (int) old_w, (int) old_h,
		       (int) c->cap_w_pref, (int) c->cap_h_pref, (int) old_flo, (int) old_fhi, (int) c->fps_lo_pref,
		       (int) c->fps_hi_pref);

	/* 换相机 = 换独占设备，只能拆了重开；正在采集才要重开，没 activate 的时候留着不动，
	 * 让 activate 那条路径去开（否则会出现"没在预览里却占着相机"这种谁也看不见的占用）。 */
	if (obs_source_active(c->source)) {
		close_capture(c, "update");
		if (!open_capture(c))
			blog(LOG_ERROR, "android-camera: 源 '%s' 改设置后重开失败：%s", obs_source_get_name(c->source),
			     c->last_error[0] ? c->last_error : "(没记下原因)");
	}
}

/* 属性页上的【申请相机权限】。用 add_button2 而不是 add_button —— 后者在
 * obs-properties.h:217 已标 OBS_DEPRECATED。priv 传的是 data，而属性页可能在建源之前打开
 * （那时 data 为 NULL），所以这里要判空。
 * 总线契约里 request_permission "不得阻塞等用户点框"，所以本函数返回时权限还没定；
 * 返回 false（不刷属性页）是故意的 —— 立刻刷新只会再显示一次"未授予"，反而像是按钮没反应。 */
static bool on_request_permission(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct camera2_source *c = data;

	const int r = obs_android_camera_request_permission();
	const char *what = r == 0 ? "本来就已授予（弹窗不会出现）"
				  : r == 1 ? "已发起，系统弹窗出来了 —— 选完后重开本属性页才会刷新"
					   : "发起失败：拿不到 Activity，或已被系统永久拒绝（要去 设置>应用>权限 里开）";
	blog(LOG_INFO, "android-camera: 属性页点【申请相机权限】，总线返回 %d —— %s", r, what);
	if (c)
		set_error(c, "申请权限：%s", what);
	return false;
}

static obs_properties_t *camera2_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct camera2_source *c = data;
	struct camera2_info cams[MAX_CAMERAS];

	/* 三措辞：总线未接通、没授权、有授权但数不到相机，是三种不同的下一步。合成一句就会
	 * 把用户支到错误的地方去（USB/音频那两条总线当年各踩过一次）。 */
	if (!obs_android_camera_ready()) {
		obs_properties_add_text(props, "bus_state",
					"（相机权限总线未接通：宿主 App 没注册 obs_android_set_camera_host）",
					OBS_TEXT_INFO);
	} else if (!obs_android_camera_has_permission()) {
		obs_properties_add_text(props, "perm_state",
					"未授予 CAMERA 权限。也可以直接点下面那个按钮再问一次；"
					"若首启那次已经选了【不再询问】，就只能去【设置 > 应用 > OBS Studio > 权限】里打开。",
					OBS_TEXT_INFO);
		obs_properties_add_button2(props, "request_perm", "申请相机权限", on_request_permission, c);
	}

	/* 属性页可能在建源之前就被打开（右键菜单→属性），那时 data 为 NULL，
	 * 所以自己临时开一个 manager，用完必须自己关 —— 下面 own_manager 就是记这笔。 */
	ACameraManager *manager = c ? c->manager : ACameraManager_create();
	const bool own_manager = c ? false : true;
	const int n = manager ? enum_cameras(manager, cams, MAX_CAMERAS) : -1;

	obs_property_t *list = obs_properties_add_list(props, "camera_id", "相机", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	if (n < 0) {
		obs_property_list_add_string(list, "（拿不到相机清单）", "");
	} else if (n == 0) {
		obs_property_list_add_string(list, "（NDK 侧没有可用相机：没授权，或只有 NDK 不支持的 legacy 硬件）", "");
	} else {
		for (int i = 0; i < n; i++) {
			char text[CAMERA_ID_LEN + 96];
			snprintf(text, sizeof(text), "%s '%s'（转角 %d°）", facing_name(cams[i].facing), cams[i].id,
				 cams[i].orientation);
			obs_property_list_add_string(list, text, cams[i].id);
		}
		obs_property_list_add_string(list, "（自动：按下面的朝向偏好挑第一台）", "");
	}

	obs_property_t *facing = obs_properties_add_list(props, "facing", "朝向偏好", OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(facing, "不挑", -1);
	obs_property_list_add_int(facing, "后置", ACAMERA_LENS_FACING_BACK);
	obs_property_list_add_int(facing, "前置", ACAMERA_LENS_FACING_FRONT);

	/* d 腿的两张下拉只列设备表里的档 —— 用的就是开采集时那同一把尺子（choose_camera +
	 * read_camera_caps），所以"属性里列得出来的"和"开采集开得出来的"必然是同一批。读表只要
	 * getCameraCharacteristics，它不碰设备（b98 在 MuMu 上量过：开着属性页不会把相机占住）。
	 * c==NULL 时（属性页先于建源被打开）没有偏好可解析，也就不知道读哪一台的表 —— 那只能给自动档。 */
	struct camera2_caps caps;
	bool have_caps = false;
	if (c && manager) {
		struct camera2_info sel;
		if (choose_camera(c, &sel))
			have_caps = read_camera_caps(manager, &sel, &caps);
	}

	char auto_size[96];
	snprintf(auto_size, sizeof(auto_size), "（自动：%d×%d 以内挑最大的一档）", CAP_MAX_WIDTH, CAP_MAX_HEIGHT);

	obs_property_t *sizes = obs_properties_add_list(props, "cap_size", "分辨率", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	if (have_caps && caps.num_sizes > 0) {
		obs_property_list_add_string(sizes, auto_size, "");
		for (int i = 0; i < caps.num_sizes; i++) {
			char label[96];
			char val[32];
			snprintf(val, sizeof(val), "%dx%d", (int) caps.w[i], (int) caps.h[i]);
			snprintf(label, sizeof(label), "%s（%.1f MP）", val,
				 (double) caps.w[i] * (double) caps.h[i] / 1000000.0);
			obs_property_list_add_string(sizes, label, val);
		}
	} else {
		obs_property_list_add_string(sizes, "（自动：这台读不到尺寸表，只能自动）", "");
	}

	obs_property_t *fpsl = obs_properties_add_list(props, "fps", "帧率", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	if (have_caps && caps.num_fps > 0) {
		obs_property_list_add_string(fpsl, "（自动：表里帧率上限最高的一档）", "");
		for (int i = 0; i < caps.num_fps; i++) {
			char label[96];
			char val[24];
			snprintf(val, sizeof(val), "%d-%d", (int) caps.fps_lo[i], (int) caps.fps_hi[i]);
			if (caps.fps_lo[i] == caps.fps_hi[i])
				snprintf(label, sizeof(label), "%s fps（定帧）", val);
			else
				snprintf(label, sizeof(label), "%s fps（AE 在这个区间里自己降）", val);
			obs_property_list_add_string(fpsl, label, val);
		}
	} else {
		obs_property_list_add_string(fpsl, "（自动：这台没给 AE 帧率表，按设备默认跑）", "");
	}

	/* 色彩矩阵留成可改的，是因为"Android 相机给的 8bit YUV 到底算不算 limited range"这件事
	 * 只有真机能定（见 push_frame 那段注释与 plan.md 里 uvc-input 那条同源待办）。
	 * 默认值写死成 DEFAULT，撞色偏时不用重打包。 */
	obs_property_t *cs = obs_properties_add_list(props, "colorspace", "色彩矩阵", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cs, "自动（按 Rec. 709）", VIDEO_CS_DEFAULT);
	obs_property_list_add_int(cs, "Rec. 601", VIDEO_CS_601);
	obs_property_list_add_int(cs, "Rec. 709", VIDEO_CS_709);

	if (c) {
		/* 这几个计数是采集线程在写、UI 线程在读：读到旧值甚至读到半个 64 位都不影响判断，
		 * 它只用来回答"到底有没有帧进来"，不当账算。 */
		char state[512];
		char asked_fps[48];
		if (c->s.fps_hi > 0)
			snprintf(asked_fps, sizeof(asked_fps), "%d-%dfps", (int) c->s.fps_lo, (int) c->s.fps_hi);
		else
			snprintf(asked_fps, sizeof(asked_fps), "默认帧率");
		if (c->s.reader && c->s.session)
			snprintf(state, sizeof(state),
				 "正在采集：%dx%d %s（都是向设备要的值，设备可能圆整），已送出 %llu 帧，丢弃 %llu 帧"
				 " —— 这一行是打开本对话框时的快照，只在第一帧到手时刷新一次。",
				 (int) c->s.width, (int) c->s.height, asked_fps, (unsigned long long) c->frames_out,
				 (unsigned long long) c->frames_dropped);
		else if (c->last_error[0])
			snprintf(state, sizeof(state), "没在采集。最后一次失败原因：%s", c->last_error);
		else
			snprintf(state, sizeof(state), "没在采集（源未 activate）。已送出 %llu 帧，丢弃 %llu 帧",
				 (unsigned long long) c->frames_out, (unsigned long long) c->frames_dropped);
		obs_properties_add_text(props, "cap_state", state, OBS_TEXT_INFO);
	}

	if (own_manager && manager)
		ACameraManager_delete(manager);

	return props;
}

struct obs_source_info camera2_input_info = {
	.id = "android_camera_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* 与 UVC 那路同一条理由：靠 obs_source_output_video 推裸帧的异步源必须带 ASYNC，
	 * 否则 obs_register_source 会因为"非异步视频源得自己给 get_width/get_height"而拒掉。 */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = camera2_getname,
	.create = camera2_create,
	.destroy = camera2_destroy,
	.activate = camera2_activate,
	.deactivate = camera2_deactivate,
	/* f 腿的驱动：交还/重开/失联重建全在这一拍里（见 camera2_video_tick 上面那三条规矩）。
	 * 它必须挂在 video_tick 而不是别的钩子上 —— 源没有"面变了"这类回调，而视频线程
	 * 是唯一一根既天天回来、又已经被允许碰采集句柄的线程。 */
	.video_tick = camera2_video_tick,
	.update = camera2_update,
	.get_defaults = camera2_get_defaults,
	.get_properties = camera2_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	obs_register_source(&camera2_input_info);

	/* obs_register_source 是 void，被拒只会 blog 一声 ⇒ 自己回读，不然会写出
	 * "libobs 已拒绝、模块谎报成功"的假账（A2 实测过那条坑）。 */
	const uint32_t flags = obs_get_source_output_flags("android_camera_input");
	if (!flags) {
		blog(LOG_ERROR, "android-camera: 'android_camera_input' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO, "android-camera: 已注册源类型 'android_camera_input'（flags=0x%x）", (unsigned) flags);

	/* 模块加载时就数一遍并打日志 —— 这是 b 腿的验收判据，且它不依赖前端建源，
	 * 所以在没有 UI 参与的一轮里也能读到证据。 */
	ACameraManager *m = ACameraManager_create();
	if (m) {
		struct camera2_info cams[MAX_CAMERAS];
		const int n = enum_cameras(m, cams, MAX_CAMERAS);
		blog(LOG_INFO, "android-camera: 模块加载时数到 %d 台相机", n);
		ACameraManager_delete(m);
	} else {
		blog(LOG_ERROR, "android-camera: ACameraManager_create() 失败，模块加载时数不到相机");
	}
	log_camera_bus_state("模块加载");

	return true;
}
