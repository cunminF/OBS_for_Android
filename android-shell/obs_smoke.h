// 阶段 1 冒烟：把 libobs 真正跑起来（obs_startup / obs_shutdown）。
#pragma once

#include <QString>

// 准备 OBS_ROOT_PATH、解包 libobs 数据、装 blog 转发，然后跑一遍 startup/shutdown。
// 返回一段可直接显示/打日志的多行结论（同时逐行进 logcat 标签 OBS-SMOKE）。
QString runObsSmoke();

// 里程碑 M2：obs_reset_video 建 GLES 设备 → 全量 effect 编译 → 离屏渲染 + 像素回读。
QString runObsRenderSmoke();

// 里程碑 M3：obs_load_all_modules 加载插件（image-source / obs-x264 / obs-encode-sink）
// → color source 进合成链 → x264 编出 H.264 Annex-B 码流并核对 NAL。
QString runObsModuleSmoke();

// 阶段 2 的 A1：android-audio 插件（AAudio 采集源）。
// obs_reset_audio 起软件混音链 → 建 android_audio_input 源 → 用
// obs_source_add_audio_capture_callback 数回调/帧数/峰值，证明"设备麦克风 →
// AAudio → obs_source_output_audio → libobs 音频链"整条路是通的。
QString runObsAudioSmoke();

// 阶段 2 的 A2：USB fd 传递基建 + android-capture 的无设备路径。
// obs_load_all_modules 加载静态链了 libusb/libuvc/libjpeg 的插件 → JNI 查
// UsbManager 的 usb.host 特性与设备枚举 → 用 fd=0 / fd=-1 / fd=/dev/null 三个
// 取值驱动插件走三条不同分支，靠 blog 环形缓冲把「到底走了哪条」读回来。
// 真实取流仍受 OTG 真机限制（plan.md 8.8 第 10 项）。
QString runObsUsbSmoke();

// 阶段 2 的 A3：USB 热插拔监听 + 授权请求 + 设备选择属性页。
// 逐段验这条桥：Java ObsUsbHost（UsbManager/receiver）→ 壳 obs_usb_host.cpp →
// libobs obs_android_usb_* 总线 → android-capture 按设备名借 fd。
// 测不到的仍是"真摄像头出帧"（模拟器无 OTG，plan.md 8.8 第 10 项）。
QString runObsUsbHostSmoke();

// 阶段 2 的 A4：音频输入设备多路枚举 + 属性下拉框 + 低延迟/EXCLUSIVE/MMAP 实测。
// 逐段验这条桥：Java ObsAudioHost（AudioManager.getDevices + AudioDeviceCallback/receiver）
// → 壳 obs_audio_host.cpp → libobs obs_android_audio_* 总线 → android-audio 拿设备 id
// 喂 AAudioStreamBuilder_setDeviceId()。
// 与 USB 域不同的是这里模拟器上**真有**输入设备，所以能测到"选中的设备真的出帧"；
// 测不到的是 MMAP 数据通路（AAudio 的 MMAP 接口是 API 36 才有的，本机是 API 35）。
QString runObsAudioHostSmoke();

// 阶段 3 的前置 · S1：swapchain 上屏。
// Java ObsDisplayHost（SurfaceView + z 序 + surface 三回调）→ 壳 obs_display_host.cpp
// （ANativeWindow_fromSurface）→ libobs obs_display_create（在图形线程上）→
// gl-android 的 eglCreateWindowSurface/eglSwapBuffers 分支。
// 这段必须在 Qt 窗口已经 attach 之后再跑（要等 surfaceCreated），且不能挡 UI 线程，
// 所以 main.cpp 把它放到工作线程上、window.show() 之后触发 —— 和前面几段不同。
// 结束时故意不 obs_shutdown：颜色留在屏幕上给截图采样。
QString runObsSwapchainSmoke();
