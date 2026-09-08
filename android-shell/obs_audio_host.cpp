#include "obs_audio_host.h"

#include <obs.h> // obs_android_set_audio_host / struct obs_android_audio_device

#include <QByteArray>
#include <QStringList>
#include <QJniObject>

#include <cstring>

/* Java 侧的桥类（android-shell/android/java/org/qtproject/example/obs_shell/ObsAudioHost.java）
 * 全是 public static 方法，所以只用 callStaticObjectMethod / callStaticMethod。 */
static const char kAudioHostClass[] = "org/qtproject/example/obs_shell/ObsAudioHost";

/* 与 obs_usb_host.cpp 里那份是同一个十行小工具。没抽成公共头：两处各留一份，改动面小，
 * 而且哪天 Qt 换了取 Context 的方式，两个文件一起报错比一个公共头悄悄生效更好查。 */
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
	const size_t n = (size_t) utf8.size() < cap ? (size_t) utf8.size() : cap;
	memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

/* --- obs_android_audio_host 回调：把 Java 的快照串拆成固定长结构体 ---------------- */

/* 快照协议：id|type|isSource|product|label，条目之间用 ';'。
 * 前两个字段是数字 —— 拆不出数字就不能当设备 id 用，宁可丢掉这条。 */
static int jbGetAudioDevices(void *param, struct obs_android_audio_device *out, int max)
{
	(void) param;
	const QJniObject snap =
		QJniObject::callStaticObjectMethod(kAudioHostClass, "snapshot", "()Ljava/lang/String;");
	if (!snap.isValid())
		return -1; // Java 侧压根没调到，和"没有设备"是两种故障

	const QString joined = snap.toString();
	if (joined.isEmpty())
		return 0; // 没有输入设备是合法状态

	const QStringList entries = joined.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	int n = 0;
	int dropped = 0;
	for (const QString &e : entries) {
		if (n >= max)
			break;
		const QStringList f = e.split(QLatin1Char('|'));
		if (f.size() < 5) {
			dropped++;
			continue; // 协议被改坏：宁可不给也不猜
		}
		bool okId = false, okType = false;
		const int id = f.at(0).toInt(&okId);
		const int type = f.at(1).toInt(&okType);
		if (!okId || !okType) {
			dropped++;
			continue;
		}
		out[n].id = id;
		out[n].type = type;
		out[n].is_source = (f.at(2) == QLatin1String("1"));
		copyField(out[n].product, sizeof(out[n].product), f.at(3));
		copyField(out[n].label, sizeof(out[n].label), f.at(4));
		n++;
	}

	/* 一条都没拆出来但串不为空：这是协议对不上，不是"没有设备"。报 -1 让调用方去查桥，
	 * 别把"拆包坏了"伪装成"这台机器没麦克风"。 */
	if (n == 0 && dropped > 0)
		return -1;
	return n;
}

static bool jbHasCapturePermission(void *param)
{
	(void) param;
	return QJniObject::callStaticMethod<jboolean>(kAudioHostClass, "hasRecordPermission", "()Z") != 0;
}

/* 生命周期同 USB 桥：表必须比任何还活着的采集源活得久，所以注册在 obs_startup 之前、
 * 注销在 obs_shutdown 之后（见 main.cpp 的调用顺序）。 */
static struct obs_android_audio_host g_audioHost = {
	nullptr,
	jbGetAudioDevices,
	jbHasCapturePermission,
};

bool obsAudioHostInstall(QString *detail)
{
	const QJniObject ctx = appContext();
	if (!ctx.isValid()) {
		if (detail)
			*detail = QStringLiteral("音频宿主未安装：QtNative.getContext()/activity() 都拿不到 Context");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kAudioHostClass, "install",
							   "(Landroid/content/Context;)Z", ctx.object<jobject>());
	if (!ok) {
		if (detail)
			*detail = QStringLiteral("ObsAudioHost.install() 返回 false —— 这台设备没有 AudioManager 服务？");
		return false;
	}

	obs_android_set_audio_host(&g_audioHost);
	if (detail)
		*detail = QStringLiteral("音频宿主已安装（Java callback + receiver + libobs 总线）");
	return true;
}

void obsAudioHostUninstall()
{
	obs_android_set_audio_host(nullptr);
}

bool obsAudioHostQuery(ObsAudioHostStatus *out, QString *detail)
{
	if (!out)
		return false;

	out->javaInstalled = QJniObject::callStaticMethod<jboolean>(kAudioHostClass, "isInstalled", "()Z") != 0;
	out->hasPermission = QJniObject::callStaticMethod<jboolean>(kAudioHostClass, "hasRecordPermission", "()Z") != 0;
	out->deviceCallbacks = (int) QJniObject::callStaticMethod<jint>(kAudioHostClass, "deviceCallbacks", "()I");
	out->hotplugEvents = (int) QJniObject::callStaticMethod<jint>(kAudioHostClass, "hotplugEvents", "()I");
	out->lastCallbackOnMain = (int) QJniObject::callStaticMethod<jint>(kAudioHostClass, "lastCallbackOnMain", "()I");
	out->devices = (int) QJniObject::callStaticMethod<jint>(kAudioHostClass, "deviceCount", "()I");
	out->lastAction = QJniObject::callStaticObjectMethod(kAudioHostClass, "lastAction", "()Ljava/lang/String;")
				 .toString();

	struct obs_android_audio_device devs[16];
	const int n = obs_android_audio_enum_devices(devs, 16);
	if (n < 0) {
		if (detail)
			*detail = QStringLiteral("总线枚举返回 %1（音频总线没注册，或 Java snapshot 拆不出来）").arg(n);
		return false;
	}
	out->busDevices = n;

	/* 全 0 有两种原因：真的一次设备增减都没发生过，和静态方法压根没调到（类名写错 / 类没进
	 * APK）。后者必须报错，判据用 libobs 总线 —— install 成功时两边是一起置位的。 */
	if (!out->javaInstalled && !obs_android_audio_ready()) {
		if (detail)
			*detail = QStringLiteral("Java 侧未安装且 libobs 总线也没注册 —— ObsAudioHost 的静态方法没调到");
		return false;
	}

	if (detail)
		*detail = QStringLiteral("Java 台数=%1，总线台数=%2").arg(out->devices).arg(n);
	return true;
}

bool obsAudioHostSendTestUpdate(QString *detail)
{
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kAudioHostClass, "sendTestUpdate", "()Z");
	if (detail)
		*detail = ok ? QStringLiteral("测试刷新广播已发出") : QStringLiteral("sendTestUpdate() 返回 false（还没 install?）");
	return ok != 0;
}

bool obsAudioHostList(QStringList *lines, int *count, QString *detail)
{
	if (!lines || !count)
		return false;

	struct obs_android_audio_device devs[16];
	const int n = obs_android_audio_enum_devices(devs, 16);
	if (n < 0) {
		if (detail)
			*detail = QStringLiteral("重扫失败（返回 %1）").arg(n);
		return false;
	}

	*count = n;
	for (int i = 0; i < n; i++) {
		*lines << QStringLiteral("  [%1] id=%2 type=%3 可采集=%4 / '%5' / '%6'")
                              .arg(i)
                              .arg(devs[i].id)
                              .arg(devs[i].type)
                              .arg(devs[i].is_source ? QStringLiteral("是") : QStringLiteral("否"))
                              .arg(QString::fromUtf8(devs[i].label))
                              .arg(QString::fromUtf8(devs[i].product));
	}
	if (detail)
		*detail = n ? QStringLiteral("总线枚举到 %1 台输入设备").arg(n) : QStringLiteral("总线枚举到 0 台输入设备");
	return true;
}
