cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-audio MODULE)
add_library(OBS::android-audio ALIAS android-audio)

target_sources(android-audio PRIVATE aaudio-input.c)
# aaudio 是 NDK 公共库（API 26+）：链接期用的是 sysroot 里的桩库，
# 运行时由 /system/lib64/libaaudio.so 提供，所以不需要（也不应该）打进 APK。
target_link_libraries(android-audio PRIVATE OBS::libobs aaudio)

set_target_properties_obs(android-audio PROPERTIES FOLDER plugins/android-audio PREFIX "")
