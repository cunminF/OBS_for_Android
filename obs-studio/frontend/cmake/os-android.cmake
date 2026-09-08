# OBS CMake Android frontend platform module
#
# 对照 os-linux.cmake 的差别全部来自"Android 的 Qt 套件里没有这些东西"，实测依据：
#   * Qt6DBus      —— 不存在（$Qt/6.9.3/android_*/lib/cmake/ 下只有 Concurrent/Core/Gui/
#                     Network/OpenGL/OpenGLWidgets/PrintSupport/Svg/SvgWidgets/Widgets/Xml），
#                     所以 platform-x11.cpp 那份走 XDG portal 的实现整体换掉，不链 Qt::DBus。
#   * Libpci       —— Android 没有 PCI 总线枚举，system-info 换成读系统属性的实现。
#   * Python       —— 阶段 6 之前不接脚本，见 cmake/android/defaults.cmake。
#   * metainfo/desktop/图标 install —— FHS 桌面集成产物，Android 用不到。

target_sources(
  obs-studio
  PRIVATE
    utility/AndroidRuntimeBootstrap.cpp
    utility/CrashHandler_Android.cpp
    utility/NativeEventFilter.cpp
    utility/platform-android.cpp
    utility/system-info-android.cpp
    # 路线 B（3-2③ B-1）：预览控件 → Java SurfaceView → ANativeWindow 的宿主桥。
    # 对应的 Java 侧在 cmake/android/java/com/obsproject/studio/ObsDisplayHost.java，
    # androiddeployqt 从 QT_ANDROID_PACKAGE_SOURCE_DIR 自动收 java/ 目录，不需要在这里列。
    widgets/OBSAndroidDisplay.cpp
    # 3-5：推流/录制期间的前台服务起停桥（Java 侧 cmake/android/java/.../ObsForegroundService.java）。
    widgets/OBSAndroidKeepAlive.cpp
    # 3-5：一期运行时权限申请（Java 侧 .../ObsPermissionHost.java）。
    widgets/OBSAndroidPermissions.cpp
    # 3-4：桌面 "openUrl(本地路径)" 的 Android 替代（显示路径 + 复制剪贴板）。
    widgets/OBSAndroidPath.cpp
)

target_compile_definitions(
  obs-studio
  PRIVATE OBS_INSTALL_PREFIX="${OBS_INSTALL_PREFIX}" $<$<BOOL:${ENABLE_PORTABLE_CONFIG}>:ENABLE_PORTABLE_CONFIG>
)

# 链接期补两类桌面平台上会"自动带上"的依赖。实测依据 = pass-6 的 ld.lld 报错，
# 再用 llvm-nm 逐个归档定位引用方/定义方：
#
# 1) swresample：frontend/CMakeLists.txt:12 只要了 avcodec/avutil/avformat 三个组件。
#    桌面平台 FFmpeg 是动态库，符号由 libavcodec.so 自己带走；Android 这侧 FFmpeg
#    全是 .a，libavcodec.a 里有 1 处对 swr_convert 的未定义引用，定义在 libswresample.a，
#    所以最终可执行文件必须自己把 swresample 链上。
#    FindFFmpeg 的每个导入目标都有 `NOT TARGET` 保护，重复调用只补建缺的那个。
#
# 2) mediandk / android：libavcodec 是 --enable-mediacodec 构建的，引用 AMediaFormat_*
#    （NDK libmediandk.so，api 29+）和 ANativeWindow_*（libandroid.so）。
#    libobs 那边是在自己的 os-android.cmake 里 PRIVATE 链的，不外泄给前端。
find_package(FFmpeg REQUIRED COMPONENTS swresample)
target_link_libraries(obs-studio PRIVATE FFmpeg::swresample mediandk android)

# 3) 静态 FFmpeg 链进**共享库**（3-2 ② 才撞出来的，3-1 那轮产物是可执行文件所以没暴露）：
#    ld.lld 直接拒绝 ——
#      relocation R_X86_64_PC32 cannot be used against symbol 'ff_pb_15'; recompile with -fPIC
#    不是依赖没编 PIC：`~/deps-work/src/x86_64/ffmpeg/config.h` 里 `#define CONFIG_PIC 1`，
#    而 libavcodec.a 内含 79,579 条 `R_X86_64_PC32`（arm64 那份 0 条，aarch64 的
#    ADR_PREL_PG_HI21 族本来就没这约束 ⇒ 这条只卡 x86_64）。真原因是那些 `ff_pb_*`/`ff_pw_*`
#    常量是**全局可见、可被抢占**的数据符号：链成可执行文件时地址是静态的，PC 相对寻址合法；
#    链成 `.so` 时链接器不能允许"PC 相对 + 可能被别的 DSO 抢占"这种组合。
#    （桌面平台 FFmpeg 是动态库、这些符号压根不进最终产物 —— 这条是按"3-1 那轮链接可执行
#    文件通过、本轮改链共享库才失败"推出来的，**没有**去查上游桌面构建怎么做的。）
#
#    解法取 `--exclude-libs,ALL`（不把静态归档里的符号导出到 .dynsym ⇒ 符号变成不可抢占）：
#    同一条链接命令三选一实测（.o 全部现成，只比重链）——
#      不加               rc=1，22 条 error（证明这条探针命令本身能复现故障）
#      -Wl,-Bsymbolic     rc=0，96,071,056 B，.dynsym 里 ff_* 5,731 条
#      --exclude-libs,ALL rc=0，95,534,888 B，.dynsym 里 ff_* **0** 条
#    两者都照样导出 `main`（FUNC GLOBAL DEFAULT，Java 侧 dlsym 的前提），选后一条是因为它
#    把已定义的 GLOBAL/WEAK 导出从 11,853 个收成 3,941 个 —— 与"这是最终 APK 里的应用库、
#    不是给别人链接的库"这个身份相符。
target_link_options(obs-studio PRIVATE -Wl,--exclude-libs,ALL)

# ---------------------------------------------------------------------------
# 3-2 ②：把前端打成 APK
#
# 交付形态的变化（本轮实测出来的，不是猜的）：qt_add_executable 在 Android 上建的是
# MODULE 库，产物不再是 rundir/bin/obs 那个 ELF 可执行文件，而是 frontend/libobs_<abi>.so
# （SUFFIX 被 qt6_android_apply_arch_suffix 设成 _<abi>.so，见 Qt6AndroidMacros.cmake:466-483）。
#
# 资源取**源树**不取 rundir：rundir 里的副本要等对应 ninja target 的 copy 规则才刷新，
# 而 qt_add_resources 的 FILES 是 configure 期定死的清单 —— 新构树上 configure 时 rundir
# 还是空的。这条教训来自 .qoder/sync-obs-to-shell.sh（"只编 libobs-opengl 时同步过去的是
# 改之前的 .effect（实测踩过）"）。
# ---------------------------------------------------------------------------
set(_obs_lib_data "${CMAKE_SOURCE_DIR}/libobs/data")
set(_frontend_data "${CMAKE_CURRENT_SOURCE_DIR}/data")

file(GLOB_RECURSE _obs_lib_data_files CONFIGURE_DEPENDS "${_obs_lib_data}/*")
file(GLOB_RECURSE _frontend_data_files CONFIGURE_DEPENDS "${_frontend_data}/*")
list(LENGTH _obs_lib_data_files _obs_lib_data_count)
list(LENGTH _frontend_data_files _frontend_data_count)
if(_obs_lib_data_count EQUAL 0)
  message(FATAL_ERROR "3-2 打包：${_obs_lib_data} 下没有任何文件 —— APK 里不会有 libobs 数据")
endif()
if(_frontend_data_count EQUAL 0)
  message(FATAL_ERROR "3-2 打包：${_frontend_data} 下没有任何文件 —— APK 里不会有前端 locale/主题")
endif()
message(STATUS "3-2 打包：数据资源 libobs/data=${_obs_lib_data_count} 个文件，frontend/data=${_frontend_data_count} 个文件")

# :/obsdata -> $OBS_ROOT_PATH/share/libobs（obs-android.c 的 find_libobs_data_file）
qt_add_resources(
  obs-studio "obsdata"
  PREFIX "/obsdata"
  BASE "${_obs_lib_data}"
  FILES ${_obs_lib_data_files})

# :/obsfrontenddata -> $OBS_ROOT_PATH/share/obs/obs-studio
# （platform-android.cpp 的 GetDataFilePath 拼的是 <root>/$OBS_DATA_PATH/obs-studio/，
#  OBS_DATA_PATH = share/obs，与桌面安装树同构）
qt_add_resources(
  obs-studio "obsfrontenddata"
  PREFIX "/obsfrontenddata"
  BASE "${_frontend_data}"
  FILES ${_frontend_data_files})

# :/obsplugindata/<模块> -> $OBS_ROOT_PATH/share/obs/obs-plugins/<模块>
#
# 插件数据（.effect / locale / luma_wipes…）没法跟着 .so 一起进 nativeLibraryDir：
# PackageManager 只把 APK 的 lib/<abi>/ 里以 .so 结尾的文件解出来，其余文件连目录都建不出，
# 而 obs_module_file() 最终走 os_file_exists()（真 stat），资源路径 :/... 它不认。
# 所以与 libobs/前端数据同一套路：进 Qt 资源 → 启动时解到应用私有目录。
# 目标清单取全局属性 OBS_MODULES_ENABLED —— 顶层 CMakeLists.txt 里 add_subdirectory(plugins)
# 在 add_subdirectory(frontend) 之前，故此刻已完整；模块名就是 .so 去后缀后的名字，
# 与 libobs/obs-module.c 的 process_found_module 推出的 name 一致，data 目录按同名对上。
get_property(_obs_plugin_modules GLOBAL PROPERTY OBS_MODULES_ENABLED)
set(_obs_plugin_data_dirs "")
set(OBS_ANDROID_PLUGIN_MODULE_ENTRIES "")
foreach(_m IN LISTS _obs_plugin_modules)
  string(APPEND OBS_ANDROID_PLUGIN_MODULE_ENTRIES "  \"${_m}\",\n")
  get_target_property(_m_src ${_m} SOURCE_DIR)
  if(_m_src AND EXISTS "${_m_src}/data")
    file(GLOB_RECURSE _m_files CONFIGURE_DEPENDS "${_m_src}/data/*")
    list(LENGTH _m_files _m_count)
    if(_m_count GREATER 0)
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _m_name ${_m})
      qt_add_resources(
        obs-studio "obsplugindata_${_m_name}"
        PREFIX "/obsplugindata/${_m}"
        BASE "${_m_src}/data"
        FILES ${_m_files})
      list(APPEND _obs_plugin_data_dirs "${_m}(${_m_count})")
    endif()
  endif()
endforeach()
list(JOIN _obs_plugin_data_dirs " " _obs_plugin_data_dirs)
message(STATUS "P-1 打包：插件数据进 APK 资源 = ${_obs_plugin_data_dirs}（候选模块 ${_obs_plugin_modules}）")

# 装载清单：OBSApp::loadAppModules() 的 Android 分支按这张表逐个裸名 dlopen（表的内容见模板注释）。
# 没有它的话插件"进得了 APK、进不了 obs"—— 实测：只进白名单不装载时，转场列表为空、
# channel 0 上没有转场源，切场景仍走 3-4 的直挂兜底分支。
string(STRIP "${OBS_ANDROID_PLUGIN_MODULE_ENTRIES}" OBS_ANDROID_PLUGIN_MODULE_ENTRIES)
configure_file(cmake/templates/android-plugin-modules.h.in android-plugin-modules.h NEWLINE_STYLE LF)
list(LENGTH _obs_plugin_modules _obs_plugin_module_count)
message(STATUS "P-1 装载：android-plugin-modules.h 生成，插件数=${_obs_plugin_module_count}")

# 渲染后端不参与链接（obs_reset_video 时 os_dlopen("libobs-opengl")），所以只有列进
# EXTRA_LIBS 才会被 androiddeployqt 打进 APK 的 lib/<abi>/。文件名本来就以 lib 开头，
# 不需要插件那种改名（androiddeployqt 硬性要求 EXTRA_LIBS 的项以 "lib" 开头，实测记在
# android-shell/CMakeLists.txt:51-53）。
set(_extra_libs "")
if(TARGET OBS::libobs-opengl)
  get_target_property(_gs_dir OBS::libobs-opengl BINARY_DIR)
  set(_gs_backend "${_gs_dir}/libobs-opengl.so")
  list(APPEND _extra_libs "${_gs_backend}")
  message(STATUS "3-2 打包：渲染后端进 APK = ${_gs_backend}")
else()
  message(WARNING "3-2 打包：没有 OBS::libobs-opengl 目标 —— APK 里没有渲染后端，obs_reset_video 会失败")
endif()

# 插件 .so 不进 EXTRA_LIBS（名字不以 lib 开头，androiddeployqt 会要求改名），而是由
# .qoder/build-frontend-apk.sh 把 rundir/lib/obs-plugins/*.so 暂存到本目录的 libs/<abi>/，
# 走 androiddeployqt 的 android-package-source-directory → gradle jniLibs 那条路进 APK。
set_target_properties(
  obs-studio
  PROPERTIES
    QT_ANDROID_EXTRA_LIBS "${_extra_libs}"
    # 清单与 FileProvider 的 paths 都在 cmake/android/ 下自带；一旦设了这个变量，
    # androiddeployqt 就不再拿自己的模板覆盖清单（同 android-shell 的做法）。
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android"
    QT_ANDROID_VERSION_NAME "0.1.0"
    QT_ANDROID_VERSION_CODE 1
    QT_ANDROID_COMPILE_SDK_VERSION 35
    QT_ANDROID_TARGET_SDK_VERSION 35
    QT_ANDROID_MIN_SDK_VERSION 29)

