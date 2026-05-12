# SwiftShader Android emulator render bug — minimal repro

This repository is a minimal Android NDK reproducer for two spec violations
in **SwiftShader 4.0.0.1**, as used by the Android emulator's OpenGL ES
Translator (`-gpu swiftshader`). It builds a tiny APK that runs a single
GL operation in `onCreate` and writes a PNG of the result.

## TL;DR

The following sequence:

```c
GLuint src_tex;
glGenTextures(1, &src_tex);
glBindTexture(GL_TEXTURE_2D, src_tex);
glTexStorage2D(GL_TEXTURE_2D, 1 /* one level */, GL_RGBA8, w, h);
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                GL_RGBA, GL_UNSIGNED_BYTE, redPixels);
// NO glTexParameteri call — default min filter is GL_NEAREST_MIPMAP_LINEAR.
// Then bind src_tex, draw a textured quad into an FBO, glReadPixels.
```

samples `(0, 0, 0, 0)` everywhere on `-gpu swiftshader`. Per GLES3 §8.17
the texture is *complete* (immutable textures always are), so this should
sample the red pixels back. `-gpu swangle` (ANGLE → Vulkan →
SwiftShader-Vulkan) returns the expected red.

Two spec violations are stacked here:

1. **glTexStorage2D-created textures are treated as incomplete when the
   min filter is mipmap-aware.** Per GLES3 §8.17:
   > A texture is "complete" […] if the texture was made immutable via a
   > call to TexStorage*, the texture is considered complete.
   SwiftShader instead applies the regular mip-level-count check
   (`GL_NEAREST_MIPMAP_LINEAR` needs all levels present, only 1 is
   allocated → incomplete).
2. **Sampling an incomplete texture returns `(0, 0, 0, 0)` instead of
   `(0, 0, 0, 1)`.** GLES3 §8.17:
   > […] the value `(0, 0, 0, 1)` for floating-point texture types, or
   > `(0, 0, 0, 0xFFFFFFFF)` for integer texture types, is returned.
   SwiftShader returns alpha = 0.

Either alone would be an easy-to-work-around quirk. Stacked, they
silently drop pixel data: in real-world code that uses
`GL_ONE_MINUS_SRC_ALPHA` blending, swiftshader's `(0, 0, 0, 0)` blends
to transparent black instead of the spec-correct `(0, 0, 0, 1)` opaque
black — which is how this bug surfaced for us (everything we tried to
upload through `glTexStorage2D` became transparent black on the
emulator).

## Symptom

| Variant                                                                    | swiftshader        | swangle (ANGLE)    |
|----------------------------------------------------------------------------|--------------------|--------------------|
| `glTexStorage2D` + 1 level + default mipmap min filter (this repro)        | `(0, 0, 0, 0)` ❌  | `(255, 0, 0, 255)` ✅ |
| `glTexStorage2D` + 1 level + `glTexParameteri(..., GL_NEAREST)`            | `(255, 0, 0, 255)` ✅ | `(255, 0, 0, 255)` ✅ |
| `glTexImage2D` (mutable) + 1 level + default mipmap min filter             | `(0, 0, 0, 0)` ❌  | `(0, 0, 0, 255)` ⚠️ (spec) |
| `glTexImage2D` (mutable) + 1 level + `glTexParameteri(..., GL_NEAREST)`    | `(255, 0, 0, 255)` ✅ | `(255, 0, 0, 255)` ✅ |

Row 1 (the default repro on `main`) demonstrates **violation 1**:
swiftshader treats a `glTexStorage2D` immutable texture as incomplete
even though §8.17 says it should always be complete (and further shows
**violation 2**: alpha is 0 instead of 1).
Row 3 demonstrates **violation 2** in isolation: the texture is
genuinely incomplete in both implementations, but only swangle returns
the spec-correct `(0, 0, 0, 1)`.

To switch between rows, edit `app/src/main/cpp/repro.c`:
- Row 2 / 4: add `glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)` after the source texture is created.
- Row 3 / 4: replace `glTexStorage2D` + `glTexSubImage2D` with a single `glTexImage2D` call.

## Versions tested

| Component             | Version                                     |
|-----------------------|---------------------------------------------|
| Android emulator      | API 35, `system-images;android-35;google_apis;x86_64` |
| `GL_VENDOR`           | `Google (Google Inc.)`                      |
| `GL_RENDERER`         | `Android Emulator OpenGL ES Translator (Google SwiftShader)` |
| `GL_VERSION`          | `OpenGL ES 3.0 (OpenGL ES 3.0 SwiftShader 4.0.0.1)` |
| Android NDK           | `28.2.13676358`                             |
| Compile / target SDK  | 35                                          |
| Host                  | GitHub Actions `ubuntu-22.04` runners, KVM  |

## Reproducing locally

Two ways to run it. The instrumented test gives a hard pass/fail signal
straight from JUnit; the activity gives a PNG you can eyeball.

### As an instrumented test (canonical pass/fail)

```sh
# Launch an emulator with `-gpu swiftshader` (or `-gpu swangle`), then:
./gradlew :app:connectedDebugAndroidTest
```

On `-gpu swangle` the test passes. On `-gpu swiftshader` it fails with
something like:

```
com.example.swsrepro.ReproTest > immutableTextureSamplesAsRed FAILED
  java.lang.AssertionError:
    pixel (0, 0): expected [255, 0, 0, 255], got [0, 0, 0, 0]:
    arrays first differed at element [0]; expected:<255> but was:<0>
```

Source: [`app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt`](app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt).

### As an activity (visual PNG)

```sh
./gradlew assembleDebug
# launch an emulator with `-gpu swiftshader`, then:
APK_PATH=app/build/outputs/apk/debug/app-debug.apk ./ci/run-repro.sh
# inspect `artifacts/swsrepro-logcat.txt` and `artifacts/pngs/test.png`
```

### Or just push a branch

[`.github/workflows/repro.yml`](.github/workflows/repro.yml) runs the
build + emulator matrix automatically for both `swiftshader` and
`swangle`, producing both the PNG and the JUnit reports as CI artifacts.
On `swiftshader` the "Run instrumented test" step shows red — that's the
bug demonstration in the GitHub Actions UI.

## Source

- [`app/src/main/cpp/repro.c`](app/src/main/cpp/repro.c) — the actual
  trigger sequence; ~230 lines: EGL bring-up + the textured-quad draw +
  readback.
- [`app/src/main/cpp/jni_glue.c`](app/src/main/cpp/jni_glue.c) — single
  JNI entry point (`ReproNative.runTest`).
- [`app/src/main/kotlin/com/example/swsrepro/MainActivity.kt`](app/src/main/kotlin/com/example/swsrepro/MainActivity.kt)
  — calls `ReproNative.runTest` from `Activity.onCreate`, writes the
  result PNG to `cacheDir`.
- [`app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt`](app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt)
  — instrumented test that reads pixels `(0, 0)` and `(W/2, H/2)`
  directly from the readback buffer and asserts both equal red.
- [`.github/workflows/repro.yml`](.github/workflows/repro.yml) — runs
  the build + emulator matrix on GHA for both `swiftshader` and
  `swangle`, including the JUnit run.
