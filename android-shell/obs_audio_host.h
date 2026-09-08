#pragma once

#include <QString>
#include <QStringList>

/* A4：Android 音频输入设备宿主桥（plan.md 阶段 2 的 A4）
 *
 * Java 侧 ObsAudioHost 负责 Framework 那套（AudioManager.getDevices(GET_DEVICES_INPUTS)
 * + AudioDeviceCallback + 麦克风静音/耳机拔出广播），这里把它包成 libobs 的
 * obs_android_audio_host 回调表注册进 libobs，android-audio 插件就只依赖 libobs 的公开符号。
 *
 * 与 obs_usb_host.h 同构，但回调表只有两条：音频既没有 fd（AAudio 自己 open），也没有
 * 按设备授权（RECORD_AUDIO 是一次性运行时权限），所以桥只负责"给清单 + 给权限状态"。
 */

/** 冒烟/诊断要看的宿主状态 */
struct ObsAudioHostStatus {
	bool javaInstalled = false;
	bool hasPermission = false;
	/** Java 侧直接数出来的输入设备台数 */
	int devices = 0;
	/** 走 libobs 总线枚举到的台数 —— 两个数必须相等，不等就是桥的拆包出了问题 */
	int busDevices = 0;
	/** AudioDeviceCallback 被 Framework 调过几次（模拟器上大概是 0） */
	int deviceCallbacks = 0;
	/** 回调 + 广播合计的刷新次数 */
	int hotplugEvents = 0;
	/** 上一次回调跑在哪：-1 从没跑过 / 0 非主线程 / 1 主线程 */
	int lastCallbackOnMain = -1;
	QString lastAction;
};

/** 取 Context → Java install() → 把回调表注册进 libobs。幂等。 */
bool obsAudioHostInstall(QString *detail);

/** 注销总线（obs_shutdown 之后调，避免拆链途中插件还能回调进 Java） */
void obsAudioHostUninstall();

/** 读 Java 侧 + 总线两侧的状态。返回 false = JNI 调用失败，*detail 给原因 */
bool obsAudioHostQuery(ObsAudioHostStatus *out, QString *detail);

/** 往自己 App 内发一条测试刷新广播（验 receiver 链路的唯一办法） */
bool obsAudioHostSendTestUpdate(QString *detail);

/** 把总线枚举到的清单格式化成人话（冒烟直接贴进结论） */
bool obsAudioHostList(QStringList *lines, int *count, QString *detail);
