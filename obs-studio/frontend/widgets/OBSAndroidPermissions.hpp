// 3-5（plan.md §五）：一期运行时权限的申请口。Java 侧 = ObsPermissionHost.java。
// 只负责"把系统弹窗弹出来 + 现读状态"，不接 onRequestPermissionsResult（那个回调归 Qt 自己的
// QtActivityDelegate，插不进去），所以要查结果就在下一次事件里再读一次。
#pragma once

#include <QString>

// 起一次申请（缺的几个并成一个系统框）。detail 带 Java describe() 的原文，供 blog/冒烟。
// 返回 false 只代表"没拿到 Activity"，不代表用户拒绝。
bool obsAndroidPermissionsRequestStartup(QString *detail = nullptr);

// 一行状态快照（各权限授予态 + 请求次数 + 最后一次动作/错误）。
QString obsAndroidPermissionsStatus();

// P-17：把相机总线的三条腿注册进 libobs 的 obs_android_* —— 查权限 / 申请权限 / "此刻该不该占着相机"
// （第三条由 Qt 的 applicationStateChanged 喂，见 .cpp 里 kCameraHost 那段）。
// 不调这一句的话，采集插件里 obs_android_camera_ready() 恒 false —— 它能编能跑，
// 但属性页分不清"这台没相机"和"没给权限"，而退后台时也不会交还设备。注册是幂等的，App 启动时调一次即可。
void obsAndroidPermissionsRegisterCameraBus();

// P-18-a：把屏幕采集的同意总线注册进 libobs 的 obs_android_screen_* —— 两格：问有没有令牌 /
// 去要一次令牌。Java 侧是 ObsProjectionHost（一个收 onActivityResult 的透明代理 Activity），
// 不是这个文件里那个 ObsPermissionHost：MediaProjection 是一次性 token、系统那边没有可查状态。
// 不调这一句的话采集插件里 obs_android_screen_ready() 恒 false —— 属性页会说"没令牌"，
// 而用户分不清"没同意"与"壳没接线"。注册幂等，App 启动时调一次即可。
void obsAndroidPermissionsRegisterScreenBus();
