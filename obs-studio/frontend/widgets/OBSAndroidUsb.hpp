// USB host 总线（obs_android_usb_*）的前端注册口。Java 侧 = ObsUsbHost.java。
// 自 android-shell/obs_usb_host.h 移植：形状与 OBSAndroidPermissions.hpp 里相机/屏幕两条总线
// 的注册口一致（幂等、App 启动时调一次）。
#pragma once

#include <QString>

// 把 USB host 总线的四条腿（枚举/申请授权/open_fd/close_fd）注册进 libobs 的
// obs_android_usb_*，并把 Java 侧 ObsUsbHost 装上（UsbManager + 热插拔 receiver）。
// 不调这一句的话，android-capture 的 UVC 源（android_video_input）属性页会显示"总线未接通"
// —— 它能编能跑，但枚举不到任何 USB 设备。注册幂等，App 启动时调一次即可。
// 返回 false 只代表"没装上"（拿不到 Context / 机器没有 USB Host 服务），detail 给原因。
bool obsAndroidUsbRegisterBus(QString *detail = nullptr);
