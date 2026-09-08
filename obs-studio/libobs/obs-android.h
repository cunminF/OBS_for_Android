#pragma once

#include "obs-defs.h"
#include "util/c99defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android 无 root 的 USB / 音频设备通路（plan.md 第四节 阶段 2）。
 *
 * 约束：插件是纯 C 的 .so，只链 libobs，拿不到 Qt 也没有 JavaVM；而 USB 设备只能
 * 由 Java 侧 UsbManager 打开（openDevice → getFileDescriptor）才有 fd，音频输入清单
 * 只能由 Java 侧 AudioManager 读到（且 Android 10 起读输入设备属性要 RECORD_AUDIO）。
 * 所以中间需要一条总线：App 启动时把一组回调注册进 libobs，插件通过 libobs 调用它们。
 *
 * 为什么放在 libobs 而不是让插件自己反向找宿主：实测到的是插件 .so 的 NEEDED 只有
 * libc/libdl/libm/liblibobs（A3 用 llvm-readobj 核过），即 libobs 是插件唯一的公共依赖；
 * "插件 dlsym(RTLD_DEFAULT) 够不到壳主 .so 的符号"则是按 Android 的加载语义推的
 * （没做反查实验）。本方案只依赖前半句 —— 走 libobs 是链接期就能解析的，
 * .qoder/check-m3-syms.sh 能静态查到，因此不需要成立后半句。
 *
 * 线程约定：get_devices 可能在 libobs 的视频线程（源的 update/activate）或
 * UI 线程（obs_get_source_properties）上被调，实现方必须自己保证线程安全，并且
 * **不得阻塞等用户点授权弹窗** —— 那是异步的，见 obs_android_usb_request_permission()。
 */

#define OBS_ANDROID_USB_NAME_LEN 64
#define OBS_ANDROID_USB_LABEL_LEN 160

/** 一个 USB 设备的快照。字段全是值类型，为的是让回调把数据拷进调用者的缓冲，
 *  不用操心跨语言的对象生命周期。 */
struct obs_android_usb_device {
	/** UsbDevice.getDeviceName()，形如 /dev/bus/usb/001/002，也是 open 的键 */
	char name[OBS_ANDROID_USB_NAME_LEN];
	/** 给人看的名字，形如 "1d6b:0103 USB Camera（未授权）" */
	char label[OBS_ANDROID_USB_LABEL_LEN];
	/** UsbManager.hasPermission() */
	bool has_permission;
};

/** App（壳）注册进来的回调表。任何一项都可以是 NULL，libobs 的包装函数会按"失败"处理。 */
struct obs_android_usb_host {
	void *param;

	/** 填 out[0..max-1]，返回实际设备数（>=0），拿不到时返回 -1。 */
	int (*get_devices)(void *param, struct obs_android_usb_device *out, int max);

	/** 0=已经有权限，1=已弹出系统授权框（结果异步，之后重新 get_devices 看变化），-1=失败。 */
	int (*request_permission)(void *param, const char *name);

	/** UsbDeviceConnection.getFileDescriptor() 的结果；失败返回 -1（含用户拒授权）。 */
	int (*open_fd)(void *param, const char *name);

	/** 关掉 open_fd 拿到的连接。libusb 侧不需要它，但 fd 泄漏在 Android 上会占住设备。 */
	void (*close_fd)(void *param, int fd);
};

/** App 启动时调用；传 NULL 可注销（壳在 obs_shutdown 之后、退出前调）。 */
EXPORT void obs_android_set_usb_host(const struct obs_android_usb_host *host);

/** 总线是否已注册 —— 冒烟用它区分"没有设备"和"根本没接线"。 */
EXPORT bool obs_android_usb_ready(void);

/** 包装函数：总线没注册时一律返回失败值，调用方不必每次判空。 */
EXPORT int obs_android_usb_enum_devices(struct obs_android_usb_device *out, int max);
EXPORT int obs_android_usb_request_permission(const char *name);
EXPORT int obs_android_usb_open(const char *name);
EXPORT void obs_android_usb_close(int fd);

/* ------------------------------------------------------------------------- */
/* A4：Android 音频输入设备总线（与 USB 总线同构，但少两条腿）                  */
/* ------------------------------------------------------------------------- */

/* 为什么音频也要一条总线：AAudio 是纯 C 的 API，能"用哪台设备"只有一个 int deviceId，
 * 而**这个 id 只能从 Java 的 AudioDeviceInfo.getId() 拿**（AAudio 自己不枚举设备）。
 * 插件想要"下拉框里列出内置麦克风/耳机麦/USB 声卡"，就必须有人替它问 Framework。
 *
 * 与 USB 总线的两处差别，所以没有照抄回调表：
 *   1) 音频没有 fd —— AAudio 自己会 openDevice，桥只负责"给 id"；
 *   2) 音频没有按设备授权 —— RECORD_AUDIO 是一次性运行时权限，所以这里问的是
 *      "这个权限有没有"（Android 10 起没它就读不到输入设备的属性），
 *      而不是 USB 那种"对某台设备 requestPermission"。 */

#define OBS_ANDROID_AUDIO_PRODUCT_LEN 64
#define OBS_ANDROID_AUDIO_LABEL_LEN 160

/** 一台音频输入设备的快照。id 就是 AAudioStreamBuilder_setDeviceId() 要的 int。 */
struct obs_android_audio_device {
	/** AudioDeviceInfo.getId()，直接喂 AAudioStreamBuilder_setDeviceId()。
	 *  注意 0 在本工程里是"交给系统选"的哨兵值（AAudio 的 AAUDIO_UNSPECIFIED 也是 0），
	 *  而 Framework 并没有规定端口号不能是 0 —— 所以插件把 id<=0 的条目挡在下拉框外，
	 *  见 plugins/android-audio/aaudio-input.c 的 aaudio_properties。 */
	int32_t id;
	/** AudioDeviceInfo.getType()。AAudio.h 里 AAudio_DeviceType 的注释自述"values are
	 *  copied from JAVA SDK device types"，所以同一个数字两边通用。 */
	int32_t type;
	/** AudioDeviceInfo.isSource()：这台能不能采集（getDevices 已按 INPUTS 过滤，
	 *  留着是因为全双工设备会同时出现在两侧）。 */
	bool is_source;
	/** AudioDeviceInfo.getProductName()：设备自己上报的字符串，宿主侧已滤掉分隔符 */
	char product[OBS_ANDROID_AUDIO_PRODUCT_LEN];
	/** 给人看的一行（类型名 + 产品名 + id），属性页直接当条目文本用 */
	char label[OBS_ANDROID_AUDIO_LABEL_LEN];
};

/** App（壳）注册进来的音频宿主回调表。任何一项都可以是 NULL，包装函数按"失败"处理。 */
struct obs_android_audio_host {
	void *param;

	/** 填 out[0..max-1]，返回实际设备数（>=0，0 是合法值），拿不到时返回 -1。 */
	int (*get_devices)(void *param, struct obs_android_audio_device *out, int max);

	/** RECORD_AUDIO 是否已授予。没授予时 get_devices 往往仍能返回条目但属性是空的，
	 *  所以这条要单独问，插件才能把"没权限"和"没设备"说成两句话。 */
	bool (*has_capture_permission)(void *param);
};

/** App 启动时调用；传 NULL 可注销。 */
EXPORT void obs_android_set_audio_host(const struct obs_android_audio_host *host);

/** 总线是否已注册 —— 冒烟用它区分"没有输入设备"和"根本没接线"。 */
EXPORT bool obs_android_audio_ready(void);

EXPORT int obs_android_audio_enum_devices(struct obs_android_audio_device *out, int max);
/** 总线未注册时也返回 false："问不到"不等于"有权限"，调用方先判 obs_android_audio_ready()。 */
EXPORT bool obs_android_audio_has_permission(void);

/* ------------------------------------------------------------------------- */
/* P-17：内置摄像头（Camera2 NDK）的权限总线                                    */
/* ------------------------------------------------------------------------- */

/* 相机总线过两格：**权限**（CAMERA 授予态）与**此刻该不该持有设备**（前后台）。除此之外
 * 这一条仍然比另外两条窄得多 —— Camera2 NDK 自己就能枚举设备
 * （ACameraManager_getCameraIdList）、读静态特性（LENS_FACING）、开设备、取帧
 * （AImageReader + ACameraCaptureSession），全都不需要 Java；
 * 唯独这两格只长在 Java 那一侧：checkSelfPermission 要 Context，Activity 生命周期
 * 只有 Qt 的 applicationStateChanged 看得到，而插件是纯 C、只链 libobs、拿不到 JavaVM。
 *
 * 没有它也能编能跑，但报不出人话：没授予时 getCameraIdList 给空清单，
 * 与"这台机器真没相机"是同一种表现（见音频总线里同样的两句之分）。
 *
 * 线程约定与音频总线相同：可能在视频线程（源 activate）或 UI 线程（属性页）上被调，
 * 实现方自己保证线程安全，且 request_permission **不得阻塞等用户点框**。 */
struct obs_android_camera_host {
	void *param;

	/** CAMERA 运行时权限是否已授予。 */
	bool (*has_capture_permission)(void *param);

	/** 0=本来就有，1=已发起申请（结果异步，之后再问一次 has_capture_permission），
	 *  -1=失败，含"已被系统永久拒绝、再问也不会弹框"。 */
	int (*request_permission)(void *param);

	/** 此刻该不该**持有**相机设备 —— 与权限正交：权限是"能不能开"，这一格是"现在该不该占着"。
	 * 落地要它是因为 Android 的 Activity 停到后台时窗口会没（`onStopped`），而 Camera2 不会
	 * 替我们把设备交还：退后台不松手，下次进应用就是 `CAMERA_IN_USE`（开不起来，
	 * 且这条错误在别的机型上还可能表现为"预览停在最后一帧"）。
	 *
	 * **未注册/未填这一格时返回 true**（fail-open），与上面 has_capture_permission 的
	 * fail-closed 刻意相反：权限问不到时"当作没有"只是报不出人话，而这一格问不到时
	 * "当作不能采集"会直接把相机判死 —— 两者后果不对称，所以默认值也不同。 */
	bool (*capture_allowed)(void *param);
};

/** App 启动时调用；传 NULL 可注销。 */
EXPORT void obs_android_set_camera_host(const struct obs_android_camera_host *host);

/** 总线是否已注册 —— 冒烟用它区分"没有相机"和"根本没接线"。 */
EXPORT bool obs_android_camera_ready(void);

/** 总线未注册时也返回 false："问不到"不等于"有权限"，调用方先判 obs_android_camera_ready()。 */
EXPORT bool obs_android_camera_has_permission(void);
EXPORT int obs_android_camera_request_permission(void);

/** 总线未注册、或没填 capture_allowed 时返回 true —— 见结构体里那段"后果不对称"的说明。 */
EXPORT bool obs_android_camera_capture_allowed(void);

/* ------------------------------------------------------------------------- */
/* P-18-a/b/d：屏幕采集（MediaProjection）总线：问、要、还、量、接、收          */
/* ------------------------------------------------------------------------- */

/* 这一条比其余三条都窄 —— 它没有 fd、没有设备清单，但有**一块目标面**。
 *
 * 为什么窄到这个地步：屏幕采集的整个机制与 USB/音频/相机都不同 ——
 *   1) 它**不是一种可查询的状态**。运行时权限有 checkSelfPermission，投影没有：
 *      MediaProjection 是一次性的 token，只通过 Activity 的 onActivityResult 交付一次，
 *      之后系统里没有任何 API 能问"用户刚才同意过没有"。所以这一格只能由壳记账，
 *      而"记账"这件事决定了必须有真收得到回调的 Java 组件（见 ObsProjectionHost.java 头注释），
 *      不能照抄 ObsPermissionHost 那套"只弹框、结果靠现读"。
 *   2) 它**不需要 fd**。USB 总线要 open_fd 是因为 libusb 只认 fd。投影这边方向相反：
 *      过界的是"目标面"而不是"令牌" —— b 腿在 native 建 AImageReader，把它的 ANativeWindow
 *      以 void* 递过总线，由**前端**用 ANativeWindow_toSurface()（NDK 头标 __INTRODUCED_IN(26)，
 *      已核对）换成 Surface jobject 交给 Java，Java 拿令牌去 createVirtualDisplay。
 *      为什么转换必须在前端做而不是插件里：只有前端 link 了 `android`（os-android.cmake:49），
 *      插件一律 link `mediandk` 而不 link `android`（见 android-camera/CMakeLists.txt 那段
 *      同样的取舍）—— 让插件去调 ANativeWindow_* 就等于给它开一条别的插件都没有的依赖。
 *      代价是总线这一格只能传 void*，两侧靠"这是 ANativeWindow*"的口头约定对接。
 *      令牌本身不出 Java 层，所以总线不传它。
 *   3) 它**没有设备可枚举**。屏幕就一块。
 *
 * 线程约定同前三条：可能在视频线程（源 activate）或 UI 线程（属性页）上被调，实现方自己
 * 保证线程安全，request_consent **不得阻塞等用户点同意框**。
 * start_display/stop_display 同一约定：它们由源的 activate/deactivate 调，落在图形线程上，
 * 实现方不得在那儿等任何 UI 结果。 */
struct obs_android_screen_host {
	void *param;

	/** 手上有没有可用的投影令牌 —— 采集源用它决定"能开"还是"先去同意"。
	 *  未注册/未填时返回 false（fail-closed）：与相机那格 has_capture_permission 同理，
	 *  "问不到"就说"没有"，最坏结果只是报不出人话，不会让用户以为在采而实际没采。 */
	bool (*has_consent)(void *param);

	/** 0=本来就有，1=已发起（结果异步，调用方之后再来问一次 has_consent），
	 *  -1=失败（含拿不到 Activity 这类结构性失败）。 */
	int (*request_consent)(void *param);

	/** 主动把令牌交还给系统（= 我们这一侧的"停止共享"），**顺带收面**。
	 *  为什么它必须是独立的一格、不能由 stop_display 代做：面与令牌的寿命本来就不同 ——
	 *  切场景/临时停用只该场面（令牌留着，切回来立刻能采，不必再问用户一次），
	 *  而**最后一颗采集源被删掉、或应用退出**时必须连令牌一起交回去，
	 *  否则进程还活着的时候系统一直以为在被录（常驻"停止共享"通知 + 前台服务白占
	 *  mediaProjection 那一档 —— 就是 P-18-a 那条"陈旧回调擦格子"实测到的那种自相矛盾态）。
	 *  必须幂等：交还两次不该报错。 */
	void (*release_consent)(void *param);

	/** 采多大。必须回**设备物理像素**，不是 Qt 的逻辑像素（这台 wm density=240 时
	 *  dpr=1.5，逻辑 1067x667 vs 物理 1280x720 —— 拿错那一档就是画面不成比例）。
	 *  dpi 可以为 0，Java 侧会自己兜一个 160。
	 *  返回 false 表示问不到（含尺寸算出 0），调用方据此判"开不起来"而不是硬建一个 0x0 的面。 */
	bool (*display_size)(void *param, uint32_t *width, uint32_t *height, uint32_t *dpi);

	/** 把投影面接到这块 ANativeWindow（实为 AImageReader 的 acquireLatestSurface/getSuperWindow
	 *  拿到的那个指针，当不透明 void* 用）上。
	 *  返回 false = 没接上，具体原因留在 Java 侧 lastError 里，由属性页那句状态说给人听。
	 *  这一格不回填尺寸：尺寸是上面 display_size 那一格的事，插件先问、照它建 reader、再传面。 */
	bool (*start_display)(void *param, void *native_window);

	/** 收面。**必须幂等**：源的 deactivate 可能被调两次（关闭时与切场景时），
	 *  而"没在采的时候收面"不能报错也不崩。没有返回值就是因为没有可报的失败。 */
	void (*stop_display)(void *param);

	/* ---- P-18-e：系统内录（AudioPlaybackCapture）三条腿 ----
	 *
	 * 方向仍然全是"native 问、Java 答"。这一路本来最顺手的形状是让 Java 的采集线程
	 * 直接往回推 PCM（那要在前端开第一条 Java→native 的 JNI 导出），这里刻意不走它：
	 * ① 整个 Android 移植到今天为止没有一条 Java→native 的路，加第一条要连带决定
	 *    "注册在哪、类怎么找到、RegisterNatives 还是按符号名懒解析"—— Qt 把前端 .so 是
	 *    dlopen 进来的，不是 classloader 的 native lib，按名字解析这条路很可能直接
	 *    UnsatisfiedLinkError（未量，但风险形状清楚）；② PCM 的节拍本来就该由消费方定，
	 *    插件自己那条音频线程按"取空就睡 10 ms"来问，比让 Java 猜一个推送粒度更好接；
	 * ③ Java 侧因此只需要"读 AudioRecord → 攒进环形缓冲"这一件事，失败面小。
	 *
	 * 线程约定：start/stop 与视频那两格同（图形线程或源销毁路径上）；read_audio
	 * **只会在插件那条音频线程上被调**，实现方要么自己可重入要么按单线程设计。 */

	/** 建 AudioRecord 与那条读线程。成功时回填**实到**的采样率与声道数（AudioRecord 可以
	 *  拒绝我们要求的格式而给自己换一个，出参必须是它实际那一份 —— 插件拿这两个数填
	 *  obs_source_audio，报错了就是变速/变调）。手里没令牌时返回 false（这一路依附令牌）。
	 *  必须幂等：已经在录的时候再调一次应当返回 true 而不是再起一条线程。 */
	bool (*start_audio)(void *param, uint32_t *sample_rate, uint32_t *channels);

	/** 停读线程并释放 AudioRecord。**必须幂等**，且返回时那条线程必须已经不在
	 *  （插件那边紧接着就要 free 自己的缓冲）。 */
	void (*stop_audio)(void *param);

	/** 从环形缓冲取走最多 max_frames 帧（交织 int16，每声道一列），返回实际帧数。
	 *  0 = 此刻没有数据，不是错误 —— 调用方稍后再问。 */
	uint32_t (*read_audio)(void *param, int16_t *out, uint32_t max_frames);
};

/** App 启动时调用；传 NULL 可注销。 */
EXPORT void obs_android_set_screen_host(const struct obs_android_screen_host *host);

/** 总线是否已注册 —— 冒烟用它区分"用户没同意"和"根本没接线"。 */
EXPORT bool obs_android_screen_ready(void);

/** 总线未注册时也返回 false —— "问不到"不等于"有令牌"。 */
EXPORT bool obs_android_screen_has_consent(void);
EXPORT int obs_android_screen_request_consent(void);

/** 没接线时是空转 —— 那种情况下本来就没有令牌可交还，没有可报的失败。 */
EXPORT void obs_android_screen_release_consent(void);

/* 下面三格全 fail-closed（stop 那格没有成功与否可言）：未注册/未填时
 * display_size/start_display 返回 false，源据此走"开不起来"那条分支，
 * 绝不拿一个没问到的尺寸去建 reader。 */
EXPORT bool obs_android_screen_display_size(uint32_t *width, uint32_t *height, uint32_t *dpi);
EXPORT bool obs_android_screen_start_display(void *native_window);
EXPORT void obs_android_screen_stop_display(void);

/* 这三格同样 fail-closed：没接线时 start 返回 false（源据此就不起音频线程）、stop 空转、
 * read 返回 0 帧。"问不到"在这一路一律不等于"在录"。 */
EXPORT bool obs_android_screen_start_audio(uint32_t *sample_rate, uint32_t *channels);
EXPORT void obs_android_screen_stop_audio(void);
EXPORT uint32_t obs_android_screen_read_audio(int16_t *out, uint32_t max_frames);

#ifdef __cplusplus
}
#endif
