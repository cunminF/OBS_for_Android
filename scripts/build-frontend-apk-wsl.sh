#!/bin/bash
# WSL 版前端 APK 构建（build-frontend-apk.sh 的 Linux 移植，arm64 专用）。
#
#   bash scripts/build-frontend-apk-wsl.sh            构建 + 签名 + 校验
#   bash scripts/build-frontend-apk-wsl.sh configure  只跑 CMake configure（快速验证 CMake 改动）
#
# 与 Windows 版的差异：
#   - 路径全部在 WSL home（~/obsd），无 AGP 非 ASCII 路径问题，无需 /d/obsd 联接
#   - Qt 宿主是 linux_gcc_64（提供 androiddeployqt），目标是 android_arm64_v8a
#   - 只保留 arm64-v8a（真机阶段），不含装机步骤——真机用 adb 手动装
# 环境来自 ~/.obs-android-env.sh（工具链部署脚本生成）。
set -uo pipefail

ACT="${1:-build}"
API="${API:-29}"
ABI=arm64-v8a
ROOT="$HOME/OBS_for_Android"

[ -f "$HOME/.obs-android-env.sh" ] && . "$HOME/.obs-android-env.sh"

SDK="${ANDROID_SDK_ROOT:?缺 ANDROID_SDK_ROOT，先 source ~/.obs-android-env.sh}"
QT="${QT_DIR:?缺 QT_DIR}"
HOST="${QT_HOST_PATH:?缺 QT_HOST_PATH}"
NDK="${NDK_ROOT:?缺 NDK_ROOT}"
GRADLE_BIN="$HOME/toolchain/gradle-8.14.5/bin"

export PATH="$HOME/.venv/bin:$GRADLE_BIN:$SDK/platform-tools:$PATH"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"

[ -d "$QT/android_arm64_v8a" ] || { echo "FATAL: 缺少 Qt for Android：$QT/android_arm64_v8a" >&2; exit 1; }
[ -d "$NDK" ] || { echo "FATAL: 缺少 NDK：$NDK" >&2; exit 1; }
[ -x "$JAVA_HOME/bin/java" ] || { echo "FATAL: 缺少 JDK 17：$JAVA_HOME" >&2; exit 1; }
[ -x "$GRADLE_BIN/gradle" ] || { echo "FATAL: 缺少 Gradle：$GRADLE_BIN" >&2; exit 1; }
command -v cmake >/dev/null || { echo "FATAL: PATH 里没有 cmake（venv 没生效？）" >&2; exit 1; }

BUILD_DIR="$ROOT/build-fe-$ABI-plugins"
LOGDIR="$ROOT/.qoder"
mkdir -p "$LOGDIR"
LOG="${LOG:-$LOGDIR/fe-$ABI-apk-build-wsl.log}"

# Windows 树留下的 CMakeCache 指向 Windows 路径，必须清掉重配
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && grep -aq 'Windows\\\|C:/\|D:/' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "=== 检测到 Windows 侧残留的 CMakeCache，清空构建目录重配 ==="
  rm -rf "$BUILD_DIR"
fi

echo "########## WSL APK 构建 $(date '+%F %T') ##########"
echo "cmake : $(cmake --version | head -1)"
echo "ninja : $(ninja --version)"
echo "gradle: $(gradle --version 2>/dev/null | awk '/^Gradle /{print $2; exit}')"
echo "Qt    : $QT/android_arm64_v8a  host=$HOST"
echo "NDK   : $NDK"
echo "ABI   : $ABI  API=$API  构建目录=$BUILD_DIR"

# ---- configure ----
cmake -S "$ROOT/obs-studio" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$ROOT/deps-android/prebuilt/$ABI;$QT/android_arm64_v8a" \
  -DQT_HOST_PATH="$HOST" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_PLUGINS=ON \
  -DENABLE_FRONTEND=ON \
  -DENABLE_SCRIPTING=OFF \
  -DENABLE_HEVC=ON \
  -DANDROID_SDK_ROOT="$SDK" \
  -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install" 2>&1 | tee "$LOGDIR/fe-$ABI-apk-configure-wsl.log" | tail -25
CFG=${PIPESTATUS[0]}
echo "=== configure exit=$CFG ==="
[ "$CFG" = 0 ] || exit 1
[ "$ACT" = "configure" ] && { echo "=== 只配置，已完成 ==="; exit 0; }

# ---- 整树构建 ----
echo "=== build 整树（日志 $LOG） ==="
cmake --build "$BUILD_DIR" -j"$(nproc)" -- -k 0 > "$LOG" 2>&1
BUILD_RC=$?
echo "=== build exit=$BUILD_RC  失败任务数=$(grep -c '^FAILED:' "$LOG") ==="
grep -E "error:|fatal error|FAILED:" "$LOG" | head -30
[ "$BUILD_RC" = 0 ] || { echo "FATAL: 整树构建失败，看 $LOG"; exit 1; }

# ---- 插件暂存进包源目录（同 Windows 版的理由：否则新插件不进 APK） ----
PKG_LIBS="$ROOT/obs-studio/frontend/cmake/android/libs/$ABI"
PLUG_SRC=$(find "$BUILD_DIR/rundir" -type d -path '*/lib/obs-plugins' 2>/dev/null | head -1)
if [ -n "$PLUG_SRC" ]; then
  mkdir -p "$PKG_LIBS"
  cp -f "$PLUG_SRC"/*.so "$PKG_LIBS"/
  echo "=== 插件暂存 → frontend/cmake/android/libs/$ABI/: $(ls "$PKG_LIBS" | tr '\n' ' ') ==="
else
  echo "WARNING: rundir 里没找到 obs-plugins 目录 —— APK 不会带 OBS 插件"
fi

# ---- 强制重打包（同 Windows 版：清 androiddeployqt 产物 + gradle native 中间件） ----
AB_DIR="$BUILD_DIR/frontend/android-build"
if [ -d "$AB_DIR" ]; then
  mkdir -p "$AB_DIR/libs/$ABI"
  [ -n "$PLUG_SRC" ] && cp -f "$PLUG_SRC"/*.so "$AB_DIR/libs/$ABI"/
  rm -f "$AB_DIR/obs-studio.apk"
  rm -rf "$AB_DIR/build/outputs/apk" \
         "$AB_DIR/build/intermediates/merged_jni_libs" \
         "$AB_DIR/build/intermediates/merged_native_libs" \
         "$AB_DIR/build/intermediates/stripped_native_libs"
  find "$AB_DIR/build/outputs" -name '*-signed*.apk' -delete 2>/dev/null
  echo "=== 强制重打包：插件已拷进 android-build/libs/$ABI ==="
fi

APP_LIB="$BUILD_DIR/frontend/libobs_${ABI}.so"
echo "=== 应用库 = $APP_LIB ==="
ls -la "$APP_LIB" 2>/dev/null || ls -la "$BUILD_DIR/frontend"/*.so 2>/dev/null

# ---- androiddeployqt 直接出包 ----
DEPLOY_QT="$HOST/bin/androiddeployqt"
FE="$BUILD_DIR/frontend"
echo "=== androiddeployqt 出包（日志 $LOGDIR/fe-$ABI-apk-wsl.log） ==="
( cd "$FE" && "$DEPLOY_QT" \
    --input "$FE/android-obs-studio-deployment-settings.json" \
    --output "$FE/android-build" \
    --apk "$FE/android-build/obs-studio.apk" \
    --depfile "$FE/android-build/obs-studio.d" \
    --builddir "$FE" --release ) > "$LOGDIR/fe-$ABI-apk-wsl.log" 2>&1
APK_RC=$?
echo "=== androiddeployqt exit=$APK_RC ==="
grep -iE "BUILD FAILED|FAILURE|CANNOT LINK|Exception|error:" "$LOGDIR/fe-$ABI-apk-wsl.log" | head -25
grep -iE "BUILD SUCCESSFUL|Android package built successfully" "$LOGDIR/fe-$ABI-apk-wsl.log" | tail -3
[ "$APK_RC" = 0 ] || { echo "FATAL: androiddeployqt 出包失败，看 $LOGDIR/fe-$ABI-apk-wsl.log"; exit 1; }

# ---- 应用库 main 可被 dlsym 校验 ----
RE="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
if [ -f "$APP_LIB" ]; then
  echo "=== 应用库 main 动态符号 ==="
  "$RE" --dyn-syms "$APP_LIB" | grep -w "main" || echo "FAIL：.dynsym 里没有 main"
fi

# ---- 签名 ----
APK=$(find "$BUILD_DIR" -path "*build/outputs/apk/release/*-unsigned.apk" 2>/dev/null | head -1)
[ -n "$APK" ] || APK=$(find "$BUILD_DIR" -path "*build/outputs/apk/*.apk" ! -name '*-signed*.apk' 2>/dev/null | head -1)
[ -n "$APK" ] || { echo "FATAL: 未找到生成的 APK"; exit 1; }

APKSIGNER="$SDK/build-tools/36.0.0/apksigner"
[ -f "$APKSIGNER" ] || APKSIGNER="$SDK/build-tools/34.0.0/apksigner"
if ! "$APKSIGNER" verify "$APK" >/dev/null 2>&1; then
  echo "=== 签名 $(basename "$APK") ==="
  SIGNED="$(dirname "$APK")/$(basename "${APK%.apk}")-signed.apk"
  cp -f "$APK" "$SIGNED"
  "$APKSIGNER" sign \
    --ks "$ROOT/android-shell/debug.keystore" \
    --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
    --out "$SIGNED" "$SIGNED" || { echo "FATAL: apksigner 签名失败"; exit 1; }
  "$APKSIGNER" verify "$SIGNED" || { echo "FATAL: 签名校验未通过"; exit 1; }
  APK="$SIGNED"
fi

echo "=== APK ==="
ls -la "$APK"
echo "=== APK 内的 native 库（$ABI）与清单 ==="
unzip -l "$APK" | grep -c "lib/$ABI/" | xargs echo "lib/$ABI 条目数="
AAPT2="$SDK/build-tools/36.0.0/aapt2"
[ -f "$AAPT2" ] || AAPT2="$SDK/build-tools/34.0.0/aapt2"
"$AAPT2" dump badging "$APK" 2>/dev/null | grep -E "^package:|application-label:|native-code"

# ---- 归档到 releases ----
REL="$ROOT/releases"
mkdir -p "$REL"
STAMP=$(date '+%m%d-%H%M')
cp -f "$APK" "$REL/OBS-Android-$ABI-wsl-$STAMP.apk"
echo "=== 已归档: $REL/OBS-Android-$ABI-wsl-$STAMP.apk ==="
md5sum "$APK" "$REL/OBS-Android-$ABI-wsl-$STAMP.apk"
echo "########## 完成 $(date '+%F %T') ##########"
