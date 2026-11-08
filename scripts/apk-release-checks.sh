#!/bin/bash
# Sourced by build-frontend-apk-wsl.sh for release builds only.

release_die() {
  echo "FATAL: $*" >&2
  exit 1
}

release_certificate_sha256() {
  sed -nE 's/^[[:space:]]*SHA256: ([0-9A-Fa-f:]+)[[:space:]]*$/\1/p' |
    tr -d ':' | tr '[:upper:]' '[:lower:]'
}

release_preflight() {
  local name certificate debug_certificate
  for name in ANDROID_RELEASE_KEYSTORE ANDROID_RELEASE_KEY_ALIAS \
              ANDROID_RELEASE_STORE_PASSWORD ANDROID_RELEASE_KEY_PASSWORD; do
    [ -n "${!name:-}" ] || release_die "release requires $name"
  done
  [ -f "$ANDROID_RELEASE_KEYSTORE" ] || release_die "release keystore does not exist"
  [ -x "$APKSIGNER" ] || release_die "missing apksigner: $APKSIGNER"
  [ -x "$AAPT2" ] || release_die "missing aapt2: $AAPT2"
  [ -x "$JAVA_HOME/bin/keytool" ] || release_die "missing JDK keytool"
  export ANDROID_RELEASE_STORE_PASSWORD ANDROID_RELEASE_KEY_PASSWORD

  certificate=$("$JAVA_HOME/bin/keytool" -list -v \
    -J-Duser.language=en -J-Duser.country=US \
    -keystore "$ANDROID_RELEASE_KEYSTORE" -alias "$ANDROID_RELEASE_KEY_ALIAS" \
    -storepass:env ANDROID_RELEASE_STORE_PASSWORD) || release_die "cannot read release certificate"
  RELEASE_CERT_SHA256=$(printf '%s\n' "$certificate" | release_certificate_sha256)
  [[ "$RELEASE_CERT_SHA256" =~ ^[0-9a-f]{64}$ ]] || release_die "cannot identify release certificate"

  debug_certificate=$("$JAVA_HOME/bin/keytool" -list -v \
    -J-Duser.language=en -J-Duser.country=US \
    -keystore "$ROOT/android-shell/debug.keystore" -alias androiddebugkey \
    -storepass android) || release_die "cannot read repository debug certificate"
  DEBUG_CERT_SHA256=$(printf '%s\n' "$debug_certificate" | release_certificate_sha256)
  [[ "$DEBUG_CERT_SHA256" =~ ^[0-9a-f]{64}$ ]] || release_die "cannot identify repository debug certificate"

  [ "$RELEASE_CERT_SHA256" != "$DEBUG_CERT_SHA256" ] || release_die "repository debug certificate is forbidden for release"
  if printf '%s\n' "$certificate" | grep -iEq 'CN[[:space:]]*=[[:space:]]*Android Debug([,[:space:]]|$)'; then
    release_die "Android Debug certificate is forbidden for release"
  fi
}

release_verify_apk() {
  local apk="$1" badging verification fingerprint
  badging=$("$AAPT2" dump badging "$apk") || release_die "cannot inspect release APK manifest"
  printf '%s\n' "$badging" | grep -q '^package:' || release_die "missing APK manifest metadata"
  if printf '%s\n' "$badging" | grep -q '^application-debuggable'; then
    release_die "release APK has android:debuggable=true"
  fi

  verification=$(LC_ALL=C "$APKSIGNER" verify --verbose --print-certs "$apk") || release_die "release APK signature verification failed"
  # Exactly one signer, matching the explicitly selected release key.
  fingerprint=$(printf '%s\n' "$verification" |
    sed -nE 's/^Signer #[0-9]+ certificate SHA-256 digest: ([0-9A-Fa-f]+)[[:space:]]*$/\1/p' |
    tr '[:upper:]' '[:lower:]')
  [ "$fingerprint" = "$RELEASE_CERT_SHA256" ] || release_die "APK signer does not match the release certificate"
  [ "$fingerprint" != "$DEBUG_CERT_SHA256" ] || release_die "debug-signed release APK is forbidden"
  if printf '%s\n' "$verification" | grep -iEq 'CN[[:space:]]*=[[:space:]]*Android Debug([,[:space:]]|$)'; then
    release_die "Android Debug certificate is forbidden for release"
  fi
}

release_sign_apk() {
  local unsigned="$1" signed="$2"
  # Always sign with the selected key, including APKs already signed by Gradle.
  "$APKSIGNER" sign --ks "$ANDROID_RELEASE_KEYSTORE" \
    --ks-key-alias "$ANDROID_RELEASE_KEY_ALIAS" \
    --ks-pass env:ANDROID_RELEASE_STORE_PASSWORD \
    --key-pass env:ANDROID_RELEASE_KEY_PASSWORD \
    --out "$signed" "$unsigned" || release_die "release APK signing failed"
  release_verify_apk "$signed"
}
