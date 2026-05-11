#!/usr/bin/env bash
# Drives the repro APK on a booted Android emulator (called from CI).
# Assumes adb is on PATH and a single emulator is attached.
set -euo pipefail

APK_PATH="${APK_PATH:-app/build/outputs/apk/debug/app-debug.apk}"
PKG="com.example.swsrepro"
OUT_DIR="artifacts"

mkdir -p "$OUT_DIR"

echo "=== device environment ==="
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.opengles.version
adb shell getprop ro.kernel.qemu.gltransport.name || true
adb shell getprop ro.hardware.gltransport || true

echo "=== install + launch ==="
# AVD cache may contain a previously-installed copy of the app signed with a
# different debug keystore — uninstall first so `install -r` doesn't fail with
# INSTALL_FAILED_UPDATE_INCOMPATIBLE.
adb uninstall "$PKG" || true
adb install -r "$APK_PATH"
adb logcat -c
adb shell am start -W -n "$PKG/.MainActivity"

echo "=== wait for app to exit ==="
for i in $(seq 1 30); do
  if ! adb shell pidof "$PKG" >/dev/null 2>&1; then
    echo "app exited after ${i}s"
    break
  fi
  sleep 1
done

echo "=== logcat (swsrepro) ==="
adb logcat -d \
  | grep -E "I swsrepro|F DEBUG|AndroidRuntime" \
  | tee "$OUT_DIR/swsrepro-logcat.txt" \
  || true

echo "=== device-side cache listing ==="
adb shell "run-as $PKG ls -la cache/ 2>&1 || echo 'cache dir not readable'"

echo "=== pull PNGs via run-as (debuggable APK) ==="
ABS_OUT="$(pwd)/$OUT_DIR/pngs"
mkdir -p "$ABS_OUT"
for v in variant2 variant3 variant4 variant5 variant6 variant7 variant8 variant9 variant10 variant11 variant12 variant13 variant14 variant15 variant16 variant17; do
  dest="$ABS_OUT/$v.png"
  # exec-out preserves binary bytes (no CRLF mangling).
  adb exec-out "run-as $PKG cat cache/$v.png" > "$dest" 2>/dev/null || true
  if [ ! -s "$dest" ]; then
    rm -f "$dest"
    echo "  $v: not produced"
  else
    echo "  $v: $(wc -c < "$dest") bytes"
  fi
done

echo "=== artifacts contents ==="
echo "cwd=$(pwd)"
ls -laR "$OUT_DIR" || true
