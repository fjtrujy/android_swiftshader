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

echo "=== device-side file listing ==="
adb shell "ls -la /storage/emulated/0/Android/data/$PKG/files/ 2>&1 || echo 'no files dir'"

echo "=== pull PNGs ==="
ABS_OUT="$(pwd)/$OUT_DIR/pngs"
mkdir -p "$ABS_OUT"
adb pull "/storage/emulated/0/Android/data/$PKG/files/." "$ABS_OUT" || echo "no PNGs to pull"

echo "=== artifacts contents ==="
echo "cwd=$(pwd)"
ls -laR "$OUT_DIR" || true
