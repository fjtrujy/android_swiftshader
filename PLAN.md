# SwiftShader / Android Emulator GL Repro — Plan

Goal: produce the minimal, dependency-free Android app that exhibits the SwiftShader
rendering bug seen in GoodNotes Renderer CI, and file an upstream issue against the
correct repository with evidence that maintainers can run themselves.

The known bug, restated for self-containment:

> On `emulator -gpu swiftshader` (Linux x86_64, GitHub-hosted runner), GL
> draw calls into an offscreen framebuffer produce a bitmap whose every pixel is
> `(R=255, G=255, B=255, A=0)` — i.e. opaque-white-encoded-as-transparent — instead
> of the expected drawn content. The same code on hardware GPUs (and on Linux
> SwiftShader-via-ANGLE, untested) produces the correct image.

---

## 1. Hypothesis to isolate

We don't yet know **which** of three components is wrong. They live in different
repos and report to different teams, so we must disambiguate before filing:

| Suspect | Repo | How to rule in/out |
|---|---|---|
| SwiftShader (the GL/Vulkan ICD) | `google/swiftshader` | Run identical GL code outside the emulator, against SwiftShader desktop build — if it fails there, it's SwiftShader. |
| ANGLE-on-SwiftShader | `google/angle` | `-gpu swangle` uses ANGLE → Vulkan → SwiftShader-Vulkan. `swiftshader` uses GL-SwiftShader directly. If the bug only repros under `swangle`, it's ANGLE. If it repros under both, it's SwiftShader. |
| Emulator gfxstream bridge | `aosp/external/qemu` (Google's emulator fork) | Run the same GL code on a real arm64 device or x86 chromebook with hardware GL — if it works, then run the same GL code via a non-Android SwiftShader build — if that works, the bug is in the emulator/host-guest GL bridge. |

**First experiment to do, before any other work**: confirm the bug repros in a
**minimal** NDK app — no Swift, no third-party libs, all GL in C accessed via JNI
from a single Kotlin Activity — on `-gpu swiftshader`. If it does not repro at the minimal level, the bug is
specific to a GL feature the Renderer uses (MSAA, sRGB, depth attachment, GLES3
PBO, etc.) and we need to incrementally add features until it appears. That
feature is then the title of the upstream issue.

Hypotheses to test in order, each as a separate variant in the same app:

1. **Onscreen swap-chain clear-to-color**: `eglMakeCurrent` on the native window, `glClearColor(1, 0, 0, 1); glClear; eglSwapBuffers`. Read back the surface. If red appears, onscreen GL works.
2. **Offscreen FBO clear + `glReadPixels`**: Create a `GL_RGBA8` FBO, clear to red, `glReadPixels`. If red appears, offscreen GL works.
3. **Offscreen FBO clear + `AndroidBitmap_lockPixels` + `glReadPixels`** into the mapped buffer. This is what the Renderer does. Likely failure point.
4. **Offscreen FBO with depth attachment**.
5. **Offscreen FBO with MSAA + `glBlitFramebuffer` resolve**.
6. **Offscreen FBO with sRGB internal format (`GL_SRGB8_ALPHA8`)**.

Stop at the **first variant that produces `(255, 255, 255, 0)` everywhere**. That
variant is the repro.

---

## 2. Repro app structure

Layout in `~/Projects/android_swiftshader/`:

```
.
├── app/
│   ├── src/main/
│   │   ├── AndroidManifest.xml          # single Activity, no theme
│   │   ├── cpp/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── repro.c                  # all GL code; one function per hypothesis
│   │   │   └── jni_glue.c               # JNI surface, returns pixel bytes to Kotlin
│   │   └── kotlin/com/example/swsrepro/
│   │       └── MainActivity.kt          # runs each variant on UI thread, dumps results
├── build.gradle.kts
├── settings.gradle.kts
├── gradle/libs.versions.toml
├── PLAN.md
└── .github/workflows/repro.yml          # see §3
```

Design constraints, all deliberate so a SwiftShader maintainer can run it in
one minute without asking us anything:

- **Kotlin + C only.** Kotlin Activity for the lifecycle wiring; all GL in C via JNI.
- **No JetPack / AppCompat / AndroidX** — depend only on `android.app.Activity`.
- **No Gradle plugins beyond `com.android.application` + `org.jetbrains.kotlin.android`** — no `androidx.core`, no Compose, no view-binding.
- **NDK C, not C++** — fewer link issues, no STL ABI considerations.
- **Single Activity, no UI** — `onCreate` runs each variant, writes a PNG per variant to `/sdcard/Android/data/.../files/`, then `finish()`s. CI pulls the PNGs.
- **Each variant returns its center-pixel + corner-pixel RGBA via JNI** and logcat-prints them — easy to grep, no image-diffing needed.
- **`compileSdk` set to whatever the GitHub Actions Android SDK ships by default** — avoid SDK-version flakes.

---

## 3. CI workflow

Goal: prove the bug exists in GitHub-hosted Ubuntu x86_64 with stock emulator,
without needing any Goodnotes-specific machinery.

`.github/workflows/repro.yml`:

- Trigger: `workflow_dispatch` + `push`.
- Runner: `ubuntu-22.04` (x86_64). Matches the GoodNotes failing config.
- Steps:
  1. Checkout.
  2. Setup JDK 17.
  3. Setup Android SDK (use `android-actions/setup-android@v3`).
  4. Install system image `system-images;android-34;google_apis;x86_64`.
  5. Install NDK 28.x (matching GoodNotes).
  6. `./gradlew assembleDebug` — produces the APK.
  7. Enable KVM (`/dev/kvm` access).
  8. Create AVD via `avdmanager` non-interactively.
  9. Boot emulator: `emulator -avd ... -no-window -no-audio -no-boot-anim -gpu swiftshader`.
  10. `adb wait-for-device` + `getprop sys.boot_completed` loop.
  11. `adb install` + `adb shell am start` + wait for `finish()`.
  12. `adb pull` the per-variant PNGs and the logcat dump.
  13. Run a simple Python script that asserts the variant-3 (or whichever is the repro) PNG is the uniform `(255, 255, 255, 0)` pattern, and that variant-1 is **not** (sanity check that the emulator+GL is alive at all).
  14. Upload PNGs + logcat as artifact named `swiftshader-repro-evidence`.

Optional second job, **same APK, same emulator AVD, different `-gpu` flag**:
`-gpu swangle`. Compare results. If swangle passes and
swiftshader fails → bug is in SwiftShader-GL, not ANGLE. If both
fail → bug is in SwiftShader-Vulkan (since `swangle` goes through
ANGLE → SwiftShader-Vulkan).

Optional third job, **same APK on a hardware-GPU emulator** (`-gpu host`,
which won't actually work on GHA without `/dev/dri`, but useful for the
local repro doc). Skip in CI; mention in README.

---

## 4. Evidence to capture for the bug report

A SwiftShader issue is more likely to be triaged quickly if it contains:

- **Title**: the exact GL feature that triggers it, e.g.
  `glReadPixels from RGBA8 FBO returns (255, 255, 255, 0) under -gpu swiftshader on Android emulator (x86_64)`
- **Reproducer**: link to this repo + `git rev-parse HEAD`. Mention: `git clone && ./gradlew assembleDebug && ./run-on-emulator.sh` reproduces in under 5 minutes on a fresh Ubuntu 22.04 machine.
- **Expected vs. actual** PNGs side-by-side (we upload these as workflow artifacts).
- **Environment**:
  - Android emulator version (`emulator -version`)
  - System image package + revision
  - SwiftShader build hash (the emulator embeds it — `strings $ANDROID_HOME/emulator/lib64/gles_swiftshader/libGLESv2_swiftshader.so | grep -i version` or similar)
  - Host OS (`uname -a`)
  - GLES version reported by the device (`adb shell getprop ro.opengles.version`)
- **Apitrace** (if we can capture one): the emulator supports `EMULATOR_GLES_TRACE=1` or similar — investigate during step 2 above. An apitrace stream lets maintainers replay exact GL calls against their own SwiftShader build without our app at all.
- **Bisection across emulator versions**: try the prior two emulator releases (downloadable via `sdkmanager --list_installed`); record which versions reproduce. Maintainers want to know if this is a regression.
- **Bisection across system images** (e.g. android-31, android-33, android-34, android-35): is it a guest-side bug or a host-side bug? Same system image, different host emulator versions → host bug. Same host emulator, different system images → guest bug.

A "fully loaded" bug report has: 1 repo link, 1 logcat, 2 PNGs, 6 lines of
environment data, 1 apitrace (if doable), 1 sentence saying "introduced in
emulator version X according to my bisection". That's the bar to aim for.

---

## Execution order (proposed)

1. **Phase 1 — minimal C app, manual run locally** (1–2 hours): write `repro.c` with variants 1–3; build APK; install on `Medium_Phone_API_36.1_swangle` and on a fresh x86_64 swiftshader-only AVD; capture the failure mode. Sanity check that the bug actually repros in a stripped-down environment **before** investing in CI.
2. **Phase 2 — CI workflow** (1 hour): add the workflow, get it green, get the artifact uploaded.
3. **Phase 3 — feature isolation** (open-ended): add variants 4–6 until we pinpoint the exact GL feature.
4. **Phase 4 — environment evidence** (30 min): apitrace + version bisection.
5. **Phase 5 — file the issue**.

Total work-budget before filing: 1 day, ideally less. If phase 1 doesn't repro
the bug in a minimal NDK app, the bug is likely **not in SwiftShader at all** —
it's in our Renderer's GL setup, and the upstream filing is wrong. We should
be prepared to discover this.
