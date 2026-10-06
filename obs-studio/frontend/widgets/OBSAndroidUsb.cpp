#include "OBSAndroidUsb.hpp"

#ifdef __ANDROID__

#include <obs.h> // obs_android_set_usb_host / struct obs_android_usb_device

#include <QByteArray>
#include <QStringList>
#include <QJniObject>

#include <cstring>

/* 自 android-shell/obs_usb_host.cpp 忠实移植，改动只有两处：
 *   * Java 类名从 org/qtproject/example/obs_shell/ObsUsbHost 换成 com/obsproject/studio/ObsUsbHost；
 *   * 冒烟/诊断函数（query/refresh/sendTestHotplug）没有前端调用方，不搬；
 *     注册口换成与相机/屏幕总线同形状的 obsAndroidUsbRegisterBus()，并在注册时做一次现调探针。
 *
 * Java 侧的桥类（frontend/cmake/android/java/com/obsproject/studio/ObsUsbHost.java）
 * 全是 public static 方法，所以只用 QJniObject::callStaticObjectMethod / callStaticMethod。 */
static const char kUsbHostClass[] = "com/obsproject/studio/ObsUsbHost";

static QString jstringToQString(const QJniObject &s)
{
	if (!s.isValid())
		return QString();
	return s.toString();
}

/* A1/A2 实测过 Qt 6.9.3 的暴露方式：org/qtproject/qt/android/QtNative 的
 * static Context getContext() / static Activity activity()（不是 Kotlin 属性名）。
 * 这里只要通用 Context（registerReceiver/openDevice 都是 Context 的方法），两个口哪个通拿哪个。 */
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

/* 生命周期约定与相机总线（kCameraHost）相同：文件级 const、退出时不注销。四条腿里拿不到
 * Java 对象/类时全部按失败值返回（-1 / 空表），不会在拆链途中抛 JNI 异常 —— 这就是
 * 这张表可以不为退出专门注销的原因。 */
static const struct obs_android_usb_host kUsbHost = {
	nullptr,
	jbGetDevices,
	jbRequestPermission,
	jbOpenFd,
	jbCloseFd,
};

bool obsAndroidUsbRegisterBus(QString *detail)
{
	/* 与相机/屏幕总线同一个幂等形状：总线本身是覆盖写，重复注册没事，但"只接一次"用
	 * 这个 bool 兜住，日志也只打一遍。 */
	static bool s_bus_registered = false;
	if (s_bus_registered) {
		if (detail)
			*detail = QStringLiteral("USB 总线已注册过（幂等跳过）");
		return true;
	}
	s_bus_registered = true;

	const QJniObject ctx = appContext();
	if (!ctx.isValid()) {
		const QString line = QStringLiteral("USB 宿主未安装：QtNative.getContext()/activity() 都拿不到 Context");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidUsb[Android]: %s", line.toUtf8().constData());
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kUsbHostClass, "install",
								   "(Landroid/content/Context;)Z", ctx.object<jobject>());
	if (!ok) {
		const QString line = QStringLiteral("ObsUsbHost.install() 返回 false —— 该设备没有 USB Host 服务？");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidUsb[Android]: %s", line.toUtf8().constData());
		return false;
	}

	obs_android_set_usb_host(&kUsbHost);

	/* 现调探针（与屏幕总线注册处同一个思路）：这一轮还没有任何采集源来问，所以
	 * "native → libobs 总线 → JNI → Java snapshot 走得通"这件事只有靠这一发有一手证据。
	 * 打出 ready=1 且设备数 >=0 就说明总线指针对了、JNI 方法名签名对了、类真在 APK 里。 */
	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	QStringList lines;
	for (int i = 0; i < n; i++)
		lines << QStringLiteral("%1(%2 权限=%3)")
				 .arg(QString::fromUtf8(devs[i].label))
				 .arg(QString::fromUtf8(devs[i].name))
				 .arg(devs[i].has_permission ? QStringLiteral("有") : QStringLiteral("无"));
	const QString line = QStringLiteral("USB 总线四条腿已注册（ready=%1 枚举=%2）%3")
				     .arg(obs_android_usb_ready() ? 1 : 0)
				     .arg(n)
				     .arg(lines.isEmpty() ? QStringLiteral("") : QStringLiteral("：") + lines.join(QStringLiteral("，")));
	if (detail)
		*detail = line;
	blog(n < 0 ? LOG_WARNING : LOG_INFO, "OBSAndroidUsb[Android]: %s", line.toUtf8().constData());
	return true;
}

#endif // __ANDROID__
