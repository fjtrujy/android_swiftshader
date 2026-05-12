# SwiftShader Android emulator render bug — minimal repro

Minimal Android NDK reproducer for two ES 3.0 spec violations in
**SwiftShader 4.0.0.1**, as used by the Android emulator's OpenGL ES
Translator (`-gpu swiftshader`).

A `glTexStorage2D`-allocated, single-level RGBA8 texture filled with
opaque red samples as `(0, 0, 0, 0)` on `-gpu swiftshader` when the
default min filter (`GL_NEAREST_MIPMAP_LINEAR`) is in effect. The same
program on `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan) samples
red, which is what the GLES 3 spec requires.

## The trigger

```c
GLuint tex;
glGenTextures(1, &tex);
glBindTexture(GL_TEXTURE_2D, tex);
glTexStorage2D(GL_TEXTURE_2D, 1 /* one level */, GL_RGBA8, w, h);
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                GL_RGBA, GL_UNSIGNED_BYTE, redPixels);
// No glTexParameteri: min filter stays at the default
// GL_NEAREST_MIPMAP_LINEAR. Then sample `tex` from a fragment shader
// and glReadPixels — every pixel comes back (0, 0, 0, 0) on swiftshader.
```

Full code: [`app/src/main/cpp/repro.c`](app/src/main/cpp/repro.c)
(~230 lines, single main thread, no PBO, no shared context, no blending).

## What the spec says

Both violations are in **GLES 3.0 §8.17, "Texture Completeness"**.

### Violation 1 — immutable textures are unconditionally complete

> A texture is mipmap complete if [...] *or* the texture was made
> immutable via a call to **TexStorage*** [...] in which case the
> texture is unconditionally complete.

`glTexStorage2D` makes the texture immutable. The spec is explicit:
once a texture is immutable, it's complete regardless of how many
levels were allocated vs. how many the min filter would consume.

SwiftShader instead runs the standard mip-count check
(`GL_NEAREST_MIPMAP_LINEAR` requires levels `0..floor(log2(max(w,h)))`,
but only level 0 was allocated → flagged incomplete) and skips the
"unconditionally complete" short-circuit for immutable textures.

### Violation 2 — incomplete-texture sample value

> If the texture is not complete, the texel value returned [...] is
> `(0.0, 0.0, 0.0, 1.0)` for non-shadow [...] floating-point texture
> types [...].

When the source texture is genuinely incomplete (e.g. mutable +
single-level + mipmap-aware filter), the sample must return
`(0, 0, 0, 1)` — opaque black. Swangle does this. SwiftShader returns
`(0, 0, 0, 0)` — transparent black.

This compounds with violation 1: the texture in this repro is *not*
actually incomplete per spec, but SwiftShader thinks it is, and the
incomplete-sample value it returns is also wrong. So a pipeline that
blends against a `(0, 0, 0, 0)` destination ends up dropping the pixel
entirely instead of producing opaque black.

## Where the bug likely lives in SwiftShader

I haven't read the source, but the symptom points at two specific
places:

1. The **immutable-texture short-circuit in the completeness check**
   appears to be missing or unreached. The texture object presumably
   has an `immutable` / `isImmutable` flag set by `glTexStorage2D`; the
   completeness predicate should test it before falling through to the
   per-level checks.
2. The **incomplete-texture sentinel** appears to be a hard-coded
   `(0, 0, 0, 0)` (e.g. a zero-init `vec4` or a memset'd byte buffer)
   rather than the spec-required type-dependent value:
   `(0, 0, 0, 1)` for floating-point sampled types,
   `(0, 0, 0, 0xFFFFFFFF)` for integer ones.

Fixing either one alone would already unbreak this repo's repro.

## Effect on real applications

A textured-quad pipeline that uses `GL_ONE_MINUS_SRC_ALPHA` blending
against a `(0, 0, 0, 0)` destination drops the pixel entirely on
swiftshader instead of producing opaque black — which is how this
surfaced for us. Any application using `glTexStorage2D` for textures
without explicitly setting a non-mipmap min filter is affected.

**Client workaround**: call
```c
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
// or GL_LINEAR
```
after creating the texture. Once the min filter is non-mipmap-aware,
the completeness check passes on swiftshader and the sample returns
red as expected.

## Reproducing

### As an instrumented test (hard pass/fail)

```sh
# Launch an emulator with `-gpu swiftshader` (or `-gpu swangle`), then:
./gradlew :app:connectedDebugAndroidTest
```

On `-gpu swangle` the test passes. On `-gpu swiftshader` it fails with:

```
com.example.swsrepro.ReproTest > immutableTextureSamplesAsRed FAILED
  java.lang.AssertionError:
    pixel (0, 0): expected [255, 0, 0, 255], got [0, 0, 0, 0]:
    arrays first differed at element [0]; expected:<255> but was:<0>
```

The test reads pixels `(0, 0)` and `(W/2, H/2)` from the readback
buffer and asserts both equal `(255, 0, 0, 255)`. Source:
[`app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt`](app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt).

### Via CI

Push any branch and [`.github/workflows/repro.yml`](.github/workflows/repro.yml)
runs the build + emulator matrix for both backends. The swiftshader
job fails on the instrumented test (that *is* the bug demonstration);
both jobs upload the rendered PNG + JUnit reports as CI artifacts.

### Via the Activity (visual PNG)

```sh
./gradlew assembleDebug
# launch an emulator with `-gpu swiftshader`, then:
APK_PATH=app/build/outputs/apk/debug/app-debug.apk ./ci/run-repro.sh
# inspect `artifacts/swsrepro-logcat.txt` and `artifacts/pngs/test.png`
```

## Versions tested

| Component             | Version                                                       |
|-----------------------|---------------------------------------------------------------|
| Android emulator      | API 35, `system-images;android-35;google_apis;x86_64`         |
| `GL_VENDOR`           | `Google (Google Inc.)`                                        |
| `GL_RENDERER`         | `Android Emulator OpenGL ES Translator (Google SwiftShader)`  |
| `GL_VERSION`          | `OpenGL ES 3.0 (OpenGL ES 3.0 SwiftShader 4.0.0.1)`           |
| Android NDK           | `28.2.13676358`                                               |
| Host                  | GitHub Actions `ubuntu-22.04` runners, KVM                    |

## Source

- [`app/src/main/cpp/repro.c`](app/src/main/cpp/repro.c) — ~230 lines:
  EGL bring-up + the textured-quad draw + readback.
- [`app/src/main/cpp/jni_glue.c`](app/src/main/cpp/jni_glue.c) — single
  JNI entry point (`ReproNative.runTest`).
- [`app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt`](app/src/androidTest/kotlin/com/example/swsrepro/ReproTest.kt)
  — the pass/fail instrumented test.
- [`.github/workflows/repro.yml`](.github/workflows/repro.yml) —
  build + emulator matrix on GHA, runs both the visual repro and
  the instrumented test for `swiftshader` and `swangle`.
