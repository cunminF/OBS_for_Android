cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_FFMPEG_LOGGING "Enables obs-ffmpeg logging" OFF)

# 阶段 4 P-6：Android 档强制关掉"原生 SRT/RIST mpegts 输出"。理由是实测具体的 ——
# cmake/dependencies.cmake 在该选项为 ON 时要求 Librist + Libsrt 两个 target，缺任一个就
# FATAL_ERROR（"Disable this error by setting ENABLE_NEW_MPEGTS_OUTPUT to OFF"，它自己写的退路），
# 而 deps-android/prebuilt/<abi>/lib 里两个都没有（只有 mbedtls/curl/jansson/x264/ffmpeg/…）。
# 代价：obs-ffmpeg-mpegts.c 不进编译。`ffmpeg_mpegts_muxer` 这个符号**不会因此消失** ——
# 它由无条件编译的 obs-ffmpeg-mux.c:884 `#ifndef NEW_MPEGTS_OUTPUT` 那一支提供，
# 也就是 mpegts 回到"经 ffmpeg_muxer 那条路"（而那条路的伴生进程在 Android 起不来，见下面 ffmpeg-mux 那段）。
if(OS_ANDROID)
  set(ENABLE_NEW_MPEGTS_OUTPUT OFF)
endif()

option(ENABLE_NEW_MPEGTS_OUTPUT "Use native SRT/RIST mpegts output" ON)

if(OS_LINUX OR OS_WINDOWS)
  option(ENABLE_FFMPEG_NVENC "Enable legacy FFmpeg NVENC encoder" OFF)
endif()

include(cmake/dependencies.cmake)

add_library(obs-ffmpeg MODULE)
add_library(OBS::ffmpeg ALIAS obs-ffmpeg)

# 决策 8：Android 上不建 `obs-ffmpeg-mux` 这个**伴生可执行文件**。它本来是 `ffmpeg_muxer`
# 输出通过 os_process_start + 命名管道喂数据的子进程，Android（尤其目标设备上的 SELinux/无
# fork+exec 语义）起不来。这里只裁掉 target —— 没有任何 target 链 `OBS::ffmpeg-mux`（它是
# 独立可执行文件，不是 INTERFACE 目标，不会像 rtmp-services 那次跳过 file-updater 那样连带
# 丢传递依赖），所以插件侧零链接影响；代价是 `ffmpeg_muxer` 这个输出**注册但一启动就报错**
# （obs-ffmpeg-mux.c 本体仍无条件编译，见上面 NEW_MPEGTS_OUTPUT 那段）。⇒ 录制必须走
# obs-outputs 的原生容器（默认 `hybrid_mp4` → `mp4_output`，P-5 一节已实测那条映射成立）。
if(NOT OS_ANDROID)
  add_subdirectory(ffmpeg-mux)
endif()

target_sources(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:obs-ffmpeg-logging.c>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:obs-ffmpeg-nvenc.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-mpegts.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-rist.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-srt.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-url.h>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:obs-ffmpeg-vaapi.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.h>
    obs-ffmpeg-audio-encoders.c
    obs-ffmpeg-av1.c
    obs-ffmpeg-compat.h
    obs-ffmpeg-formats.h
    obs-ffmpeg-openh264.c
    obs-ffmpeg-hls-mux.c
    obs-ffmpeg-mux.c
    obs-ffmpeg-mux.h
    obs-ffmpeg-output.c
    obs-ffmpeg-output.h
    obs-ffmpeg-source.c
    obs-ffmpeg-video-encoders.c
    obs-ffmpeg.c
)

target_compile_options(obs-ffmpeg PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-shorten-64-to-32>)
target_compile_definitions(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:ENABLE_FFMPEG_LOGGING>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:ENABLE_FFMPEG_NVENC>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:NEW_MPEGTS_OUTPUT>
)

target_link_libraries(
  obs-ffmpeg
  PRIVATE
    OBS::libobs
    OBS::media-playback
    OBS::opts-parser
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avdevice
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::va>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::drm>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libpci::pci>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libdrm::Libdrm>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Librist::Librist>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Libsrt::Libsrt>
)

if(OS_WINDOWS)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL x64)
    target_sources(obs-ffmpeg PRIVATE texture-amf-opts.hpp texture-amf.cpp)
    target_link_libraries(obs-ffmpeg PRIVATE AMF::AMF)
  endif()

  configure_file(cmake/windows/obs-module.rc.in obs-ffmpeg.rc)
  target_sources(obs-ffmpeg PRIVATE obs-ffmpeg.rc)
endif()

set_target_properties_obs(obs-ffmpeg PROPERTIES FOLDER plugins/obs-ffmpeg PREFIX "")

if(OS_ANDROID)
  # 这份插件把静态 FFmpeg **又编了一份进自己的 .so**，所以 libobs 与前端那两处 Android 专属处理
  # 必须原样复刻（不复制就会在这里重现已经解决过的错误）：
  # ① 链接 `log android mediandk` —— libavcodec 是 `--enable-mediacodec` 构建的，引用
  #    AMediaCodec_*/AMediaFormat_*（NDK 的 libmediandk.so，minSdk 29 就是为它定的）与
  #    ANativeWindow_*（libandroid.so），NDK 日志符号在 liblog。同 libobs/cmake/os-android.cmake:31
  #    与 frontend/cmake/os-android.cmake:49 那两条。
  # ② `-Wl,--exclude-libs,ALL` —— 静态 FFmpeg 的汇编用 adrp+add 直接引 ff_* 数据表，这些符号一旦
  #    进 .dynsym 就成了可抢占符号，lld 会报
  #    `relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol`（libobs 当年就是这条，
  #    见 os-android.cmake:33-36 的注释）。
  # ③ `ZLIB::ZLIB` 与 `camera2ndk` —— build 48 实测的链接错误，两条都是 **build.ninja 的 LINK_FLAGS
  #    里带着 NDK 默认的 `-Wl,--no-undefined`** 才暴露的（"共享库允许未定义符号、dlopen 时才炸"这个
  #    想当然不成立，报错直接停在链接期）：
  #      · `inflate/inflateReset/deflate/deflateBound/uncompress` ⇒ 静态 FFmpeg 的 muxer/demuxer
  #        （matroska 的 zlib 压缩块等）要 zlib。libobs 与 obs-outputs 也都是
  #        `find_package(ZLIB REQUIRED)` + `ZLIB::ZLIB`（libobs/CMakeLists.txt:15,253；
  #        plugins/obs-outputs/CMakeLists.txt:6,83），这里沿用同一写法，实解析到 NDK sysroot 的
  #        libz.so。
  #      · `ACameraManager_* / ACaptureSession* / ACameraDevice_* / ACameraMetadata_*` ⇒
  #        deps-android 的 FFmpeg 带 Android Camera2 输入设备，引用点在 **libavdevice.a**
  #        （llvm-nm --undefined-only 逐个归档扫出来的；obs-ffmpeg 经 shared/media-playback 这个
  #        INTERFACE 目标链了 FFmpeg::avdevice，而 libobs 没链 avdevice，所以那两处从没撞上）。
  #        NDK 侧的提供者是 libcamera2ndk.so（API 24+，本文件全树唯一一处用它的地方）。
  #    两条的完整判据不是"猜"出来的：先拿 `.qoder/p6-undef-scan.sh` 把七个归档的未定义符号并集
  #    减去【归档自身 + libobs.so 导出 + 候选 NDK .so】，剩下的只有 zlib/Camera2 这两批与 4 个
  #    `__*tf3` 编译器内建（clang 驱动自带的 libclang_rt 静态库提供，实测未报错）。
  find_package(ZLIB REQUIRED)
  target_link_libraries(obs-ffmpeg PRIVATE log android mediandk ZLIB::ZLIB camera2ndk)
  target_link_options(obs-ffmpeg PRIVATE -Wl,--exclude-libs,ALL)
endif()
