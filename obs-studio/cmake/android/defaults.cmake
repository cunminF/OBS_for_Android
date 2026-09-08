# OBS CMake Android defaults module
#
# Android 上没有 FHS 文件系统概念：产物最终全部塞进 APK，
#   * .so            -> lib/<abi>/
#   * data/ 资源      -> assets/，首次启动解包到 app 私有目录
# 这里的 OBS_*_DESTINATION 只是给 helpers.cmake 的 rundir 复制逻辑用的相对路径，
# 真正的落地位置由 obs-android.c 在运行时用 JNI 取到的私有目录决定（阶段 1）。

include_guard(GLOBAL)

option(ENABLE_V4L2 "Enable V4L2 capture support" OFF)
option(ENABLE_BROWSER "Enable browser plugin (CEF)" OFF)

include(GNUInstallDirs)

set(OBS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rundir")
set(OBS_EXECUTABLE_DESTINATION "${CMAKE_INSTALL_BINDIR}")
set(OBS_INCLUDE_DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/obs")
set(OBS_LIBRARY_DESTINATION "${CMAKE_INSTALL_LIBDIR}")
set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
set(OBS_SCRIPT_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-scripting")
set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs")
set(OBS_CMAKE_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake")

set(OBS_PLUGIN_PATH "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
set(OBS_SCRIPT_PLUGIN_PATH "${CMAKE_INSTALL_LIBDIR}/obs-scripting")
set(OBS_DATA_PATH "${OBS_DATA_DESTINATION}")
set(OBS_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# 可执行文件不存在（OBS 在 Android 上是 libobs-studio.so + Qt Activity 载入）
set(OBS_EXECUTABLE_RPATH "")
set(OBS_LIBRARY_RPATH "")
set(OBS_MODULE_RPATH "")

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

# NDK 工具链把查找限制在 sysroot 内（*_MODE_*=ONLY），交叉编译的依赖前缀会全部找不到。
# 本文件在 project() 之后立刻被顶层 CMakeLists include，因此这里的设置能覆盖工具链的值。
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
