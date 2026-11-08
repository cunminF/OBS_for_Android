#!/bin/bash
# Integration tests using real Android SDK tools; no Qt/NDK build is required.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SDK="${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT}"
TOOLS="$SDK/build-tools/${ANDROID_BUILD_TOOLS_VERSION:-36.0.0}"
APKSIGNER="${APKSIGNER:-$TOOLS/apksigner}"
AAPT2="${AAPT2:-$TOOLS/aapt2}"
ANDROID_JAR="${ANDROID_JAR:-$SDK/platforms/android-35/android.jar}"
: "${JAVA_HOME:?set JAVA_HOME to JDK 17}"
. "$ROOT/scripts/apk-release-checks.sh"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
PASSED=0

expect_failure() {
  local message="$1"
  shift
  if ( "$@" ) >"$TEST_DIR/failure.log" 2>&1; then
    echo "FAIL: expected failure: $message" >&2
    exit 1
  fi
  if ! grep -Fq "$message" "$TEST_DIR/failure.log"; then
    cat "$TEST_DIR/failure.log" >&2
    echo "FAIL: missing diagnostic: $message" >&2
    exit 1
  fi
  PASSED=$((PASSED + 1))
}

export ANDROID_RELEASE_STORE_PASSWORD='test password :$& value'
export ANDROID_RELEASE_KEY_PASSWORD="$ANDROID_RELEASE_STORE_PASSWORD"
export ANDROID_RELEASE_KEY_ALIAS=release
export ANDROID_RELEASE_KEYSTORE="$TEST_DIR/release key.p12"

generate_key() {
  "$JAVA_HOME/bin/keytool" -genkeypair -noprompt -storetype PKCS12 \
    -keystore "$1" -alias "$ANDROID_RELEASE_KEY_ALIAS" -dname "$2" \
    -keyalg RSA -keysize 2048 -validity 2 \
    -storepass:env ANDROID_RELEASE_STORE_PASSWORD \
    -keypass:env ANDROID_RELEASE_KEY_PASSWORD >/dev/null 2>&1
}
generate_key "$ANDROID_RELEASE_KEYSTORE" 'CN=Release Gate Test'
generate_key "$TEST_DIR/other.p12" 'CN=Other Release Key'
generate_key "$TEST_DIR/local-debug.p12" 'CN=Android Debug,O=Android,C=US'

make_apk() {
  local name="$1" debug_attribute="$2"
  cat >"$TEST_DIR/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.obsproject.releasecheck" android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35" />
  <application android:label="Release gate test" $debug_attribute />
</manifest>
EOF
  "$AAPT2" link -I "$ANDROID_JAR" --manifest "$TEST_DIR/AndroidManifest.xml" -o "$TEST_DIR/$name.apk"
}
make_apk safe 'android:debuggable="false"'
make_apk debuggable 'android:debuggable="true"'
make_apk default ''

for name in ANDROID_RELEASE_KEYSTORE ANDROID_RELEASE_KEY_ALIAS \
            ANDROID_RELEASE_STORE_PASSWORD ANDROID_RELEASE_KEY_PASSWORD; do
  ( unset "$name"; expect_failure "release requires $name" release_preflight )
  PASSED=$((PASSED + 1))
done
( ANDROID_RELEASE_KEYSTORE="$TEST_DIR/missing"; expect_failure 'keystore does not exist' release_preflight )
( ANDROID_RELEASE_STORE_PASSWORD=incorrect; expect_failure 'cannot read release certificate' release_preflight )
( APKSIGNER="$TEST_DIR/missing"; expect_failure 'missing apksigner' release_preflight )
( AAPT2="$TEST_DIR/missing"; expect_failure 'missing aapt2' release_preflight )
PASSED=$((PASSED + 4))

# A copy with a different filename AND alias must still be rejected by fingerprint.
cp "$ROOT/android-shell/debug.keystore" "$TEST_DIR/renamed-debug.keystore"
"$JAVA_HOME/bin/keytool" -changealias -keystore "$TEST_DIR/renamed-debug.keystore" \
  -alias androiddebugkey -destalias release -storepass android >/dev/null 2>&1
( ANDROID_RELEASE_KEYSTORE="$TEST_DIR/renamed-debug.keystore"
  ANDROID_RELEASE_STORE_PASSWORD=android
  ANDROID_RELEASE_KEY_PASSWORD=android
  expect_failure 'repository debug certificate is forbidden' release_preflight )
( ANDROID_RELEASE_KEYSTORE="$TEST_DIR/local-debug.p12"
  expect_failure 'Android Debug certificate is forbidden' release_preflight )
PASSED=$((PASSED + 2))

release_preflight
release_sign_apk "$TEST_DIR/safe.apk" "$TEST_DIR/safe-signed.apk"
release_sign_apk "$TEST_DIR/default.apk" "$TEST_DIR/default-signed.apk"
PASSED=$((PASSED + 2))
expect_failure 'android:debuggable=true' release_sign_apk "$TEST_DIR/debuggable.apk" "$TEST_DIR/debuggable-signed.apk"
expect_failure 'signature verification failed' release_verify_apk "$TEST_DIR/safe.apk"
( ANDROID_RELEASE_KEY_PASSWORD=incorrect
  expect_failure 'release APK signing failed' release_sign_apk "$TEST_DIR/safe.apk" "$TEST_DIR/wrong-password.apk" )
PASSED=$((PASSED + 1))

# A valid APK signed with the wrong release key is still unsafe to publish.
"$APKSIGNER" sign --ks "$TEST_DIR/other.p12" --ks-key-alias release \
  --ks-pass env:ANDROID_RELEASE_STORE_PASSWORD --key-pass env:ANDROID_RELEASE_KEY_PASSWORD \
  --out "$TEST_DIR/other-signed.apk" "$TEST_DIR/safe.apk"
expect_failure 'APK signer does not match' release_verify_apk "$TEST_DIR/other-signed.apk"

# Re-signing a previously signed APK must replace its signer with the selected key.
release_sign_apk "$TEST_DIR/other-signed.apk" "$TEST_DIR/resigned.apk"
PASSED=$((PASSED + 1))

# Corrupt the signed payload without damaging the manifest, to exercise verify.
mkdir -p "$TEST_DIR/assets"
printf 'original payload\n' >"$TEST_DIR/assets/payload.txt"
"$AAPT2" link -I "$ANDROID_JAR" --manifest "$TEST_DIR/AndroidManifest.xml" \
  -A "$TEST_DIR/assets" -o "$TEST_DIR/payload.apk"
release_sign_apk "$TEST_DIR/payload.apk" "$TEST_DIR/payload-signed.apk"
python3 - "$TEST_DIR/payload-signed.apk" <<'PY'
import sys
import zipfile

path = sys.argv[1]
with zipfile.ZipFile(path) as apk:
    entry = apk.getinfo('assets/payload.txt')
with open(path, 'r+b') as apk:
    apk.seek(entry.header_offset + 26)
    name_length = int.from_bytes(apk.read(2), 'little')
    extra_length = int.from_bytes(apk.read(2), 'little')
    apk.seek(entry.header_offset + 30 + name_length + extra_length)
    byte = apk.read(1)
    apk.seek(-1, 1)
    apk.write(bytes([byte[0] ^ 1]))
PY
expect_failure 'signature verification failed' release_verify_apk "$TEST_DIR/payload-signed.apk"

( AAPT2=false; expect_failure 'cannot inspect release APK manifest' release_verify_apk "$TEST_DIR/safe-signed.apk" )
PASSED=$((PASSED + 1))
echo "PASS: $PASSED release signing and APK safety checks"
