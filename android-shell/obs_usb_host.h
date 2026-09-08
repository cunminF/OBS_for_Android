#pragma once

#include <QString>

/* Android 无 root 的 USB 宿主桥（plan.md 阶段 2 第 3 项 / A3）
 *
 * Java 侧 ObsUsbHost 负责 Framework 那套（枚举 / 授权 / openDevice / 热插拔广播），
 * 这里把它包成 libobs 的 obs_android_usb_host 回调表注册进 libobs，采集插件
 * （android-capture / 后续 android-usb-audio）就只依赖 libobs 的公开符号。
 *
 * 为什么不让插件自己走 JNI：插件是纯 C 的 .so，只链 libobs，既没有 Qt 也没有 JavaVM。
 * 实测到的是插件 .so 的 NEEDED 只有 libc/libdl/libm/liblibobs（llvm-readobj 量的，
 * 见 plan.md 8.14）；"插件 dlsym(RTLD_DEFAULT) 也够不到壳主 .so 里的符号"是按 Android
 * 的加载语义推的，没做反查实验。本方案只依赖前半句：libobs 是唯一两边都链得到的地方。
 */

/** 冒烟/诊断要看的宿主状态 */
struct ObsUsbHostStatus {
	bool javaInstalled = false;
	int devices = 0;
	int connections = 0;
	int hotplugEvents = 0;
	QString lastAction;
	QString lastPermissionResult;
};

/** 取 Context → Java install() → 把回调表注册进 libobs。幂等。
 *  返回 false 时 *detail 说明卡在哪一步（不致命：没有 USB Host 的机器上就该 false）。 */
bool obsUsbHostInstall(QString *detail);

/** 注销总线（obs_shutdown 之后调，避免插件在拆链途中还能回调进 Java） */
void obsUsbHostUninstall();

/** 读 Java 侧状态。返回 false = JNI 调用失败，*detail 给原因 */
bool obsUsbHostQuery(ObsUsbHostStatus *out, QString *detail);

/** 往自己 App 内发一条测试热插拔广播（模拟器上验 receiver 链路的唯一办法） */
bool obsUsbHostSendTestHotplug(QString *detail);

/** 主动重扫设备并把结果写进日志缓冲（*detail 收到条数与错误） */
bool obsUsbHostRefresh(QString *detail);
