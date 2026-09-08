cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-camera MODULE)
add_library(OBS::android-camera ALIAS android-camera)

target_sources(android-camera PRIVATE camera2-input.c)
target_link_libraries(android-camera PRIVATE OBS::libobs)

if(OS_ANDROID)
  # Camera2 NDK。按裸名链就行 —— 前端 os-android.cmake:49 链 mediandk/android 用的是同一套写法，
  # NDK 工具链会把名字解到 <sysroot>/usr/lib/<triple>/30/libcamera2ndk.so 那枚**桩库**上：
  # 链接期只要 DT_NEEDED 里有个名字，运行时 linker 从系统分区取真身 ⇒ 不需要（也不能）把
  # libcamera2ndk.so 打进 APK，与 obs-ffmpeg 那几枚 .so 的打包账无关。
  #
  # 这里链 mediandk 是给 c 腿的 AImageReader_* / AImage_*（两组符号都导出在 libmediandk.so 的桩库里）。
  # 仍然**不**链 android（ANativeWindow_*）：本文件只是从 AImageReader_getWindow 拿到一个
  # ANativeWindow 指针往下递，一个 ANativeWindow_* 函数都没调用过 —— 前向声明不需要库，
  # 等真调了（比如 P-18 的 ANativeWindow_toSurface）再补，不提前挂。
  target_link_libraries(android-camera PRIVATE camera2ndk mediandk)
endif()

set_target_properties_obs(android-camera PROPERTIES FOLDER plugins/android-camera PREFIX "")
