cmake_minimum_required(VERSION 3.21)

project(obs_android_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# libobs：由 WSL 交叉编译后用 .qoder/sync-obs-to-shell.sh 同步进来
#   obs/<abi>/{lib/liblibobs.so, include/**, share/libobs/**}
# 链接它 = 壳里能直接调 obs_startup()，同时 androiddeployqt 会把 .so 打进 APK 的 lib/<abi>/
# ---------------------------------------------------------------------------
set(OBS_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/obs/${ANDROID_ABI}")
set(OBS_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps-android/prebuilt/${ANDROID_ABI}")
if(NOT EXISTS "${OBS_PREBUILT}/lib/liblibobs.so")
  message(FATAL_ERROR
    "缺少 libobs 交叉产物：${OBS_PREBUILT}\n"
    "先在 WSL 里跑： bash /mnt/d/OBS安卓/.qoder/sync-obs-to-shell.sh ${ANDROID_ABI}")
endif()
# 渲染后端是运行时 dlopen 的（os_dlopen("libobs-opengl") → dlopen("libobs-opengl.so")），
# 不参与链接，所以必须靠下面的 QT_ANDROID_EXTRA_LIBS 才会被打进 APK
if(NOT EXISTS "${OBS_PREBUILT}/lib/libobs-opengl.so")
  message(FATAL_ERROR
    "缺少渲染后端：${OBS_PREBUILT}/lib/libobs-opengl.so\n"
    "先在 WSL 里编译 libobs-opengl，再跑 sync-obs-to-shell.sh ${ANDROID_ABI}")
endif()
# libobs 公开头 util/sse-intrin.h 会 #include <simde/x86/sse2.h>，依赖头必须一起可见
if(NOT EXISTS "${OBS_DEPS}/include/simde")
  message(FATAL_ERROR "缺少交叉依赖头文件：${OBS_DEPS}/include/simde（先跑 deps-android/build.sh ${ANDROID_ABI}）")
endif()

add_library(libobs SHARED IMPORTED GLOBAL)
set_target_properties(libobs PROPERTIES
    IMPORTED_LOCATION "${OBS_PREBUILT}/lib/liblibobs.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_PREBUILT}/include;${OBS_DEPS}/include")

file(GLOB_RECURSE OBS_DATA_FILES CONFIGURE_DEPENDS "${OBS_PREBUILT}/share/libobs/*")

# ---------------------------------------------------------------------------
# 里程碑 M3：插件（image-source 的 color source + obs-x264 + obs-encode-sink）
#   插件 .so 打两份：
#   1) Qt 资源 :/obspluginlibs —— 运行时解到 OBS_ROOT_PATH/lib，这才是 libobs 能 glob 到
#      的真实文件（实测：Qt6/AGP 默认 useLegacyPackaging=false，装机后 lib/<abi>/ 是空目录，
#      dladdr 只会给 ".../base.apk!/lib/x86_64/libxxx.so" 这种非真实路径，没法 glob 也没法软链）。
#      Android 15 是否允许 dlopen 应用私有可写目录里的 .so，是 M3 要实测的问题之一；
#      若被拒，退路是 2) + manifest 开 extractNativeLibs（legacy packaging）拿到真实只读路径。
#   2) EXTRA_LIBS —— 保留。文件名必须带 "lib" 前缀，androiddeployqt 否则直接报
#      "The file name of external library ... must begin with \"lib\""（实测）；
#      前缀不影响 libobs 找资源目录（obs-module.c:630 会去掉 "lib" 再试一次 %module%）。
#   data —— 打成 :/obsplugindata，运行时解到 OBS_ROOT_PATH/share/obs/obs-plugins/<module>。
# ---------------------------------------------------------------------------
file(GLOB OBS_PLUGIN_LIBS CONFIGURE_DEPENDS "${OBS_PREBUILT}/obs-plugins/*.so")
file(GLOB_RECURSE OBS_PLUGIN_DATA_FILES CONFIGURE_DEPENDS "${OBS_PREBUILT}/share/obs/obs-plugins/*")
list(LENGTH OBS_PLUGIN_LIBS _obs_plugin_lib_count)
if(_obs_plugin_lib_count LESS 5)
  message(FATAL_ERROR
    "插件产物不全：${OBS_PREBUILT}/obs-plugins 只有 ${_obs_plugin_lib_count} 个 .so（需要 libimage-source/libobs-x264/libobs-encode-sink/libandroid-audio/libandroid-capture 五个）\n"
    "先在 WSL 里跑： bash /mnt/d/OBS安卓/.qoder/build-plugins.sh 再跑 sync-obs-to-shell.sh ${ANDROID_ABI}")
endif()

qt_add_executable(obs-shell
    main.cpp
    gles_probe.cpp
    obs_smoke.cpp
    obs_usb_host.cpp   # 阶段 2-3(A3)：Java UsbManager 宿主 ↔ libobs USB 总线
    obs_audio_host.cpp # 阶段 2-4(A4)：Java AudioManager 输入设备宿主 ↔ libobs 音频总线
    obs_display_host.cpp # 阶段 3 前置(S1)：Java SurfaceView 宿主 ↔ gs_init_data 的 ANativeWindow
    gl_share_probe.cpp     # 阶段 3-2 前置探针：跨 EGL 上下文传纹理 + QOpenGLWidget 画在哪儿
)

# GLES 门票验证直接走 NDK 的 EGL/GLESv3，不经 Qt 的渲染封装
target_link_libraries(obs-shell PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets
    EGL GLESv3 log android
    libobs)

# libobs 的 *.effect 等数据打成 Qt 资源，运行时由 obs_smoke.cpp 解到 OBS_ROOT_PATH
qt_add_resources(obs-shell "obsdata"
    PREFIX "/obsdata"
    BASE "${OBS_PREBUILT}/share/libobs"
    FILES ${OBS_DATA_FILES})

qt_add_resources(obs-shell "obsplugindata"
    PREFIX "/obsplugindata"
    BASE "${OBS_PREBUILT}/share/obs/obs-plugins"
    FILES ${OBS_PLUGIN_DATA_FILES})

# 插件 .so 本体的第二份拷贝：libobs 只能 glob 真实目录，见上面 M3 注释
qt_add_resources(obs-shell "obspluginlibs"
    PREFIX "/obspluginlibs"
    BASE "${OBS_PREBUILT}/obs-plugins"
    FILES ${OBS_PLUGIN_LIBS})

set_target_properties(obs-shell PROPERTIES
    QT_ANDROID_EXTRA_LIBS "${OBS_PREBUILT}/lib/libobs-opengl.so;${OBS_PLUGIN_LIBS}"
    # 阶段 2 起清单自带：android/AndroidManifest.xml 声明 RECORD_AUDIO（AAudio 采集硬前提）。
    # 设了这个变量后 androiddeployqt 就不再用自己的模板覆盖清单，见该文件头部注释。
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
    QT_ANDROID_VERSION_NAME "0.1.0"
    QT_ANDROID_VERSION_CODE 1
    QT_ANDROID_COMPILE_SDK_VERSION 35
    QT_ANDROID_TARGET_SDK_VERSION 35
    QT_ANDROID_MIN_SDK_VERSION 29
)
