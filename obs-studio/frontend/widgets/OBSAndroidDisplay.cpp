#include "OBSAndroidDisplay.hpp"

#ifdef __ANDROID__

#include <QJniEnvironment>
#include <QJniObject>
#include <QPointer>
#include <QApplication>
#include <QMouseEvent>
#include <QWidget>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <jni.h>
#include <mutex>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsDisplayHost.java）
 * 全是 public static 方法，所以只用 QJniObject::callStaticObjectMethod / callStaticMethod。 */
static const char kDisplayHostClass[] = "com/obsproject/studio/ObsDisplayHost";

/* 必须是 Activity，不能退化成 Context：addContentView 只在 Activity 上（S1 实测，
 * .qoder/s1-display-api.log）。这一点和 USB/音频宿主不同 —— 那两个只要 Context。 */
static QJniObject hostActivity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

static QString jstringToQString(const QJniObject &s)
{
	if (!s.isValid())
		return QString();
	return s.toString();
}

static int callInt(const char *method)
{
	return QJniObject::callStaticMethod<jint>(kDisplayHostClass, method, "()I");
}

static QString callString(const char *method)
{
	return jstringToQString(QJniObject::callStaticObjectMethod(kDisplayHostClass, method, "()Ljava/lang/String;"));
}

/* 和 ObsDisplayHost.java 里的 TAG 同一个字符串：一条 `logcat -s OBS-display` 就能把
 * Java 的 surface 回调和 native 的决策按时间拼成一条时序。 */
static const char kDisplayTag[] = "OBS-display";

void obsAndroidDisplayLog(const char *fmt, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	__android_log_print(ANDROID_LOG_INFO, kDisplayTag, "%s", buf);
	blog(LOG_INFO, "%s: %s", kDisplayTag, buf);
}

int obsAndroidDisplaySerial()
{
	return (int)QJniObject::callStaticMethod<jint>(kDisplayHostClass, "surfaceSerial", "()I");
}

bool obsAndroidDisplaySetVisible(bool visible)
{
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "setVisible", "(Z)Z",
								   visible ? JNI_TRUE : JNI_FALSE);
	/* Java 只在状态真的变了才动 view（幂等），所以这里每打一条就是实打实的一轮藏/露；
	 * false 表示"没有视图可动"（还没 attach 或已 detach），不是错误 —— 例如主窗口还没
	 * 建好预览时就有模态框弹出。 */
	obsAndroidDisplayLog("3-3 setVisible(%s) → Java %s", visible ? "true" : "false",
			     ok ? "已受理" : "拒绝：没有活动视图");
	return ok != 0;
}

/* ===================== B-4/B-8 生命周期：Java surface 回调 → native 回读状态再决策 =====================
 *
 * Android 暂停/恢复只销毁+重建 SurfaceView 底下的 Surface，不重跑 attach，也不触发 Qt 的
 * QWindow::visibleChanged（实测 releaseDisplay 从没因暂停而跑），所以 obs_display 会一直绑在
 * 一个已失效的 ANativeWindow 上 → 恢复后预览坏死（plan.md §五 B-4 实测）。
 *
 * B-8 把这一步从"两种回调各管一头"改成"一种事件 + 回读状态"：实测里 surface 可以在没被任何人
 * 要求的情况下消失（启动期几何抖动），旧写法一旦 lost/gained 漏跑或顺序反了，就再也对不上。
 * 现在 Java 三个回调都投递同一个事件，OBSQTDisplay 收到后回读 obsAndroidDisplaySerial()
 * 的现值决定销毁还是重建 —— 事件本身只当"该重查了"的门铃。
 *
 * 这些 native 回调在 **Android UI 线程**被调，绝不能同步等 Qt 主线程（attach() 正阻塞 Qt 线程等
 * surfaceChanged 闩，同步等必死锁）—— 只做一次 Qt::QueuedConnection 投递立即返回；真正的
 * obs_display_destroy / 重建在 OBSQTDisplay 注册的回调里、于 Qt 主线程跑。 */

static std::mutex g_lifecycleMutex;
static QPointer<QObject> g_lifecycleCtx; // 跑在 Qt 主线程的对象（OBSQTDisplay）
static std::function<void(int)> g_onSurfaceEvent;
/* B-7 甲：被抢占之后挂起的那一位。只留一层 —— 要的是"主预览 ↔ 投影仪"来回倒手，
 * 第二个投影仪来抢时宁可让它没面，也不要把手上这位忘掉（忘了就再也回不去）。 */
static QPointer<QObject> g_parkedCtx;
static std::function<void(int)> g_onParkedEvent;

static void postSurfaceEventTo(QObject *ctx, const std::function<void(int)> &fn, int what)
{
	if (!ctx || !fn)
		return;
	/* 投递到 ctx 所属线程（Qt 主线程）；ctx 若在事件跑之前析构，Qt 会丢弃挂它的排队调用。 */
	QMetaObject::invokeMethod(ctx, [fn, what] { fn(what); }, Qt::QueuedConnection);
}

static void postSurfaceEvent(int what)
{
	QObject *ctx = nullptr;
	std::function<void(int)> fn;
	{
		std::lock_guard<std::mutex> lk(g_lifecycleMutex);
		if (!g_lifecycleCtx)
			return;
		ctx = g_lifecycleCtx.data();
		fn = g_onSurfaceEvent;
	}
	postSurfaceEventTo(ctx, fn, what);
}

static void jniSurfaceEvent(JNIEnv *, jclass, jint what)
{
	postSurfaceEvent((int) what);
}

/* B-5：把 Java SurfaceView 收到的触摸转成 Qt 鼠标事件投给预览控件（OBSBasicPreview，也就是
 * g_lifecycleCtx 那个 widget）。native 在 Android UI 线程被调 → 投递到 Qt 主线程再 sendEvent：
 * 一来要读 widget 的 devicePixelRatioF() 把物理像素换成逻辑坐标，二来 Qt 事件必须在它自己的
 * 线程派发。SurfaceView zOrderOnTop 盖住预览区、把真触摸吞了，不转发的话 Qt 侧永远收不到。 */
static void jniTouch(JNIEnv *, jclass, jint action, jfloat xPx, jfloat yPx)
{
	QPointer<QObject> ctx;
	{
		std::lock_guard<std::mutex> lk(g_lifecycleMutex);
		ctx = g_lifecycleCtx;
	}
	if (!ctx)
		return;

	QMetaObject::invokeMethod(
		ctx.data(),
		[ctx, action, xPx, yPx] {
			QWidget *w = qobject_cast<QWidget *>(ctx.data());
			if (!w)
				return;
			const qreal dpr = w->devicePixelRatioF();
			const QPointF local(xPx / dpr, yPx / dpr);

			QEvent::Type type;
			Qt::MouseButton button = Qt::NoButton;
			Qt::MouseButtons buttons = Qt::NoButton;
			switch (action) {
			case 0: // ACTION_DOWN
				type = QEvent::MouseButtonPress;
				button = Qt::LeftButton;
				buttons = Qt::LeftButton;
				break;
			case 2: // ACTION_MOVE（拖拽中，buttons 保持按下态）
				type = QEvent::MouseMove;
				button = Qt::LeftButton;
				buttons = Qt::LeftButton;
				break;
			case 1: // ACTION_UP
			case 3: // ACTION_CANCEL
				type = QEvent::MouseButtonRelease;
				button = Qt::LeftButton;
				buttons = Qt::NoButton;
				break;
			default: // 多指/hover 先不接，pinch 缩放留后续
				return;
			}

			const QPointF global = QPointF(w->mapToGlobal(local.toPoint()));
			/* B-5 这条管路"到底有没有跑"的尺子（build 80 加）：之前只能从"预览区长按没反应"
			 * 倒推，分不清 ① Java 压根没回调过来、② 回调了但 ctx 不是那个挂着菜单策略的控件、
			 * ③ 投对了事件而 OBS 的 handler 没接。**ACTION_MOVE 不打** —— 一次拖拽就是几百行，
			 * 会把 logcat 环形缓冲里同一轮其它探针的证据冲掉。 */
			if (type != QEvent::MouseMove)
				obsAndroidDisplayLog("B-5 触摸转发：%s px=(%d,%d) → 逻辑(%.0f,%.0f) 投给「%s」%s",
						     action == 0 ? "DOWN" : (action == 1 ? "UP" : "CANCEL"), (int)xPx,
						     (int)yPx, local.x(), local.y(), qUtf8Printable(w->objectName()),
						     w->metaObject()->className());
			QMouseEvent ev(type, local, global, button, buttons, Qt::NoModifier);
			QApplication::sendEvent(w, &ev);
		},
		Qt::QueuedConnection);
}

static const JNINativeMethod kSurfaceNatives[] = {
	{(char *) "nativeSurfaceEvent", (char *) "(I)V", (void *) jniSurfaceEvent},
	{(char *) "nativeTouch", (char *) "(IFF)V", (void *) jniTouch},
};

static void ensureSurfaceNativesRegistered()
{
	static std::once_flag once;
	std::call_once(once, [] {
		QJniEnvironment env;
		if (!env.isValid()) {
			blog(LOG_WARNING, "OBSAndroidDisplay[Android]: B-4 注册 native 回调失败——JNIEnv 无效");
			return;
		}
		jclass clazz = env.jniEnv()->FindClass(kDisplayHostClass);
		if (!clazz) {
			env.jniEnv()->ExceptionClear();
			blog(LOG_WARNING, "OBSAndroidDisplay[Android]: B-4 FindClass(%s) 失败——surface 生命周期回调不生效",
			     kDisplayHostClass);
			return;
		}
		const jint rc = env.jniEnv()->RegisterNatives(clazz, kSurfaceNatives,
							      sizeof(kSurfaceNatives) / sizeof(kSurfaceNatives[0]));
		env.jniEnv()->DeleteLocalRef(clazz);
		if (rc != JNI_OK) {
			env.jniEnv()->ExceptionClear();
			blog(LOG_WARNING, "OBSAndroidDisplay[Android]: B-4 RegisterNatives 失败 rc=%d", (int) rc);
		} else {
			blog(LOG_INFO, "OBSAndroidDisplay[Android]: B-4 surface 生命周期 native 回调已注册");
		}
	});
}

/* 认领两个入口共用的实现。preempt=false 就是 B-8 的原语义（槽里有人就拒）；preempt=true 是
 * B-7 甲：把现任主人挂起、自己上位，并在**解锁之后**给它发 Revoked（在锁里发会当场回调进
 * OBSQTDisplay::releaseDisplay，那条又要拿同一把锁 → 自锁）。 */
static bool claimCommon(QObject *ctx, std::function<void(int)> onSurfaceEvent, bool preempt)
{
	QPointer<QObject> revokedCtx;
	std::function<void(int)> revokedFn;

	{
		std::lock_guard<std::mutex> lk(g_lifecycleMutex);

		if (g_lifecycleCtx && g_lifecycleCtx.data() != ctx) {
			if (!preempt) {
				obsAndroidDisplayLog("B-8 认领被拒：槽里是 ctx=%p，这次来的 %p 不是预览面的主人 —— 不 attach",
						     (void *)g_lifecycleCtx.data(), (void *)ctx);
				return false;
			}
			if (g_parkedCtx) {
				obsAndroidDisplayLog("B-7 抢占被拒：槽里 %p、挂起的还有 %p —— 只支持一层倒手，第二个投影仪不给面",
						     (void *)g_lifecycleCtx.data(), (void *)g_parkedCtx.data());
				return false;
			}
			revokedCtx = g_lifecycleCtx;
			revokedFn = g_onSurfaceEvent;
			g_parkedCtx = revokedCtx;
			g_onParkedEvent = revokedFn;
			obsAndroidDisplayLog("B-7 让位：现任主人 %p 挂起，新主人 %p 接管（面只有一块）",
					     (void *)revokedCtx.data(), (void *)ctx);
		}

		g_lifecycleCtx = ctx;
		g_onSurfaceEvent = std::move(onSurfaceEvent);
	}

	if (revokedCtx)
		postSurfaceEventTo(revokedCtx.data(), revokedFn, ObsAndroidSurfaceRevoked);

	obsAndroidDisplayLog("B-8 claimLifecycle ctx=%p（%s）", (void *)ctx, preempt ? "抢占接管" : "已认领");
	return true;
}

bool obsAndroidDisplayClaimLifecycle(QObject *ctx, std::function<void(int)> onSurfaceEvent)
{
	return claimCommon(ctx, std::move(onSurfaceEvent), false);
}

bool obsAndroidDisplayClaimLifecyclePreemptive(QObject *ctx, std::function<void(int)> onSurfaceEvent)
{
	return claimCommon(ctx, std::move(onSurfaceEvent), true);
}

void obsAndroidDisplayReleaseLifecycle(QObject *ctx)
{
	QPointer<QObject> promoteCtx;
	std::function<void(int)> promoteFn;

	{
		std::lock_guard<std::mutex> lk(g_lifecycleMutex);

		if (g_lifecycleCtx && g_lifecycleCtx.data() != ctx) {
			obsAndroidDisplayLog("B-8 releaseLifecycle %p 不是槽主（%p），不清", (void *)ctx,
					     (void *)g_lifecycleCtx.data());
			return;
		}

		g_lifecycleCtx = nullptr;
		g_onSurfaceEvent = nullptr;

		if (g_parkedCtx) {
			promoteCtx = g_parkedCtx;
			promoteFn = g_onParkedEvent;
			g_parkedCtx = nullptr;
			g_onParkedEvent = nullptr;
			/* 先把槽还回去再发门铃：它收到 Granted 后重跑 CreateDisplay，那条会再 claim 一次，
			 * 槽里已经是自己才不会被拒。 */
			g_lifecycleCtx = promoteCtx;
			g_onSurfaceEvent = promoteFn;
		}
	}

	if (promoteCtx) {
		obsAndroidDisplayLog("B-7 回位：预览面还给 %p（等它自己重跑 CreateDisplay）", (void *)promoteCtx.data());
		postSurfaceEventTo(promoteCtx.data(), promoteFn, ObsAndroidSurfaceGranted);
		return;
	}

	obsAndroidDisplayLog("B-8 releaseLifecycle ctx=0x0（解绑，之后的 surface 事件没人接）");
}

bool obsAndroidDisplayIsLifecycleOwner(QObject *ctx)
{
	std::lock_guard<std::mutex> lk(g_lifecycleMutex);

	/* 槽空着的时候谁都算主人：那时没有任何预览面需要保护，摘面是安全的。 */
	return !g_lifecycleCtx || g_lifecycleCtx.data() == ctx;
}

QObject *obsAndroidDisplayLifecycleOwner()
{
	std::lock_guard<std::mutex> lk(g_lifecycleMutex);

	return g_lifecycleCtx.data();
}

bool obsAndroidDisplayAttach(int x, int y, int w, int h, QString *detail)
{
	// 在真正 attach（→ 首个 surfaceCreated 回调）之前把 native 方法绑好，免得 Java 那头
	// 调未注册的原生方法抛 UnsatisfiedLinkError。call_once，注册一次即可。
	ensureSurfaceNativesRegistered();

	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("拿不到 QtNative.activity() —— 预览面不会建起来");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "attach",
								   "(Landroid/app/Activity;IIII)Z",
								   act.object<jobject>(), x, y, w, h);
	if (detail) {
		const QString status = obsAndroidDisplayStatus();
		*detail = ok ? QStringLiteral("attach(%1,%2 %3x%4px) 成功：%5").arg(x).arg(y).arg(w).arg(h).arg(status)
			     : QStringLiteral("attach(%1,%2 %3x%4px) 失败：%5").arg(x).arg(y).arg(w).arg(h).arg(status);
	}
	return ok != 0;
}

bool obsAndroidDisplayUpdateRect(int x, int y, int w, int h, QString *detail)
{
	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("updateRect: 拿不到 QtNative.activity()");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "updateRect",
								   "(Landroid/app/Activity;IIII)Z",
								   act.object<jobject>(), x, y, w, h);
	if (detail) {
		*detail = ok ? QStringLiteral("updateRect(%1,%2 %3x%4px) 已投递").arg(x).arg(y).arg(w).arg(h)
			     : QStringLiteral("updateRect(%1,%2 %3x%4px) 无活动面").arg(x).arg(y).arg(w).arg(h);
	}
	return ok != 0;
}

bool obsAndroidDisplayAcquire(ObsDisplaySurface *out, QString *detail)
{
	if (!out)
		return false;

	*out = ObsDisplaySurface{};

	const QJniObject surface =
		QJniObject::callStaticObjectMethod(kDisplayHostClass, "getSurface", "()Landroid/view/Surface;");
	if (!surface.isValid()) {
		if (detail)
			*detail = QStringLiteral("ObsDisplayHost.getSurface() 返回 null —— 没 attach 或已经 detach");
		return false;
	}

	/* fromSurface 会替我们加一次 ANativeWindow 引用（头文件原文："This acquires a reference
	 * on the ANativeWindow that is returned; be sure to use ANativeWindow_release()"）。
	 * swapchain 侧 gl_windowinfo_create 另加它自己的一次，两边不共享，所以这里必须
	 * 显式 obsAndroidDisplayRelease 还一次，不能省。 */
	QJniEnvironment env;
	if (!env.isValid()) {
		if (detail)
			*detail = QStringLiteral("当前线程没有可用的 JNIEnv（QJniEnvironment::isValid=false）");
		return false;
	}

	ANativeWindow *window = ANativeWindow_fromSurface(env.jniEnv(), surface.object<jobject>());
	if (!window) {
		if (detail)
			*detail = QStringLiteral("ANativeWindow_fromSurface 返回 null（Surface 无效？）");
		return false;
	}

	out->window = window;
	out->windowW = (uint32_t)ANativeWindow_getWidth(window);
	out->windowH = (uint32_t)ANativeWindow_getHeight(window);
	out->javaW = (uint32_t)callInt("surfaceWidth");
	out->javaH = (uint32_t)callInt("surfaceHeight");
	out->javaFormat = callInt("surfaceFormat");
	out->rect = callString("rectOnScreen");

	if (detail)
		*detail = QStringLiteral("ANativeWindow=%1x%2  Java surfaceChanged=%3x%4  rect=%5  fmt=%6")
				  .arg(out->windowW)
				  .arg(out->windowH)
				  .arg(out->javaW)
				  .arg(out->javaH)
				  .arg(out->rect.isEmpty() ? QStringLiteral("<空>") : out->rect)
				  .arg(out->javaFormat);
	return true;
}

void obsAndroidDisplayRelease(void *window)
{
	if (window)
		ANativeWindow_release((ANativeWindow *)window);
}

bool obsAndroidDisplayDetach(QString *detail)
{
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "detach", "()Z");
	if (detail)
		*detail = ok ? QStringLiteral("预览面已摘掉，surfaceDestroyed 回调到达")
			     : QStringLiteral("预览面摘掉了但没等到 surfaceDestroyed —— 见 OBS-display 标签原文");
	return ok != 0;
}

QString obsAndroidDisplayStatus()
{
	const QString desc = callString("describe");
	if (!desc.isEmpty())
		return desc;

	/* describe() 拿不到时（静态方法没调到 / 类没进 APK）用计数器兜一层 —— 全 0 且总线没接通
	 * 才是"桥没通"，和"接通了但一个 surface 回调都没发过"是两回事。 */
	return QStringLiteral("describe 空；计数 attach=%1 created=%2 changed=%3 destroyed=%4")
		.arg(callInt("attachCount"))
		.arg(callInt("createdCount"))
		.arg(callInt("changedCount"))
		.arg(callInt("destroyedCount"));
}

#endif // __ANDROID__
