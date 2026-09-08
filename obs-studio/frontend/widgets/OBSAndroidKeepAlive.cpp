#include "OBSAndroidKeepAlive.hpp"

#ifdef __ANDROID__

#include <QJniEnvironment>
#include <QJniObject>

#include <mutex>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsForegroundService.java）
 * 全是 public static，和 ObsDisplayHost 一样只走 callStaticMethod。 */
static const char kServiceClass[] = "com/obsproject/studio/ObsForegroundService";

/* 必须是 Activity：QtNative.activity() 是这套里唯一稳定的 Context 来源（同 OBSAndroidDisplay.cpp），
 * Java 侧立刻 getApplicationContext()，不把 Activity 引用留在静态字段上。 */
static QJniObject hostActivity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

static QString callDescribe()
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kServiceClass, "describe", "()Ljava/lang/String;");
	return s.isValid() ? s.toString() : QString();
}

/* 上一次递给 Java 的状态。前台服务只在"有没有输出在跑"这一维上开关，另加一个状态字符串
 * 只是给通知栏文案去重用的；真正的服务状态以 Java 侧 instance/计数器为准（describe 里能看到）。 */
static std::mutex g_keepAliveMutex;
static bool g_running = false;
static QString g_state;

bool obsAndroidKeepAliveSetRunning(bool running, const char *state, QString *detail)
{
	const QString what = state ? QString::fromUtf8(state) : QStringLiteral("running");
	QString jstatus;
	bool repeat = false;

	{
		std::lock_guard<std::mutex> lk(g_keepAliveMutex);
		if (!running && !g_running) {
			if (detail)
				*detail = QStringLiteral("本来就没在跑，忽略 stop");
			return true;
		}
		/* 3-5 看门狗每秒复核一次：状态没变就别再惊动 Java（重复 startForeground 会每秒重刷通知）。 */
		repeat = running && g_running && g_state == what;
		g_running = running;
		if (!repeat)
			g_state = what;
	}

	if (repeat) {
		if (detail)
			*detail = QStringLiteral("已在跑(%1)，忽略重复").arg(what);
		return true;
	}

	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("拿不到 QtNative.activity() —— 前台服务没起来");
		jstatus = obsAndroidKeepAliveStatus();
		blog(LOG_WARNING, "OBSAndroidKeepAlive[Android]: %s", jstatus.toUtf8().constData());
		return false;
	}

	bool ok;
	if (running) {
		const QJniObject jwhat = QJniObject::fromString(what);
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "start",
							   "(Landroid/content/Context;Ljava/lang/String;)Z",
							   act.object<jobject>(), jwhat.object<jstring>()) != 0;
	} else {
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "stop", "(Landroid/content/Context;)Z",
							    act.object<jobject>()) != 0;
	}

	jstatus = obsAndroidKeepAliveStatus();
	const QString verb = running ? QStringLiteral("起前台服务(%1)").arg(what) : QStringLiteral("停前台服务");
	const QString line = QStringLiteral("%1 %2：%3")
				     .arg(verb)
				     .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"))
				     .arg(jstatus);
	if (detail)
		*detail = line;
	blog(ok ? LOG_INFO : LOG_WARNING, "OBSAndroidKeepAlive[Android]: %s", line.toUtf8().constData());
	return ok;
}

QString obsAndroidKeepAliveStatus()
{
	const QString desc = callDescribe();
	if (!desc.isEmpty())
		return desc;
	/* describe() 调不到（类没进 APK / 静态方法没搜到）时至少把 native 侧的意图报出来，
	 * 与"服务在跑但 describe 返回空"是两回事。 */
	return QStringLiteral("describe 空；native 侧期望 running=%1").arg(g_running);
}

#endif // __ANDROID__
