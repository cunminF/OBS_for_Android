#include "OBSAndroidPermissions.hpp"

#ifdef __ANDROID__

#include <QJniEnvironment>
#include <QJniObject>

/* P-18-b 的"接面"那一格要 ANativeWindow_toSurface：它属于 libandroid，而**只有前端 link 了它**
 * （build.ninja 里 frontend/libobs_*.so 那行有 -landroid -lmediandk，插件按 android-camera 的取舍
 * 一律不 link android）—— 所以这道转换只能落在本文件，插件把 ANativeWindow* 当不透明 void* 递过来。
 * minSdk 29 ≥ 26，这个符号不需要运行时判档。 */
#include <android/native_window.h>
#include <android/native_window_jni.h>

/* P-17-f 那第三条腿要的东西：<QGuiApplication> 给 applicationState / applicationStateChanged
 * （Qt 6 把它们从 QCoreApplication 搬到了 QGuiApplication），<QObject> 给 QObject::connect
 * （两处坑写在下面那次 connect 的注释里），<atomic> 给那个跨线程布尔。 */
#include <QGuiApplication>
#include <QObject>

#include <atomic>
#include <string.h>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsPermissionHost.java） */
static const char kPermissionClass[] = "com/obsproject/studio/ObsPermissionHost";

/* requestPermissions 只能在 Activity 上调（Context 没有这个方法），所以这里要的是 Activity 而不是
 * 通用 Context —— 与 ObsForegroundService 那边只要 Context 不同。 */
static QJniObject hostActivity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

static QString callDescribe(const QJniObject &ctx)
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kPermissionClass, "describe",
								"(Landroid/content/Context;)Ljava/lang/String;",
								ctx.object<jobject>());
	return s.isValid() ? s.toString() : QStringLiteral("describe 调不到（类没进 APK？）");
}

bool obsAndroidPermissionsRequestStartup(QString *detail)
{
	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		const QString line = QStringLiteral("权限申请没起来：拿不到 QtNative.activity()");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
		return false;
	}

	const jboolean posted = QJniObject::callStaticMethod<jboolean>(kPermissionClass, "requestStartup",
								       "(Landroid/app/Activity;)Z",
								       act.object<jobject>()) != 0;
	/* describe 里的"待申请/动作"才是这次到底弹了框还是本来就全有的判据；posted 只表示没拿到 Activity。 */
	const QString line = QStringLiteral("requestStartup=%1 %2")
				     .arg(posted ? QStringLiteral("true") : QStringLiteral("false"))
				     .arg(callDescribe(act));
	if (detail)
		*detail = line;
	blog(posted ? LOG_INFO : LOG_WARNING, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
	return posted;
}

QString obsAndroidPermissionsStatus()
{
	return callDescribe(hostActivity());
}

/* ---------------- P-17：相机权限总线（libobs ↔ ObsPermissionHost） ---------------- */

/* 为什么这一格必须由前端来填：插件 .so 只链 libobs、没有 JavaVM，而 CAMERA 授予态只有 Java 的
 * Context 问得到（Camera2 NDK 里没有 checkSelfPermission）。总线本体在 libobs/obs-android.h，
 * 前端在这里把两条腿接上。
 *
 * 两条腿都把 Activity 当 Context 传：checkSelfPermission 是 Context 的方法，用 Activity 调合法；
 * 而 requestCamera 里的 requestPermissions/runOnUiThread 只能在 Activity 上有，所以不能退到
 * appContext()。拿不到 Activity 时两处都按失败走（Java 侧 ctx/act 判空 → false / -1），
 * 不会抛 JNI 异常 —— 这也是这张表可以是文件级 const、退出时不忙着注销的原因。 */
static bool cameraHasPermission(void *param)
{
	(void)param;
	const QJniObject act = hostActivity();
	return QJniObject::callStaticMethod<jboolean>(kPermissionClass, "hasCameraPermission",
						      "(Landroid/content/Context;)Z",
						      act.object<jobject>()) != 0;
}

static int cameraRequestPermission(void *param)
{
	(void)param;
	const QJniObject act = hostActivity();
	return (int)QJniObject::callStaticMethod<jint>(kPermissionClass, "requestCamera",
						       "(Landroid/app/Activity;)I",
						       act.object<jobject>());
}

/* ---------------- P-17-f：前后台 → "此刻该不该占着相机" ---------------- */

/* 为什么这一格也要前端来填（跟权限那两条不是一个理由）：Camera2 的 Activity 生命周期只有
 * Java/Qt 那侧看得见 —— 插件是纯 C、没有 Activity 回调，而 Android 的政策是"你的窗口停了
 * 就该把相机放下"，不放的代价是下次进应用直接 ACAMERA_ERROR_CAMERA_IN_USE。
 *
 * 为什么**缓存**而不是一问就去读 applicationState()：这条腿是在**视频线程**上被 camera2_video_tick
 * 读的，而 QGuiApplication::applicationState()（Qt 6.9.3，android_x86_64/include/QtGui/
 * qguiapplication.h:126，信号在同文件 :153）在 Qt 里属于平台插件的 GUI 线程状态。
 * 所以只在 UI 线程收到 applicationStateChanged 时写一次这个原子量，别的线程只读。 */
static std::atomic<bool> g_captureAllowed{true};

static bool cameraCaptureAllowed(void *param)
{
	(void)param;
	return g_captureAllowed.load();
}

/* 交还的判据是"面停了"= Hidden/Suspended（Android 的 onStop），**不含 Inactive**（onPause 那一格）。
 *
 * 这条是量出来的，不是我推的：b107 原本写的是"只有 Active 才算该持有"，理由是"两边后果不对称，
 * 取保守那一头" —— 这个理由漏了一步：**保守只有在"暂时的状态之后一定还会再来一次真信号"时才成立**。
 * 实测它不成立：19:38:06 一个 monkey 发的 LAUNCH_SINGLE_TOP 重启意图让 Qt 连着发了
 * state=4 → state=2，然后就**停**在 2；同一时刻 dumpsys 说 topResumedActivity=QtActivity、
 * state=RESUMED、mCurrentFocus 也还是我们，进程 16293 活着，日志之后 109 秒再没一行。
 * 后果从"保守"变成了"永久没画面"：相机被交还，而 g_capture_allowed 再没有翻回 true，
 * 插件 f 腿第 4 支那个重开条件（allowed）永远不满足 —— 用户看到的是一个不报错的黑源。
 *
 * 换成现在的判据，同一趟生命周期各状态的落点（MuMu 实测）：HOME → 2 然后 0（交还照旧发生，
 * 立案时那条硬约束②"退后台不交还设备就是 CAMERA_IN_USE"仍然满足）；对话框/下拉栏/重启意图抢
 * 焦点 → 停在 2（画面不断）；回前台 → 4（重开，实测 11 ms）。
 *
 * 单独提成一个函数、不写两遍：下面冷启动起头那一处也用它，判据只有一份。 */
static bool stateAllowsCapture(Qt::ApplicationState state)
{
	return (state == Qt::ApplicationActive || state == Qt::ApplicationInactive);
}

static void onAndroidApplicationChanged(Qt::ApplicationState state)
{
	const bool allowed = stateAllowsCapture(state);
	const bool was = g_captureAllowed.exchange(allowed);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 应用态变了 state=%d（Qt6 qnamespace.h:271-276："
		       "0=Suspended 1=Hidden 2=Inactive 4=Active），可采集 %d→%d%s",
	     (int) state, (int) was, (int) allowed, was == allowed ? "（这一格没变）" : "");
}

static const struct obs_android_camera_host kCameraHost = {
	nullptr,
	cameraHasPermission,
	cameraRequestPermission,
	/* 第三条腿：位置对应 struct 里的 capture_allowed（照字段顺序写，与上面两条同一个形状）。
	 * 少了这一格就是 NULL，而 obs_android_camera_capture_allowed() 对 NULL 是 fail-open
	 * （返回 true）—— 忘了接的后果不是编不过，是"退后台不交还设备"，
	 * 而那要到真机上才看得见。 */
	cameraCaptureAllowed,
};

void obsAndroidPermissionsRegisterCameraBus()
{
	/* 头里那句"注册是幂等的"靠这个 bool 兜住：总线本身是覆盖写，重复注册没事，
	 * 但下面那次 connect 会**累加** —— 连接归 sender（App，活到进程结束）所有，
	 * 没有 context 对象替我们解绑，所以只允许接一次。 */
	static bool s_bus_registered = false;
	if (s_bus_registered)
		return;
	s_bus_registered = true;

	/* 先按"现在这一刻"的态起头，再挂信号：注册发生在建窗口之后（OBSBasic 构造里），
	 * 那时可能还没 Active —— b107 用旧判据（只认 Active）就在这儿起过冲突：冷启动那一拍
	 * 量到"可采集初值=0"，源 activate 后刚开起来 139 ms 就被自己拆掉（19:36:34.352 开、
	 * .492 拆），等 .525 的 state=4 再重开。用同一个判据之后 Inactive 也算可持有，
	 * 这一趟开→拆→开没了；万一将来又在建窗口之前注册，这一句也让第一拍就是对的。 */
	g_captureAllowed.store(stateAllowsCapture(QGuiApplication::applicationState()));
	/* 两个坑，都是 b107 编出来的、不是推定：
	 *  ① Qt 6 没有全局 connect()（不加限定名只在 QObject 派生类的成员函数里捡得到，
	 *     本文件这个是自由函数）⇒ 写 QObject::connect，与 dialogs/OBSAbout.cpp:71 同形。
	 *  ② 三连参那个"无 context"重载在这一版里根本不存在：前端这个 TU 的 DEFINES 里有
	 *     QT_ENABLE_STRICT_MODE_UP_TO=0xFF0000（build-fe-x86_64-plugins/build.ninja），
	 *     而 strict mode ≥ 6.7.0 就把 QT_NO_CONTEXTLESS_CONNECT 定义出来
	 *     （QtCore/qtconfigmacros.h:211-215），于是 qobject.h:275-283 那整个重载被 #ifndef 掉
	 *     —— 报的是 "no matching function for call to 'connect'"。⇒ 必须显式给 context。
	 * context 给 qGuiApp 本身：App 活到进程结束，这条连接就跟着它活到进程结束，
	 * 与上面那个 bool 记的"只注册一次"是同一笔账。形状抄 frontend/OBSApp.cpp:1681-1682。
	 * 用 qGuiApp 而不是 qApp：qApp 每装一个模块重定义一次（QtCore/QtGui/QtWidgets 各一份），
	 * 静态类型取决于本次编译最后展开了哪个头；qGuiApp 只在 qguiapplication.h:34 定义，就是 QGuiApplication*。 */
	QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, qGuiApp,
			 &onAndroidApplicationChanged);

	obs_android_set_camera_host(&kCameraHost);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 相机总线三条腿已注册（可采集初值=%d）",
	     (int) g_captureAllowed.load());
}

/* ---------------- P-18-a：屏幕采集同意总线（libobs ↔ ObsProjectionHost） ---------------- */

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsProjectionHost.java）。
 * 与权限那个类分开的理由写在那文件的头注释里：MediaProjection 没有可查状态，必须真收得到
 * onActivityResult，而 ObsPermissionHost 整套设计的前提恰恰是"不接回调"。 */
static const char kProjectionClass[] = "com/obsproject/studio/ObsProjectionHost";

/* 这里要的是通用 Context 而不是 Activity：起代理 Activity 用的是 startActivity（Context 就有），
 * 不是 requestPermissions（那个才只能在 Activity 上调）。拿不到时传个无效 jobject 下去，
 * Java 侧 ctx 判空 → -1，与相机那两条腿同一个失败形状。
 *
 * 口名是查出来的不是推的：`javap -s` 扫 Qt6Android.jar 里的 org.qtproject.qt.android.QtNative，
 * 静态取上下文的只有两个 —— `activity()` ()Landroid/app/Activity; 与 `getContext()`
 * ()Landroid/content/Context;。**没有 application()**（我第一版按别的框架的习惯写了它，
 * 那样运行时会 NoSuchMethodError → jobject 为空 → 每次 request_consent 都返回 -1，
 * 而这在属性页上看起来与"用户没同意"是同一种表现）。
 * 这两个口都是包内可见，JNI 那侧照样调得到：上面 hostActivity() 调 activity() 已经是 P-17
 * 实测跑通过的同一条路。 */
static QJniObject hostContext()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "getContext",
						  "()Landroid/content/Context;");
}

/* Java 侧那本账（意图位/令牌/面/最后一次动作与错误）只有它自己能说清，native 侧每一处失败都顺手
 * 带上它 —— 屏幕这条路失败原因全在 Java 里，不带这句就只能靠 logcat 拼。 */
static QString screenDescribe()
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kProjectionClass, "describe",
								 "()Ljava/lang/String;");
	return s.isValid() ? s.toString() : QStringLiteral("describe 调不到（类没进 APK？）");
}

static bool screenHasConsent(void *param)
{
	(void)param;
	return QJniObject::callStaticMethod<jboolean>(kProjectionClass, "hasProjection", "()Z") != 0;
}

static int screenRequestConsent(void *param)
{
	(void)param;
	const QJniObject ctx = hostContext();
	return (int)QJniObject::callStaticMethod<jint>(kProjectionClass, "requestConsent",
						       "(Landroid/content/Context;)I",
						       ctx.object<jobject>());
}

/* ---------------- P-18-b：把令牌接成一块投影面（量的、接的、收的三条腿） ---------------- */

/* 采集尺寸这一格只做一件事：把 Java 那边 Display.getMode() 的量取结果搬到 native 的 uint32_t 里。
 * 为什么非要绕这一圈而不在前端问 QScreen：QScreen 给的是**逻辑**像素（这台 wm density 覆盖成 240
 * 时 dpr=1.5 ⇒ 1067x667），而 VirtualDisplay 要的是设备物理像素（1280x720），拿错那一档不报错、
 * 只是画面与屏幕不成比例 —— 这类错只能从源头掐掉：源头就是 Java。
 * hostContext() 拿不到时返回 false（与 request_consent 那格同一个失败形状：问不到就说问不到）。
 *
 * 一次往返拿四个数（d 腿起）：源每秒要问一次尺寸，好发现旋转/分辨率变了。原先那三格
 * captureWidth/Height/Dpi 每次问要跑三遍完整量取，日志也跟着三遍。 */
static bool screenDisplaySize(void *param, uint32_t *width, uint32_t *height, uint32_t *dpi)
{
	(void)param;
	const QJniObject ctx = hostContext();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸问不到：拿不到 QtNative.getContext()");
		return false;
	}

	const QJniObject arr = QJniObject::callStaticObjectMethod(kProjectionClass, "captureSize",
							   "(Landroid/content/Context;)[I", ctx.object<jobject>());
	if (!arr.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸问不到：captureSize 返回空数组，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
		return false;
	}

	QJniEnvironment env;
	const jintArray ja = arr.object<jintArray>();
	const jsize n = env->GetArrayLength(ja);
	jint *els = (n >= 4) ? (jint *)env->GetIntArrayElements(ja, nullptr) : nullptr;
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸读不到（数组长度=%d，Java 侧=%s）", (int)n,
		     screenDescribe().toUtf8().constData());
		return false;
	}
	const jint w = els[0];
	const jint h = els[1];
	const jint d = els[2];
	env->ReleaseIntArrayElements(ja, els, JNI_ABORT); // 只读，JNI_ABORT = 不把改动写回去

	if (w <= 0 || h <= 0) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸不成立 %dx%d（Java 侧=%s）", (int)w, (int)h,
		     screenDescribe().toUtf8().constData());
		return false;
	}

	*width = (uint32_t)w;
	*height = (uint32_t)h;
	if (dpi)
		*dpi = (d > 0) ? (uint32_t)d : 0;
	return true;
}

static bool screenStartDisplay(void *param, void *native_window)
{
	(void)param;
	if (!native_window) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 接面失败：native 传上来的窗口指针是空的");
		return false;
	}
	const QJniObject ctx = hostContext();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 接面失败：拿不到 QtNative.getContext()");
		return false;
	}

	/* QJniEnvironment 在 Qt 6.9 里**不是模板**、也没有 object()：它是普通类，取 JNIEnv 用
	 * jniEnv() 或 operator->（qjnienvironment.h:17-25）。我按 QtAndroidExtras 时代的旧形状
	 * 写了 `QJniEnvironment<>` 加 `.object()`，编译当场被否 —— 又是"凭记忆写 API 形状"那一笔
	 * （仪器账第 29 条的老根），这回是开着头文件改的。 */
	QJniEnvironment env;
	ANativeWindow *window = static_cast<ANativeWindow *>(native_window);
	jobject surface = ANativeWindow_toSurface(env.jniEnv(), window);
	if (!surface) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: ANativeWindow_toSurface 返回空（窗口=%p）",
		     native_window);
		return false;
	}

	/* 尺寸**取自这块窗口本身**，不再绕回 display_size：AImageReader 是照 display_size 建的，
	 * 窗口几何就是它的尺寸 —— 以传进来的这个对象为唯一真相，两侧就不可能对不上（对不上也只能
	 * 在这一句里暴露出来，而那正是我们想看的）。 */
	const jint w = ANativeWindow_getWidth(window);
	const jint h = ANativeWindow_getHeight(window);
	uint32_t dw = 0, dh = 0, ddpi = 0;
	const jint dpi = screenDisplaySize(nullptr, &dw, &dh, &ddpi) ? (jint)ddpi : 0;

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(
		kProjectionClass, "startCapture", "(Landroid/content/Context;Landroid/view/Surface;III)Z",
		ctx.object<jobject>(), surface, w, h, dpi);

	/* NDK 头只说了"这个 Surface 会自己持有 ANativeWindow 的引用"，**没说这个 jobject 本身是
	 * local 还是 global**（原文见 native_window_jni.h:47-56）。这一格决定要不要还、以及怎么还，
	 * 猜错两半都有代价：把 global 当 local 删是 ART 直接 abort，该删不删是每次 activate 漏一个
	 * 全局引用。所以不猜 —— 拿 GetObjectRefType 现问，并把答案打进日志（1=local 2=global
	 * 3=weak global）：这一轮先看真机给什么，之后再按它收拾。 */
	const jint ref_type = env->GetObjectRefType(surface);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 接面 %dx%d@%ddpi 结果=%d Surface 引用类型=%d Java 侧=%s",
	     (int)w, (int)h, (int)dpi, (int)ok, (int)ref_type, screenDescribe().toUtf8().constData());

	if (ref_type == JNILocalRefType)
		env->DeleteLocalRef(surface);
	return ok != 0;
}

static void screenStopDisplay(void *param)
{
	(void)param;
	const QJniObject why = QJniObject::fromString(QStringLiteral("native 侧收面（源 deactivate）"));
	QJniObject::callStaticMethod<void>(kProjectionClass, "stopCapture", "(Ljava/lang/String;)V",
					   why.object<jstring>());
}

/* ---------------- P-18-d：把令牌也交回去（第六格） ---------------- */

/* 与上面那格的分别写进 obs-android.h 的结构体注释了，这里只记落地：Java 侧 stopProjection 自己
 * 就按"先收面、再 stop 令牌、再落意图位、再重算前台类型"那一步不落顺序做完，所以这一格不需要
 * 先调 stop_display —— 调两次只是把同一本账翻两遍。它返回 false 表示"手里本来就没有令牌"，
 * 那不是失败，所以这里不 blog 警告，只在真交出去的那一发记一行。 */
static void screenReleaseConsent(void *param)
{
	(void)param;
	const QJniObject why = QJniObject::fromString(QStringLiteral("native 侧交还（最后一颗采集源销毁）"));
	const jboolean did = QJniObject::callStaticMethod<jboolean>(kProjectionClass, "stopProjection",
							    "(Ljava/lang/String;)Z", why.object<jstring>());
	if (did)
		blog(LOG_INFO, "OBSAndroidPermissions[Android]: 投影令牌已交还，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
}

/* ---------------- P-18-e：系统内录三条腿 ---------------- */

/* drainAudio(short[] out, int maxFrames) 一问就是 10 ms 一发（一秒一百次），所以这里的 short[]
 * 养在文件级复用：每次新建就是每秒一百个临时对象（Java 侧那句注释给的是同一个理由）。
 * 存的是 **global ref** —— local ref 只活到本次 JNI 调用结束，而这一格要在插件那条音频线程上
 * 反复用；global ref 不随线程走，所以 start/stop 落在哪条线程都能建和销。
 *
 * 三个静态量各自只被一条路碰：read 在插件的采集线程，start/stop 在视频线程或源销毁路径上。
 * 插件那边是"先把线程 join 掉、再调 stop_audio"（那两行写在 screen-capture.c 的停止那一格），
 * 所以这里不再额外上锁。 */
static jshortArray g_audioScratch = nullptr;
static jsize g_audioScratchShorts = 0;
static uint32_t g_audioScratchChannels = 0;

static void audioReleaseScratch()
{
	if (!g_audioScratch)
		return;
	QJniEnvironment env;
	env->DeleteGlobalRef(g_audioScratch);
	g_audioScratch = nullptr;
	g_audioScratchShorts = 0;
	g_audioScratchChannels = 0;
}

/* Java 侧 startAudio() 把 `{实到采样率, 实到声道数}` 当返回值交出来（不写出参），为的是与
 * captureSize 一样**一趟 JNI 拿全** —— 与屏幕尺寸那格同一个形状，照抄那边的读法。
 * 出参必须是 Java 实到的那一组：AudioRecord 允许把我们要求的格式换成它自己的，报错了就是变速变调。 */
static bool screenStartAudio(void *param, uint32_t *sample_rate, uint32_t *channels)
{
	(void)param;
	const QJniObject arr = QJniObject::callStaticObjectMethod(kProjectionClass, "startAudio", "()[I");
	if (!arr.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录起不来（startAudio 返回 null），Java 侧=%s",
		     screenDescribe().toUtf8().constData());
		return false;
	}

	QJniEnvironment env;
	const jintArray ja = arr.object<jintArray>();
	const jsize n = env->GetArrayLength(ja);
	jint *els = (n >= 2) ? env->GetIntArrayElements(ja, nullptr) : nullptr;
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录格式读不到（数组长度=%d），Java 侧=%s", (int)n,
		     screenDescribe().toUtf8().constData());
		return false;
	}
	const jint rate = els[0];
	const jint ch = els[1];
	env->ReleaseIntArrayElements(ja, els, JNI_ABORT); // 只读，不把改动写回去

	if (rate <= 0 || ch <= 0) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录格式不成立 %dHz×%d，Java 侧=%s", (int)rate,
		     (int)ch, screenDescribe().toUtf8().constData());
		return false;
	}

	/* 缓冲按"一秒"建：48k 立体声 = 96000 个 short = 192 KB，一次性的代价，换来读那一格零分配。
	 * 幂等的第二发（或 Java 侧换了格式）走到这里：容量够就沿用，不够就销旧建新 —— 不销就是每换一次
	 * 漏一枚 global ref。 */
	const jsize need = (jsize)(rate * ch);
	if (g_audioScratch && (g_audioScratchChannels != (uint32_t)ch || g_audioScratchShorts < need))
		audioReleaseScratch();
	if (!g_audioScratch) {
		jshortArray fresh = env->NewShortArray(need);
		if (!fresh) {
			/* 缓冲给不出来就别把 AudioRecord 留在场上：那一头已经起了读线程，
			 * 让它跟着这一格一起回去，两侧才还是一致的。 */
			QJniObject::callStaticMethod<void>(kProjectionClass, "stopAudio", "()V");
			blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录缓冲建不出来（%d 个 short）", (int)need);
			return false;
		}
		g_audioScratch = (jshortArray)env->NewGlobalRef(fresh);
		env->DeleteLocalRef(fresh);
		g_audioScratchShorts = need;
		g_audioScratchChannels = (uint32_t)ch;
	}

	if (sample_rate)
		*sample_rate = (uint32_t)rate;
	if (channels)
		*channels = (uint32_t)ch;
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 内录总线已接上 %dHz×%d（复用缓冲 %d 个 short ≈ 1 秒）",
	     (int)rate, (int)ch, (int)g_audioScratchShorts);
	return true;
}

/* 与 release_consent 那格同样的"幂等 + 只在真做了事的那一发记日志"：Java 侧 stopAudio 手里没录音
 * 时是空转，所以这一格也不返回成败（结构体里就是这么定的）。 */
static void screenStopAudio(void *param)
{
	(void)param;
	const bool had = (g_audioScratch != nullptr);
	QJniObject::callStaticMethod<void>(kProjectionClass, "stopAudio", "()V");
	audioReleaseScratch();
	if (had)
		blog(LOG_INFO, "OBSAndroidPermissions[Android]: 内录总线已断开，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
}

/* 取一次 PCM。0 帧 = 此刻没有，不是错误（调用方睡 10 ms 再来）。
 *
 * **顺序要紧**：先 drain 再 GetShortArrayElements。这一格规范上允许给回一份**拷贝**（ART 通常给
 * 直接指针，但不保证），反过来写的症状是"声音永远慢一发"—— 比崩还难查，所以写在注释里。
 * 拿完就 Release(JNI_ABORT)：只读，不回写。 */
static uint32_t screenReadAudio(void *param, int16_t *out, uint32_t max_frames)
{
	(void)param;
	if (!out || max_frames == 0 || !g_audioScratch || g_audioScratchChannels == 0)
		return 0;

	/* max_frames 是**帧**，缓冲的容量是**short**，夹之前先换算（Java 侧 drainAudio 自己也会按
	 * out.length/声道数再夹一道，这里这一道是为了让 jint 那个窄化有界、且把"最多能拿回多少"说清楚）。 */
	const uint32_t cap_frames = (uint32_t)(g_audioScratchShorts / (jsize)g_audioScratchChannels);
	const jint want = (jint)((max_frames < cap_frames) ? max_frames : cap_frames);
	const jint got = QJniObject::callStaticMethod<jint>(kProjectionClass, "drainAudio", "([SI)I",
							    (jobject)g_audioScratch, want);
	if (got <= 0)
		return 0;

	/* 这一条线程是插件自己 pthread_create 出来的原生线程，不在 JVM 的线程表里。
	 * 判 env 有效再解引用：QJniEnvironment 拿不到 JNIEnv 时 jniEnv() 给空指针，
	 * 而 operator-> 就是它 —— 少这一格判空是段错误，不是"读不到"。
	 * （"Qt 会不会替外来线程 attach"这件事不靠读源码断定：屏幕尺寸那一格本来就是 libobs
	 * 的视频线程在每秒问一次、P-18-b/d 两轮的日志里都是真数，外来线程走得通是量出来的。） */
	QJniEnvironment env;
	if (!env.isValid()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录取不到帧：这条线程拿不到 JNIEnv");
		}
		return 0;
	}

	const jsize shorts = (jsize)got * (jsize)g_audioScratchChannels;
	jshort *els = env->GetShortArrayElements(g_audioScratch, nullptr); // 不写 const：Release 那一格入参是 jshort*
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录取到的 short 数组读不到（%d 帧）", (int)got);
		return 0;
	}
	memcpy(out, els, (size_t)shorts * sizeof(int16_t));
	env->ReleaseShortArrayElements(g_audioScratch, els, JNI_ABORT);
	return (uint32_t)got;
}

static const struct obs_android_screen_host kScreenHost = {
	nullptr,
	screenHasConsent,
	screenRequestConsent,
	screenReleaseConsent,
	screenDisplaySize,
	screenStartDisplay,
	screenStopDisplay,
	screenStartAudio,
	screenStopAudio,
	screenReadAudio,
};

void obsAndroidPermissionsRegisterScreenBus()
{
	static bool s_bus_registered = false;
	if (s_bus_registered)
		return;
	s_bus_registered = true;

	obs_android_set_screen_host(&kScreenHost);

	/* 这一行是 P-18-a/b/d/e 的冒烟抓手。几格在这里**现调**（ready / has_consent / display_size /
	 * start_audio / stop_audio）：本轮还没有任何采集源去问它们，所以"native → JNI → Java 走得通"
	 * 这件事只有靠这一行才有一手证据（打出 ready=1 就说明总线指针对了、JNI 方法名签名对了、类真在 APK 里）。
	 * 注：has_consent 在这里必然读到 =0 —— 令牌不跨进程活，冷启动那一刻手上没有；
	 * true 那一支要等采集源真开起来才能覆。
	 * display_size 不一样：它不要令牌、只要 Context，所以在注册这一刻就能量到真数 ——
	 * 这也是它对不对的第一手证据（预期 1280x720，与 dumpsys display 的 mode 同档）。
	 * start_audio 与 has_consent 同理是**反向**证据：这一刻没有令牌，所以它必须返回 false，
	 * 而 Java 侧会把 lastError 写成"startAudio：没有令牌…"—— 这一句跟着 describe 打出来，
	 * 就同时证明了"调用真到了 Java、方法名与签名都对、判据没错"。万一它返回 true（不该有），
	 * 后面那发 stop_audio 会当场把它收回去。
	 * read_audio 不在这里现调：没有缓冲时它压根不过 JNI，探了也不成为证据。
	 * 另外两格现调不了：request_consent 会在注册那一拍就给用户弹框；start_display 要一块真
	 * AImageReader 的窗口，编不出假的；release_consent 此刻手里没令牌，调了也只是空转。 */
	uint32_t dw = 0, dh = 0, ddpi = 0;
	const bool sized = obs_android_screen_display_size(&dw, &dh, &ddpi);
	uint32_t arate = 0, ach = 0;
	const bool audio_up = obs_android_screen_start_audio(&arate, &ach); // 预期 false（没令牌）
	obs_android_screen_stop_audio();                                    // 上面真起来了的话就地收回；否则空转
	const QJniObject s = QJniObject::callStaticObjectMethod(kProjectionClass, "describe",
								"()Ljava/lang/String;");
	const QString line = QStringLiteral(
				     "屏幕总线九条腿已注册（ready=%1 现读有令牌=%2 尺寸=%3 %4x%5@%6 "
				     "无令牌时内录起=%7 %8Hz×%9）Java 侧=%10")
				     .arg(obs_android_screen_ready() ? 1 : 0)
				     .arg(obs_android_screen_has_consent() ? 1 : 0)
				     .arg(sized ? QStringLiteral("成") : QStringLiteral("败"))
				     .arg(dw)
				     .arg(dh)
				     .arg(ddpi)
				     .arg(audio_up ? QStringLiteral("成（不该）") : QStringLiteral("败（应当）"))
				     .arg(arate)
				     .arg(ach)
				     .arg(s.isValid() ? s.toString()
						      : QStringLiteral("describe 调不到（类没进 APK？）"));
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
}

#endif // __ANDROID__
