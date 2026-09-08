# deps-android 依赖清单（版本 + 下载源）——唯一真源
#
# 网络策略（2026-09 实测）：
#   github.com        : WSL 内不可达，Windows 侧必须走代理 127.0.0.1:7892
#   code.videolan.org : 直连可达
#   ffmpeg.org        : 直连可达
#   downloads.sourceforge.net : 直连可达
# 因此 fetch.sh 会自动对 github 域名加代理。
#
# zlib 不在此列：直接使用 NDK sysroot 自带的 libz.a / zlib.h。

# 依赖分组（按移植阶段）：
#   core    -> 阶段1 libobs 必需
#   usb     -> 阶段2 USB 采集必需
#   filters -> 阶段3 滤镜/文字源必需
#   output  -> 阶段4 推流/录制必需
DEP_BATCHES="core usb filters output"

dep_version() {
  case "$1" in
    simde)    echo 0.8.2 ;;
    uthash)   echo 2.3.0 ;;
    jansson)  echo 2.14 ;;
    x264)     echo master ;;
    ffmpeg)   echo 7.1.3 ;;
    libusb)   echo 1.0.28 ;;
    libjpeg-turbo) echo 3.1.0 ;;
    libuvc)   echo 0.0.7 ;;
    speexdsp) echo 1.2.1 ;;
    rnnoise)  echo master ;;
    freetype) echo 2.13.3 ;;
    mbedtls)  echo 3.6.4 ;;
    curl)     echo 8.15.0 ;;
    fdk-aac)  echo 2.0.3 ;;
    *)        echo "" ;;
  esac
}

dep_url() {
  local v
  v=$(dep_version "$1")
  case "$1" in
    simde)    echo "https://github.com/simd-everywhere/simde/archive/refs/tags/v${v}.tar.gz" ;;
    uthash)   echo "https://github.com/troydhanson/uthash/archive/refs/tags/v${v}.tar.gz" ;;
    jansson)  echo "https://github.com/akheron/jansson/archive/refs/tags/v${v}.tar.gz" ;;
    x264)     echo "https://code.videolan.org/videolan/x264/-/archive/${v}/x264-${v}.tar.bz2" ;;
    ffmpeg)   echo "https://ffmpeg.org/releases/ffmpeg-${v}.tar.xz" ;;
    libusb)   echo "https://github.com/libusb/libusb/archive/refs/tags/v${v}.tar.gz" ;;
    libjpeg-turbo) echo "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/${v}.tar.gz" ;;
    libuvc)   echo "https://github.com/libuvc/libuvc/archive/refs/tags/v${v}.tar.gz" ;;
    speexdsp) echo "https://github.com/xiph/speexdsp/archive/refs/tags/SpeexDSP-${v}.tar.gz" ;;
    rnnoise)  echo "https://github.com/xiph/rnnoise/archive/refs/heads/${v}.tar.gz" ;;
    freetype) echo "https://downloads.sourceforge.net/project/freetype/freetype2/${v}/freetype-${v}.tar.gz" ;;
    mbedtls)  echo "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${v}/mbedtls-${v}.tar.bz2" ;;
    curl)     echo "https://github.com/curl/curl/archive/refs/tags/curl-$(echo "$v" | tr '.' '_').tar.gz" ;;
    fdk-aac)  echo "https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v${v}.tar.gz" ;;
    *)        echo "" ;;
  esac
}

# 每个依赖属于哪个批次
dep_batch() {
  case "$1" in
    simde|uthash|jansson|x264|ffmpeg) echo core ;;
    libusb|libuvc|libjpeg-turbo)     echo usb ;;
    speexdsp|rnnoise|freetype)       echo filters ;;
    mbedtls|curl|fdk-aac)            echo output ;;
    *)                               echo unknown ;;
  esac
}

# 批次展开为依赖列表（顺序即编译顺序，含依赖关系）
batch_deps() {
  case "$1" in
    core)    echo "simde uthash jansson x264 ffmpeg" ;;
    usb)     echo "libusb libjpeg-turbo libuvc" ;;
    filters) echo "speexdsp rnnoise freetype" ;;
    output)  echo "mbedtls curl fdk-aac" ;;
    *)       echo "" ;;
  esac
}

dep_names() {
  local b
  for b in $DEP_BATCHES; do batch_deps "$b"; done | tr ' ' '\n' | grep -v '^$'
}
