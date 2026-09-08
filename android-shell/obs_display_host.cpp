#include "obs_display_host.h"

#include <QJniEnvironment>
#include <QJniObject>

#include <android/native_window.h>
#include <android/native_window_jni.h>

/* Java 侧宿主（android-shell/android/java/.../ObsDisplayHost.java）全是 public static 方法，
 * 所以只用 QJniObject::callStaticObjectMethod / callStaticMethod。 */
static const char kDisplayHostClass[] = "org/qtproject/example/obs_shell/ObsDisplayHost";

/* 必须是 Activity，不能退化成 Context：addContentView/findViewById 只在 Activity 上，
 * application Context 拿不到（.qoder/s1-display-api.log 第 1 节）。这一点和 USB/音频宿主不同 ——
 * 那两个只要 Context，所以它们那两个 getter 轮着试的写法在这儿不适用。 */
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

bool obsDisplayHostAttach(int widthDp, int heightDp, QString *detail)
{
	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("拿不到 QtNative.activity() —— 显示面不会建起来");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "attach",
								   "(Landroid/app/Activity;II)Z",
								   act.object<jobject>(), widthDp, heightDp);
	if (detail) {
		const QString status = obsDisplayHostStatus();
		*detail = ok ? QStringLiteral("attach(%1x%2dp) 成功：%3").arg(widthDp).arg(heightDp).arg(status)
			     : QStringLiteral("attach(%1x%2dp) 失败：%3").arg(widthDp).arg(heightDp).arg(status);
	}
	return ok != 0;
}

bool obsDisplayHostAcquire(ObsDisplaySurface *out, QString *detail)
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

	/* fromSurface 会替我们加一次 ANativeWindow 引用（头文件注释原文："This acquires a reference
	 * on the ANativeWindow that is returned; be sure to use ANativeWindow_release()"）。
	 * swapchain 侧 gl_windowinfo_create 另加它自己的一次，两边不共享，所以这里必须
	 * 显式 obsDisplayHostRelease 还一次，不能省。 */
	QJniEnvironment env;
	if (!env.isValid()) {
		if (detail)
			*detail = QStringLiteral("当前线程没有可用的 JNIEnv（QJniEnvironment::isValid=false）—— 没调 AttachCurrentThread");
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

void obsDisplayHostRelease(void *window)
{
	if (window)
		ANativeWindow_release((ANativeWindow *)window);
}

bool obsDisplayHostDetach(QString *detail)
{
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kDisplayHostClass, "detach", "()Z");
	if (detail)
		*detail = ok ? QStringLiteral("显示面已摘掉，surfaceDestroyed 回调到达")
			     : QStringLiteral("显示面摘掉了但没等到 surfaceDestroyed —— 见 OBS-display 标签原文");
	return ok != 0;
}

QString obsDisplayHostStatus()
{
	const QString desc = callString("describe");
	if (!desc.isEmpty())
		return desc;

	/* describe() 拿不到时（静态方法根本没调到 / 类没进 APK）用计数器兜一层 —— 这几个值全 0
	 * 且总线没接通才是"桥没通"，和"接通了但一个 surface 回调都没发过"是两回事。 */
	return QStringLiteral("describe 空；计数 attach=%1 created=%2 changed=%3 destroyed=%4")
		.arg(callInt("attachCount"))
		.arg(callInt("createdCount"))
		.arg(callInt("changedCount"))
		.arg(callInt("destroyedCount"));
}
