#!/bin/bash
# 构建并安装 M0 最小 Qt Android 窗口
#
#   ./build-shell.sh                 构建 arm64-v8a 并生成 APK
#   ABI=x86_64 ./build-shell.sh      构建 x86_64（MuMu 模拟器原生速度）
#   ./build-shell.sh install         构建 + 安装到模拟器并启动
set -uo pipefail

# 注意：必须走 ASCII 联接目录（AGP 拒绝非 ASCII 工程路径，真实目录为 D:\OBS安卓）
ROOT="/d/obsd"
ABI="${ABI:-arm64-v8a}"
API="${API:-29}"
ACT="${1:-build}"

SDK="/c/Users/bbpcs/AppData/Local/Android/Sdk"
QT="/d/Qt/6.9.3"
GRADLE_BIN="/c/Users/bbpcs/.gradle/wrapper/dists/gradle-8.14.5-bin/3w1tvbe412g1z3jsd16ketrw6/gradle-8.14.5/bin"
# Gradle 8.12/8.14 均不支持 JDK 25（class file major version 69）→ 用工程内自带的 Temurin 17
JDK17="$ROOT/tools/jdk17"

SDK_W=$(cygpath -w "$SDK")
SDK_F=$(cygpath -m "$SDK")
JDK_W=$(cygpath -w "$JDK17")

case "$ABI" in
  arm64-v8a) QTARCH=android_arm64_v8a ;;
  x86_64)    QTARCH=android_x86_64 ;;
  *) echo "未知 ABI: $ABI" >&2; exit 1 ;;
esac

[ -d "$QT/$QTARCH" ]        || { echo "FATAL: 缺少 Qt for Android：$QT/$QTARCH" >&2; exit 1; }
[ -d "$SDK/ndk/30.0.16138531" ] || { echo "FATAL: 缺少 Windows 版 NDK 30" >&2; exit 1; }
[ -x "$JDK17/bin/java.exe" ] || { echo "FATAL: 缺少 JDK 17：$JDK17" >&2; exit 1; }
[ -d "$GRADLE_BIN" ]        || { echo "FATAL: 未找到本地 Gradle：$GRADLE_BIN" >&2; exit 1; }

export JAVA_HOME="$JDK_W"
export ANDROID_HOME="$SDK_W"
export ANDROID_SDK_ROOT="$SDK_W"
export PATH="$ROOT/tools/pyenv/Scripts:$JDK17/bin:$QT/mingw_64/bin:$GRADLE_BIN:$PATH"

QT_W=$(cygpath -w "$QT/$QTARCH")
HOST_W=$(cygpath -w "$QT/mingw_64")
NDK_W="$SDK_W\\ndk\\30.0.16138531"
NINJA_W=$(cygpath -w "$(command -v ninja)")

BUILD_DIR="$ROOT/android-shell/build-$ABI"

echo "=== 环境 ==="
echo "cmake : $(cmake --version | head -1)"
echo "ninja : $(ninja --version)  ($NINJA_W)"
echo "gradle: $(gradle --version 2>/dev/null | awk '/^Gradle /{print $2; exit}')"
echo "Qt    : $QT_W"
echo "NDK   : $NDK_W"
echo "ABI   : $ABI   API=$API"

echo
echo "=== configure ==="
cmake -S "$ROOT/android-shell" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_W\\build\\cmake\\android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DQt6_DIR="$QT_W\\lib\\cmake\\Qt6" \
  -DQT_HOST_PATH="$HOST_W" \
  -DCMAKE_PREFIX_PATH="$QT_W" \
  -DANDROID_SDK_ROOT="$SDK_F" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_MAKE_PROGRAM="$NINJA_W" || { echo "FATAL: configure 失败"; exit 1; }

echo
echo "=== build apk ==="
cmake --build "$BUILD_DIR" --target apk -j"${NUMBER_OF_PROCESSORS:-8}" || { echo "FATAL: apk 构建失败"; exit 1; }

APK=$(find "$BUILD_DIR" -path "*build/outputs/apk/*.apk" 2>/dev/null | head -1)
[ -n "$APK" ] || APK=$(find "$BUILD_DIR" -name "*.apk" 2>/dev/null | head -1)
[ -n "$APK" ] || { echo "FATAL: 未找到生成的 APK"; exit 1; }

APKSIGNER="$SDK/build-tools/36.0.0/apksigner.bat"
[ -f "$APKSIGNER" ] || APKSIGNER="$SDK/build-tools/34.0.0/apksigner.bat"

# Release 模式下 androiddeployqt 不签名 → 用工程内 debug.keystore 自签
if ! "$APKSIGNER" verify "$APK" >/dev/null 2>&1; then
  echo
  echo "=== 签名 $(basename "$APK") ==="
  SIGNED="$(dirname "$APK")/$(basename "${APK%.apk}")-signed.apk"
  cp -f "$APK" "$SIGNED"
  "$APKSIGNER" sign \
    --ks "$(cygpath -w "$ROOT/android-shell/debug.keystore")" \
    --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
    --out "$SIGNED" "$SIGNED" || { echo "FATAL: apksigner 签名失败"; exit 1; }
  "$APKSIGNER" verify "$SIGNED" || { echo "FATAL: 签名校验未通过"; exit 1; }
  APK="$SIGNED"
fi

echo
echo "=== APK ==="
ls -la "$APK"

if [ "$ACT" = "install" ]; then
  ADB="$SDK/platform-tools/adb.exe"
  DEV="${DEV:-emulator-5554}"
  AAPT2="$SDK/build-tools/36.0.0/aapt2.exe"
  [ -f "$AAPT2" ] || AAPT2="$SDK/build-tools/34.0.0/aapt2.exe"
  PKG=$("$AAPT2" dump badging "$APK" 2>/dev/null | awk -F"'" '/^package:/{print $2; exit}')
  [ -n "$PKG" ] || PKG=org.qtproject.example.obs_shell
  echo
  echo "=== 安装 $PKG -> $DEV ==="
  "$ADB" -s "$DEV" install -r "$APK" || exit 1
  "$ADB" -s "$DEV" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 || exit 1
  echo "已启动；日志： adb -s $DEV logcat -s QtApplication org.qtproject"
fi
