# SwiftShader Android emulator render bug — minimal repro

This repository is a minimal Android NDK reproducer for a render bug in
**SwiftShader 4.0.0.1**, as used by the Android emulator's OpenGL ES
Translator (`-gpu swiftshader`). It builds a tiny APK that runs a single
GL operation in `onCreate` and writes a PNG of the result.

## TL;DR

`glMapBufferRange` on `GL_PIXEL_UNPACK_BUFFER` with
`GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT` is broken on SwiftShader's
Android GLES translator. Writes through the mapped pointer don't reach the
buffer's backing store. The following sequence:

```c
GLuint pbo;
glGenBuffers(1, &pbo);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
glBufferData(GL_PIXEL_UNPACK_BUFFER, byteSize, NULL, GL_STREAM_DRAW);

void* mapped = glMapBufferRange(
    GL_PIXEL_UNPACK_BUFFER, 0, byteSize,
    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
memcpy(mapped, redPixels, byteSize);          // fill PBO with red
glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                GL_RGBA, GL_UNSIGNED_BYTE,
                (const void*)0);              // upload from PBO offset 0
```

…uploads zeros to the texture, even though `mapped` was filled with red.

## Symptom

The repro draws a textured quad from a 256×256 RGBA8 source texture into a
512×512 RGBA8 destination FBO, then `glReadPixels` back to compare:

| GPU backend                                   | center pixel       | corner pixel       |
|-----------------------------------------------|--------------------|--------------------|
| `-gpu swiftshader` (this bug)                 | `(0, 0, 0, 0)`     | `(0, 0, 0, 0)`     |
| `-gpu swangle` (ANGLE → Vulkan → SwiftShader) | `(255, 0, 0, 255)` | `(255, 0, 0, 255)` |

`swangle` goes through ANGLE → Vulkan → SwiftShader-Vulkan and produces the
expected red output, so the bug is specifically in SwiftShader's direct GLES
translator path.

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

## What we narrowed (and what we ruled out)

Bisection on the `bisect-padding-size` branch (one CI run per row, swiftshader
center pixel reported):

| Configuration                                                                                              | swiftshader |
|------------------------------------------------------------------------------------------------------------|-------------|
| Original failing case: `glMapBufferRange(WRITE \| INVALIDATE_BUFFER)` + memcpy + unmap + `glTexSubImage2D` | ❌ zeros    |
| Same path with `WRITE` only (no INVALIDATE)                                                                | ✅ red      |
| Same path with `WRITE \| INVALIDATE_RANGE_BIT` instead of `INVALIDATE_BUFFER_BIT`                          | ✅ red      |
| PBO upload via `glBufferSubData` (no map/unmap)                                                            | ✅ red      |
| Direct `glTexImage2D` from CPU pointer (no PBO at all)                                                     | ✅ red      |
| Worker pthread + shared context + glFenceSync (original renderer pattern)                                  | ❌ zeros    |
| Single main-thread context (no worker, no shared context, no fence)                                        | ❌ zeros    |

Everything outside the broken-flag combination renders the expected red. So
the trigger is exactly the `WRITE_BIT | INVALIDATE_BUFFER_BIT` combination on
a `GL_PIXEL_UNPACK_BUFFER`.

Things we initially suspected and ruled out:

- **Cross-thread / shared-context texture sharing.** The original
  reproducer (extracted from a real renderer) ran the upload on a worker
  thread with `eglCreateContext(... share_context = main)`,
  `glFenceSync` + `glFlush` on the worker, and `glWaitSync` on the main
  thread. We collapsed that to a single main-thread context and the bug
  still reproduces. Cross-thread sharing isn't the trigger.
- **A "preamble" pattern.** Earlier hypothesis: that a prior off-thread
  EGL/GLES session in the same process puts SwiftShader into a bad state.
  Bisected through every stage (off-thread → main-thread; full GL → bare
  EGL; init+terminate → just `eglGetDisplay`; finally no preamble at all).
  Phase 2 alone reproduces the bug, so the preamble was never load-bearing.
- **Library size / binary layout.** Earlier hypothesis: that the bug
  needed a certain code size in `libswsrepro.so`. Bisected by stubbing /
  removing all unused variant code. The bug still reproduces with the
  minimal binary (~21 KB stripped x86_64). Not a layout-sensitivity bug.

## Working theory

`glMapBufferRange` with `GL_MAP_INVALIDATE_BUFFER_BIT` is allowed to skip
reading the existing buffer contents (and possibly return scratch storage),
on the understanding that the caller will overwrite the whole buffer. The
result of writing through the returned pointer must end up in the buffer
on `glUnmapBuffer`.

SwiftShader's translator appears to either (a) hand back a pointer that
isn't actually the buffer's storage when this flag is set, and not commit
the writes on `glUnmapBuffer`, or (b) skip the `glUnmapBuffer` commit
entirely under this flag. Either way, the subsequent
`glTexSubImage2D(... (const void*)0)` reads from the PBO before any data
has actually landed, so the texture gets zeros.

`GL_MAP_INVALIDATE_RANGE_BIT` (which only invalidates the mapped range,
not the whole buffer) does not hit the same bug. `glBufferSubData` also
does not hit it.

Source files of interest in this repo:

- [`app/src/main/cpp/repro.c`](app/src/main/cpp/repro.c) — the actual
  trigger sequence; see the `// The trigger:` comment.
- [`app/src/main/cpp/jni_glue.c`](app/src/main/cpp/jni_glue.c) — the
  single JNI entry point.
- [`app/src/main/kotlin/com/example/swsrepro/MainActivity.kt`](app/src/main/kotlin/com/example/swsrepro/MainActivity.kt)
  — calls `ReproNative.runTest` from `Activity.onCreate`, writes the
  result PNG to `cacheDir`.
- [`ci/run-repro.sh`](ci/run-repro.sh) — drives the APK on a booted
  emulator, captures logcat + the result PNG.
- [`.github/workflows/repro.yml`](.github/workflows/repro.yml) — runs
  the build + emulator-run matrix on GHA for both `swiftshader` and
  `swangle`.

## Reproducing locally

```sh
./gradlew assembleDebug
# launch an emulator with `-gpu swiftshader`, then:
APK_PATH=app/build/outputs/apk/debug/app-debug.apk ./ci/run-repro.sh
# inspect `artifacts/swsrepro-logcat.txt` and `artifacts/pngs/test.png`
```

Or push a branch — `.github/workflows/repro.yml` runs the build + emulator
matrix automatically. CI artifacts include the PNGs for both backends.
