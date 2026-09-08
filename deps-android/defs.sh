#!/bin/bash
# 各依赖的交叉编译定义。由 build.sh source 后按 build_<dep> 约定调用。
#
# 前提：build.sh 已导出
#   ABI API NDK TOOLCHAIN SYSROOT TRIPLE CC CXX AR AS RANLIB STRIP NM OBJCOPY OBJDUMP LD
#   PREFIX JOBS BUILD_TRIPLE src_dir()
#
# 统一策略：全部编成 **静态库**（-fPIC），OBS 侧链接进各 .so；zlib 直接用 NDK sysroot。
set -uo pipefail

_common_flags() {
  COMMON_CFLAGS="-O2 -fPIC -I${PREFIX}/include"
  COMMON_LDFLAGS="-L${PREFIX}/lib -Wl,-z,max-page-size=16384"
}

_common_env() {
  _common_flags
  export CC CXX AR AS RANLIB STRIP NM OBJCOPY OBJDUMP LD
  export CFLAGS="$COMMON_CFLAGS" CXXFLAGS="$COMMON_CFLAGS" LDFLAGS="$COMMON_LDFLAGS"
  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR=""
}

# ---------------------------------------------------------------- header-only
build_simde() {
  local s; s=$(src_dir simde)
  mkdir -p "$PREFIX/include"
  rm -rf "$PREFIX/include/simde"
  cp -r "$s/simde" "$PREFIX/include/"
  echo "       SIMDe 头文件已安装 (header-only)"
}

build_uthash() {
  local s; s=$(src_dir uthash)
  mkdir -p "$PREFIX/include"
  cp "$s/src/"*.h "$PREFIX/include/"
  echo "       uthash 头文件已安装 (header-only)"
}

# ---------------------------------------------------------------- cmake 系
_run_cmake() {
  local s=$1; shift
  _common_env
  # 交叉 pkg-config：只准在 $PREFIX 里找，避免误用宿主 /usr/lib 的库
  export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
  rm -rf "$s/build"
  cmake -S "$s" -B "$s/build" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    "$@" || return 1
  cmake --build "$s/build" -j"$JOBS" || return 1
  cmake --install "$s/build" || return 1
}

build_jansson() {
  _run_cmake "$(src_dir jansson)" \
    -DJANSON_BUILD_ZONED_TOOLS=OFF \
    -DJANSON_BUILD_RENDERER=OFF \
    -DJANSON_BUILD_DOC=OFF
}

build_libusb() {
  # libusb 1.0.28 源码树无 CMakeLists.txt（CMake 支持仅存在于 git master），走 autotools
  _autoconf_dep libusb --disable-udev --disable-examples-build
}

build_libjpeg_turbo() {
  # libuvc 的 MJPEG 解码依赖 libjpeg；turbojpeg 以 native 模式提供 libjpeg.a + jpeglib.h
  _run_cmake "$(src_dir libjpeg-turbo)" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_JAVA=OFF -DWITH_ICC_LIB=OFF \
    -DWITH_TURBOJPEG=ON
}

build_libuvc() {
  # libuvc 依赖 libusb + libjpeg（两者须先构建）
  # 0.0.7 用 CMAKE_BUILD_TARGET 选产物类型（不认 BUILD_SHARED_LIBS），
  # 且 BUILD_EXAMPLE 默认 ON 会强制 find_package(OpenCV)，必须关掉
  _run_cmake "$(src_dir libuvc)" \
    -DCMAKE_BUILD_TARGET=Static \
    -DBUILD_EXAMPLE=OFF \
    -DBUILD_TEST=OFF
}

build_curl() {
  _run_cmake "$(src_dir curl)" \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_LIBPSL=OFF \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=ON \
    -DCURL_USE_SECOBJC=OFF \
    -DCMAKE_USE_MBEDTLS=ON \
    -DMBEDTLS_INCLUDE_DIR="$PREFIX/include" \
    -DMBEDTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$PREFIX/lib/libmbedcrypto.a"
}

# ---------------------------------------------------------------- autotools / 自有 configure
# 通用：gnu 风格 configure + --host=$TRIPLE
_autoconf_dep() {
  local name=$1; shift
  local s; s=$(src_dir "$name")
  _common_env
  ( cd "$s" || exit 1
    [ -x ./configure ] || autoreconf -fi || exit 1
    ./configure --prefix="$PREFIX" --host="$TRIPLE" --build="$BUILD_TRIPLE" \
      --enable-static --disable-shared "$@" || exit 1
    make -j"$JOBS" || exit 1
    make install || exit 1
  )
}

build_rnnoise()  { _autoconf_dep rnnoise  --disable-examples; }
build_fdk_aac() {
  # NDK 30 的 sysroot 已删除 <log/log.h>，而 fdk-aac 2.0.3 在 __ANDROID__ 分支里
  # 无条件 #include "log/log.h" 并调用 AOSP 私有的 android_errorWriteLog()。
  # 该函数只用于把畸形码流事件写进内核 event log，去掉不影响编解码 → 提供 no-op 垫片。
  mkdir -p "$PREFIX/include/log"
  cat > "$PREFIX/include/log/log.h" <<'EOF'
#pragma once
#include <android/log.h>
static inline void android_errorWriteLog(int tag, const char *subTag) {
  (void) tag;
  (void) subTag;
}
EOF
  _autoconf_dep fdk-aac --disable-programs
}
build_speexdsp() { _autoconf_dep speexdsp --disable-doc; }

build_freetype() {
  _autoconf_dep freetype \
    --without-harfbuzz --without-bzip2 --without-png --without-zlib --without-brotli
}

build_x264() {
  local s; s=$(src_dir x264)
  _common_env
  # x264 按架构自选汇编器（x86→nasm，aarch64→clang）；
  # 继承来的 AS=clang 会让 x86_64 分支拿 clang 去试 nasm 语法而直接失败
  unset AS
  ( cd "$s" || exit 1
    ./configure \
      --prefix="$PREFIX" \
      --host="$TRIPLE" \
      --cross-prefix="${TOOLCHAIN}/bin/${TRIPLE}-" \
      --sysroot="$SYSROOT" \
      --enable-static --enable-pic \
      --disable-cli \
      --disable-opencl --disable-lavf --disable-avs --disable-ffms \
      --disable-gpac --disable-lsmash --disable-interlaced \
      --extra-cflags="$COMMON_CFLAGS" \
      --extra-ldflags="$COMMON_LDFLAGS" || exit 1
    make -j"$JOBS" || exit 1
    make install-lib-static || make install || exit 1
  )
}

# ---------------------------------------------------------------- ffmpeg
build_ffmpeg() {
  local s; s=$(src_dir ffmpeg)
  _common_env
  local arch extra
  case "$ABI" in
    arm64-v8a) arch=aarch64; extra="--enable-neon" ;;
    x86_64)    arch=x86_64;  extra="--disable-neon --enable-x86asm" ;;
    *) echo "未知 ABI: $ABI" >&2; return 1 ;;
  esac
  ( cd "$s" || exit 1
    ./configure \
      --prefix="$PREFIX" \
      --enable-cross-compile \
      --target-os=android \
      --arch="$arch" \
      --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" \
      --sysroot="$SYSROOT" \
      --enable-static --disable-shared \
      --disable-doc --disable-programs --disable-debug --disable-stripping \
      --disable-autodetect \
      --enable-mediacodec --enable-jni \
      --enable-avcodec --enable-avformat --enable-avutil \
      --enable-swscale --enable-swresample --enable-avfilter --enable-avdevice \
      --enable-zlib \
      --enable-asm --enable-inline-asm \
      --disable-vulkan --disable-v4l2-m2m --disable-vaapi --disable-vdpau \
      --optflags="-O2 -fPIC" \
      --extra-ldflags="$COMMON_LDFLAGS" \
      $extra || exit 1
    make -j"$JOBS" || exit 1
    make install || exit 1
  )
}

build_mbedtls() {
  # 产物 libmbedtls.a / libmbedx509.a / libmbedcrypto.a —— build_curl 依赖这三个名字
  _run_cmake "$(src_dir mbedtls)" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF
}
