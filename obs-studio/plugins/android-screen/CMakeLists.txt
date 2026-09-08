cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-screen MODULE)
add_library(OBS::android-screen ALIAS android-screen)

target_sources(android-screen PRIVATE screen-capture.c)
target_link_libraries(android-screen PRIVATE OBS::libobs)

if(OS_ANDROID)
  # 只链 mediandk：本文件用到的符号全是 AImageReader_* / AImage_*（都在 libmediandk.so 的桩库里），
  # 桩库那套"链接期要名字、运行时取系统分区真身、不打进 APK"的道理见 ../android-camera/CMakeLists.txt:10-18。
  #
  # 仍然**不**链 android（ANativeWindow_*）—— 相机那份注释里预告的"等真调了 ANativeWindow_toSurface
  # 再补"这件事真的来了，但补的地方不是这里：那道转换落在前端 OBSAndroidPermissions.cpp（前端本来就
  # link libandroid，见 build.ninja 里 frontend/libobs_*.so 那行的 -landroid）。本插件只从
  # AImageReader_getWindow 拿一个 ANativeWindow 指针往下递，一个 ANativeWindow_* 函数都没调用过，
  # 前向声明不需要库。把 android 挂到插件上是给整个插件层开一条别的插件都没有的依赖，不划算。
  target_link_libraries(android-screen PRIVATE mediandk)
endif()

set_target_properties_obs(android-screen PROPERTIES FOLDER plugins/android-screen PREFIX "")
