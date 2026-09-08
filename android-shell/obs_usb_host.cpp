#include "obs_usb_host.h"

#include <obs.h> // obs_android_set_usb_host / struct obs_android_usb_device

#include <QByteArray>
#include <QStringList>
#include <QJniObject>

#include <cstring>

/* Java 侧的桥类（android-shell/android/java/org/qtproject/example/obs_shell/ObsUsbHost.java）
 * 全是 public static 方法，所以只用 QJniObject::callStaticObjectMethod / callStaticMethod。 */
static const char kUsbHostClass[] = "org/qtproject/example/obs_shell/ObsUsbHost";

static QString jstringToQString(const QJniObject &s)
{
	if (!s.isValid())
		return QString();
	return s.toString();
}

/* A1/A2 实测过 Qt 6.9.3 的暴露方式：org/qtproject/qt/android/QtNative 的
 * static Context getContext() / static Activity activity()（不是 Kotlin 属性名）。
 * 两处冒烟都这么拿 Context，保持一致。 */
static QJniObject appContext()
{
	QJniObject ctx;
	const char *const getters[] = {"getContext", "activity"};
	for (const char *g : getters) {
		ctx = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", g,
							 "()Landroid/content/Context;");
		if (ctx.isValid())
			break;
	}
	return ctx;
}

static void copyField(char *dst, size_t dstLen, const QString &src)
{
	if (!dst || dstLen == 0)
		return;
	const QByteArray utf8 = src.toUtf8();
	const size_t cap = dstLen - 1;
	const size_t n = (size_t)utf8.size() < cap ? (size_t)utf8.size() : cap;
	memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

/* --- obs_android_usb_host 回调：把 Java 的返回串拆成固定长结构体 ---------------- */

static int jbGetDevices(void *param, struct obs_android_usb_device *out, int max)
{
	(void)param;
	const QJniObject snap = QJniObject::callStaticObjectMethod(kUsbHostClass, "snapshot", "()Ljava/lang/String;");
	if (!snap.isValid())
		return -1;

	const QString joined = jstringToQString(snap);
	if (joined.isEmpty())
		return 0; // 没有设备是正常状态，不是错误

	const QStringList entries = joined.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	int n = 0;
	for (const QString &e : entries) {
		if (n >= max)
			break;
		const QStringList f = e.split(QLatin1Char('|'));
		if (f.size() < 3)
			continue; // 协议被 Java 侧改坏了，宁可不给也不猜
		copyField(out[n].name, sizeof(out[n].name), f.at(0));
		copyField(out[n].label, sizeof(out[n].label), f.at(1));
		out[n].has_permission = f.at(2) == QLatin1String("1");
		n++;
	}
	return n;
}

static int jbRequestPermission(void *param, const char *name)
{
	(void)param;
	return QJniObject::callStaticMethod<jint>(kUsbHostClass, "requestPermission", "(Ljava/lang/String;)I",
						  QJniObject::fromString(QString::fromUtf8(name)).object<jstring>());
}

static int jbOpenFd(void *param, const char *name)
{
	(void)param;
	return QJniObject::callStaticMethod<jint>(kUsbHostClass, "openFd", "(Ljava/lang/String;)I",
						  QJniObject::fromString(QString::fromUtf8(name)).object<jstring>());
}

static void jbCloseFd(void *param, int fd)
{
	(void)param;
	QJniObject::callStaticMethod<void>(kUsbHostClass, "closeFd", "(I)V", fd);
}

/* 生命周期约定：这个表必须比 libobs 里任何还活着的采集源活得久 —— 源在 deactivate
 * 里会调 close_fd。所以注册在 obsUsbHostInstall()（obs_startup 之前），注销在
 * obs_shutdown 之后（main.cpp 里就是这个顺序）。 */
static struct obs_android_usb_host g_usbHost = {
	nullptr,
	jbGetDevices,
	jbRequestPermission,
	jbOpenFd,
	jbCloseFd,
};

bool obsUsbHostInstall(QString *detail)
{
	const QJniObject ctx = appContext();
	if (!ctx.isValid()) {
		if (detail)
			*detail = QStringLiteral("USB 宿主未安装：QtNative.getContext()/activity() 都拿不到 Context");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kUsbHostClass, "install",
							   "(Landroid/content/Context;)Z", ctx.object<jobject>());
	if (!ok) {
		if (detail)
			*detail = QStringLiteral("ObsUsbHost.install() 返回 false —— 该设备没有 USB Host 服务？");
		return false;
	}

	obs_android_set_usb_host(&g_usbHost);
	if (detail)
		*detail = QStringLiteral("USB 宿主已安装（Java receiver + libobs 总线）");
	return true;
}

void obsUsbHostUninstall()
{
	obs_android_set_usb_host(nullptr);
}

bool obsUsbHostQuery(ObsUsbHostStatus *out, QString *detail)
{
	if (!out)
		return false;

	const jboolean inst = QJniObject::callStaticMethod<jboolean>(kUsbHostClass, "isInstalled", "()Z");
	const jint conns = QJniObject::callStaticMethod<jint>(kUsbHostClass, "connectionCount", "()I");
	const jint events = QJniObject::callStaticMethod<jint>(kUsbHostClass, "hotplugEvents", "()I");
	const QJniObject action = QJniObject::callStaticObjectMethod(kUsbHostClass, "lastAction", "()Ljava/lang/String;");
	const QJniObject perm =
		QJniObject::callStaticObjectMethod(kUsbHostClass, "lastPermissionResult", "()Ljava/lang/String;");

	/* 三个值全 0 有两种完全不同的原因：真的"刚装上、一个设备都没插过"，和"静态方法根本
	 * 没调到"（类名写错/类没进 APK）。后者必须报错，否则冒烟会把"桥没接通"读成"没设备"。
	 * 判据用 libobs 总线：install 成功时两边是一起置位的。 */
	if (!inst && !conns && !events && !obs_android_usb_ready()) {
		if (detail)
			*detail = QStringLiteral("Java 侧未安装且 libobs 总线也没注册 —— ObsUsbHost 的静态方法没调到");
		return false;
	}

	out->javaInstalled = inst != 0;
	out->connections = (int)conns;
	out->hotplugEvents = (int)events;
	out->lastAction = jstringToQString(action);
	out->lastPermissionResult = jstringToQString(perm);

	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	if (n < 0) {
		if (detail)
			*detail = QStringLiteral("obs_android_usb_enum_devices() 返回 %1（总线或 Java snapshot 有问题）").arg(n);
		return false;
	}
	out->devices = n;
	return true;
}

bool obsUsbHostSendTestHotplug(QString *detail)
{
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kUsbHostClass, "sendTestHotplug", "()Z");
	if (detail)
		*detail = ok ? QStringLiteral("测试热插拔广播已发出") : QStringLiteral("sendTestHotplug() 返回 false");
	return ok != 0;
}

bool obsUsbHostRefresh(QString *detail)
{
	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	if (n < 0) {
		if (detail)
			*detail = QStringLiteral("重扫失败（返回 %1）").arg(n);
		return false;
	}
	if (detail) {
		QStringList lines;
		for (int i = 0; i < n; i++)
			lines << QStringLiteral("  [%1] %2 / %3 / 权限=%4")
					 .arg(i)
					 .arg(QString::fromUtf8(devs[i].label))
					 .arg(QString::fromUtf8(devs[i].name))
					 .arg(devs[i].has_permission ? QStringLiteral("有") : QStringLiteral("无"));
		*detail = lines.isEmpty() ? QStringLiteral("重扫成功，当前 0 个 USB 设备")
					  : QStringLiteral("重扫成功，%1 个设备：\n%2").arg(n).arg(lines.join(QLatin1Char('\n')));
	}
	return true;
}
