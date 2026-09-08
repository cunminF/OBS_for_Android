cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PLUGINS "Enable building OBS plugins" ON)

if(NOT ENABLE_PLUGINS)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")
  return()
endif()

set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")

if(OS_ANDROID)
  # 移植期白名单：下面那批 add_obs_plugin() 大多没写 PLATFORMS 门禁，一旦
  # ENABLE_PLUGINS=ON 就会被无条件加进来（obs-ffmpeg / text-freetype2 / rtmp-services /
  # obs-outputs … 都还没移植），所以在这里提前 return，只放行当前阶段需要的少数几个：
  #   image-source     —— 阶段 1 第 5 项要的 color source（libobs 内部没有颜色源）
  #   obs-x264         —— 阶段 1 里程碑要的"x264 编码一帧"
  #   obs-encode-sink  —— 只有 obs_output 才会去 obs_encoder_start()（该符号不在
  #                       libobs 的 .dynsym 里），所以编码验证需要一个视频专用 sink
  #   android-audio    —— 阶段 2 第 1 项：AAudio 采集源（内置麦克风/有线耳机）
  #   android-capture  —— 阶段 2 第 2 项：libuvc/libusb 的 UVC 摄像头采集源
  #   android-camera   —— P-17：内置摄像头采集源（Camera2 NDK，纯 native 取流，不需要 Java 侧
  #                       相机代码；只有"CAMERA 权限问得到吗"这一格走 libobs 的相机总线，
  #                       注册方是前端 OBSAndroidPermissions.cpp）。依赖实测（b93 arm64，尺子见
  #                       .qoder/p17b-verify.sh）：只链 OBS::libobs + 裸名 camera2ndk，包内
  #                       android-camera.so 的 DT_NEEDED 恰为 liblibobs/libcamera2ndk/libm/libdl/libc
  #                       五项 —— camera2ndk 是 NDK 桩库，运行时由 linker 从系统分区取真身，
  #                       所以 APK 里不多一枚 .so，打包账不变。28 条 UNDEF 里 OBS 域**未解析 0 条**
  #                       （"等 dlopen 才炸"那类坑静态排掉），Camera2 侧 7 条 ACameraManager_*/
  #                       ACameraMetadata_* 由桩库提供；装载清单 android-plugin-modules.h 已含 "android-camera"。
  #   android-screen   —— P-18：屏幕采集源（MediaProjection）。依赖面与相机同一档：只链 OBS::libobs
  #                       + 裸名 mediandk（AImageReader_* / AImage_* 都在 libmediandk.so 那枚桩库里，
  #                       运行时由 linker 从系统分区取真身，APK 里不多一枚 .so）。
  #                       **不链 android**：这一路确实要用 ANativeWindow_toSurface，但那一步在
  #                       前端 OBSAndroidPermissions.cpp（前端本来就 link libandroid）—— 插件只把
  #                       AImageReader_getWindow 拿到的指针当不透明 void* 递过总线。
  #                       装载清单不需要在本文件之外再改一处：SAFE_MODULES 是 cmake 全局属性
  #                       OBS_MODULES_ENABLED 拼出来的（frontend/CMakeLists.txt:125-127），
  #                       而 obs_add_safe_module / obs_add_core_module 读的就是它
  #                       （frontend/widgets/OBSBasic.cpp:225-259）⇒ 下面这一行同时进构建、
  #                       进白名单、进核心装载清单，三处一本账，不会漏一处。
  #   obs-transitions  —— 阶段 4 P-1：InitDefaultTransitions 需要的转场源（含 fade/slide/
  #                       swipe/luma_wipe/stinger）。它只链 OBS::libobs，但 .so 之外的
  #                       data/*.effect 必须随 APK 走"Qt 资源 → 私有目录"那条路才找得到
  #                       （obs_module_file 拼的是 module->data_path，PackageManager 不会
  #                       把非 .so 文件解到 nativeLibraryDir），配套改动见
  #                       frontend/cmake/os-android.cmake 的 obsplugindata 与
  #                       OBSBasic.cpp 的 AddExtraModulePaths
  #   obs-filters      —— 阶段 4 P-1：视频/音频滤镜集（色彩校正、缩放、裁剪、增益、压缩器…），
  #                       源右键菜单里的"滤镜"只有装载它之后才有内容。依赖面实测很窄：只链
  #                       OBS::libobs，外加 cmake/speexdsp.cmake 的 find_package(Libspeexdsp REQUIRED)
  #                       与 cmake/rnnoise.cmake 的 Librnnoise —— 两个 .a 在
  #                       deps-android/prebuilt/<abi>/lib 里都已存在（libspeexdsp.a / librnnoise.a），
  #                       所以不需要额外裁剪。注意它同时也是 P-6 的前置：画面要过滤镜再进编码器。
  #   text-freetype2   —— 阶段 4 P-1：Freetype2 文本源。CMake 侧零裁剪（find_package(Freetype)
  #                       由 deps-android 的 include/freetype2 + libfreetype.a 满足；Fontconfig
  #                       只在 Linux/FreeBSD/OpenBSD 分支才 find_package，Android 不碰，正是决策 10
  #                       "不移植 fontconfig"的落点）。真正的坑在源文件：target_sources 里四个
  #                       find-font* 的 PLATFORM_ID 门禁没有 Android 一档，直接放开会得到一个
  #                       get_font_path/load_os_font_list/free_os_font_list 全未定义的 .so，
  #                       dlopen 时才炸 —— 所以新增 find-font-android.c（扫 /system/fonts 等三个目录
  #                       + 内置兜底家族，匹配算法沿用 find-font.c）。实测 libfreetype.a 除 bionic
  #                       libc 外无外部符号（自带 gzip、不依赖 png/bz2/brotli）。
  # 后续阶段按需要在下面逐条放开，别一次全开。
  add_subdirectory(image-source)
  add_subdirectory(obs-x264)
  add_subdirectory(obs-encode-sink)
  add_subdirectory(android-audio)
  add_subdirectory(android-capture)
  add_subdirectory(android-camera)
  add_subdirectory(android-screen)
  add_subdirectory(obs-transitions)
  add_subdirectory(obs-filters)
  add_subdirectory(text-freetype2)
  #   rtmp-services    —— 阶段 4 P-2：注册 rtmp_common / rtmp_custom 两个服务源类型。
  #                       设置对话框的"流"页要 obs_get_source_by_id("rtmp_common") 才建得起来，
  #                       没有它 OBSBasic::GetService() 返回 NULL（P-3 的闪退地雷就埋在那）。
  #                       CMake 侧三处裁剪：①OS_ANDROID 强制 ENABLE_SERVICE_UPDATES=OFF；
  #                       ②不 add_subdirectory(shared/file-updater)（那是个 INTERFACE 目标，
  #                       要把 file-updater.c 整份编进消费者并链 CURL::libcurl —— 不编它（联网更新器
  #                       本体），curl 本体 deps-android 里有、缺的只是随包 CA 信任链）；③改用 plugins/rtmp-services/file-updater-android-stub.c
  #                       顶住 update_info_create/create_single/destroy —— 其中 destroy 在
  #                       service-specific/{service-ingest,dacast}.c 与 obs_module_unload() 里是
  #                       无条件引用的（不受 ENABLE_SERVICE_UPDATES 门禁），"不链更新器"会直接
  #                       变成链接错误，替身一律返回 NULL 表示"没有本地列表可读"。
  #                       第二个坑：跳过一个 INTERFACE 目标会连带丢掉它的**传递依赖** —— libcurl 原本
  #                       正是经手 file-updater 的 INTERFACE_LINK_LIBRARIES 流进 rtmp-services 的，
  #                       而 service-specific/{nimotv,showroom}.c 里的 curl_easy_* 是无条件编译的，
  #                       实测报 8 个 `undefined symbol: curl_easy_*` ⇒ Android 档自己
  #                       find_package(CURL REQUIRED) 补链 CURL::libcurl（deps-android 里有现成的
  #                       libcurl.a / mbedtls 后端；curl_obs_set_revoke_setting 不在此列，它是宏）。
  #                       实测纠正：services.json **是**随包的（data/services.json，127,825 字节，
  #                       format_version=5，83 个服务；设备上 32-PKG 列出 package.json + services.json），
  #                       所以服务下拉有内容 —— 连带让 rtmp_common_url() 里 Twitch/IVS "auto" 和 Dacast
  #                       那三支可达，撞上替身造成的空等。插件源码里另有三处 __ANDROID__ 裁剪
  #                       （load_*_data 挪出 ENABLE_SERVICE_UPDATES 门禁、service_ingests_refresh 与
  #                       dacast_ingests_load_data 跳过 busy-wait），已进 P-3 回退清单。
  add_subdirectory(rtmp-services)
  #   obs-outputs      —— 阶段 4 P-5：核心输出集。`obs_module_load()` 注册 5 个输出
  #                       rtmp_output / null_output / flv_output / mp4_output / mov_output
  #                       （`plugins/obs-outputs/obs-outputs.c:58-62`）。它是两件事的前置：
  #                       ① rtmp-services 的服务下拉要"协议可用"才放行条目，RTMP 协议的注册者
  #                          就是这里（P-2 实测 83 条 JSON 全被闸门滤空、闸门放行=0）；
  #                       ② 录制走原生容器 muxer（mp4/flv/mov 全在插件内），**不碰** obs-ffmpeg-mux
  #                          子进程那条路（决策 8：Android 上 fork+exec 起不了伴生进程）。
  #                       依赖面实测：MbedTLS 3 + ZLIB + jansson 三个 find_package 都能由
  #                       deps-android/prebuilt/<abi> 满足（MbedTLS::mbedtls 目标在
  #                       lib/cmake/MbedTLS 里导出，连带 mbedcrypto/mbedx509/everest/p256m），
  #                       外加三个共享 OBJECT 目标 happy-eyeballs / opts-parser / bpm（无平台门禁）。
  #                       Windows 专属的那份 `rtmp-windows.c` 整文件包在 `#ifdef _WIN32` 里，
  #                       无条件加进 target_sources 也编成空 TU，不用裁。
  add_subdirectory(obs-outputs)
  #   obs-ffmpeg       —— 阶段 4 P-6：FFmpeg 系的源/输出/编码器。进白名单的真正理由是**音频**：
  #                       `aac_encoder_info`（id = ffmpeg_aac）在 obs_module_load() 里无条件注册
  #                       （plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c:466）。libobs 自己不带
  #                       任何音频编码器实现 ⇒ 没有这个插件就选不到 aac，
  #                       SimpleOutput.cpp:206 会先 throw，
  #                       录制/推流一个输出对象都建不出来；设置对话框那支空了的"音频编码器"下拉
  #                       （OBSBasicSettings.cpp:5004）导致"确定"永远弹 CodecMissingOnExit（P-4 实测）
  #                       也是同一件事。顺带带来 ffmpeg_source（媒体文件源）与
  #                       ffmpeg_output/ffmpeg_muxer/ffmpeg_mpegts_muxer/ffmpeg_hls_muxer/replay_buffer。
  #                       CMake 侧三处裁剪（见 plugins/obs-ffmpeg/CMakeLists.txt）：
  #                       ①OS_ANDROID 强制 ENABLE_NEW_MPEGTS_OUTPUT=OFF —— 它的
  #                          cmake/dependencies.cmake 无条件 find_package(Librist/Libsrt REQUIRED)
  #                          而 deps-android 没有；关掉后 obs-ffmpeg-mpegts.c 不编，
  #                          ffmpeg_mpegts_muxer 改由 obs-ffmpeg-mux.c:884 的
  #                          #ifndef NEW_MPEGTS_OUTPUT 那份提供（同一个 id 的另一条实现）。
  #                       ②不 add_subdirectory(ffmpeg-mux)（决策 8：Android 上 fork+exec 起不了
  #                          伴生可执行文件）。没有任何 target 链 OBS::ffmpeg-mux，所以跳过这个
  #                          INTERFACE 目标不会重演 rtmp-services 丢传递依赖的坑。
  #                       ③补链 log/android/mediandk 再加 -Wl,--exclude-libs,ALL：FFmpeg 在
  #                          deps-android 里是**静态** .a（libobs 靠 libobs/cmake/os-android.cmake
  #                          的同一对措施），其 x86/arm 汇编用 adrp+add 引用内部数据表，
  #                          不 exclude 会在链接期炸 R_AARCH64_ADR_PREL_PG_HI21 重定位。
  #                       源码侧一处：obs-ffmpeg.c 的 VAAPI 门禁补 !defined(__ANDROID__)。
  #                       已知代价：ffmpeg_muxer 照样注册，但它一 start 就要拉起 obs-ffmpeg-mux
  #                       子进程 ⇒ 在 Android 上必失败，所以录制必须走 obs-outputs 的原生容器
  #                       （P-5 已闭环），设置里那 7 个只有 ffmpeg muxer 才支持的封装格式
  #                       （mkv/flv/mov/mpegts/hls…）在 Android 上仍不可选 —— 归 P-6 前端清单。
  add_subdirectory(obs-ffmpeg)
  return()
endif()

macro(check_obs_browser)
  if((OS_WINDOWS AND CMAKE_VS_PLATFORM_NAME MATCHES "(ARM64|x64)") OR OS_MACOS OR OS_LINUX)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/obs-browser/CMakeLists.txt")
      message(FATAL_ERROR "Required submodule 'obs-browser' not available.")
    else()
      add_subdirectory(obs-browser)
    endif()
  else()
    add_custom_target(obs-browser)
    target_disable(obs-browser)
  endif()
endmacro()

macro(check_obs_websocket)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/obs-websocket/CMakeLists.txt")
    message(FATAL_ERROR "Required submodule 'obs-websocket' not available.")
  else()
    add_subdirectory(obs-websocket)
  endif()
endmacro()

# Add plugins in alphabetical order to retain order in IDE projects
add_obs_plugin(
    aja
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(aja-output-ui PLATFORMS WINDOWS MACOS LINUX WITH_MESSAGE)
add_obs_plugin(coreaudio-encoder PLATFORMS WINDOWS MACOS)
add_obs_plugin(
    decklink
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    decklink-captions
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    decklink-output-ui
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    frontend-tools
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(image-source)
add_obs_plugin(linux-alsa PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-capture PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-jack PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pipewire PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pulseaudio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-v4l2 PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(mac-avcapture PLATFORMS MACOS)
add_obs_plugin(mac-capture PLATFORMS MACOS)
add_obs_plugin(mac-syphon PLATFORMS MACOS)
add_obs_plugin(mac-videotoolbox PLATFORMS MACOS)
add_obs_plugin(mac-virtualcam PLATFORMS MACOS)
add_obs_plugin(nv-filters PLATFORMS WINDOWS)

check_obs_browser()

add_obs_plugin(obs-ffmpeg)
add_obs_plugin(obs-filters)
add_obs_plugin(obs-libfdk)
add_obs_plugin(obs-nvenc PLATFORMS WINDOWS LINUX)
add_obs_plugin(obs-outputs)
add_obs_plugin(
    obs-qsv11
    PLATFORMS WINDOWS LINUX
    ARCHITECTURES x64 x86_64
)
add_obs_plugin(obs-text PLATFORMS WINDOWS)
add_obs_plugin(obs-transitions)
add_obs_plugin(
    obs-vst
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(obs-webrtc)

check_obs_websocket()

add_obs_plugin(obs-x264)
add_obs_plugin(oss-audio PLATFORMS FREEBSD OPENBSD)
add_obs_plugin(rtmp-services)
add_obs_plugin(sndio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(text-freetype2)
add_obs_plugin(vlc-video WITH_MESSAGE)
add_obs_plugin(win-capture PLATFORMS WINDOWS)
add_obs_plugin(win-dshow PLATFORMS WINDOWS)
add_obs_plugin(win-wasapi PLATFORMS WINDOWS)
