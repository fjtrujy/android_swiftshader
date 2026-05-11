# android_swiftshader

Minimal Android NDK reproducer for a SwiftShader Android-emulator render bug on
Linux x86_64.

## Symptom

```
                            -gpu swiftshader            -gpu swangle
center=(256, 256):    (0, 0, 0, 0)               (255, 0, 0, 255)
corner=(0, 0):        (0, 0, 0, 0)               (255, 0, 0, 255)
```

Same APK, same emulator, same scene — only the GPU mode flag differs.

| `-gpu` flag | GL_RENDERER |
|---|---|
| `swiftshader` | `Android Emulator OpenGL ES Translator (Google SwiftShader)` (SwiftShader 4.0.0.1, GLES 3.0) |
| `swangle` | `ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero)), SwiftShader driver-5.0.0)` (GLES 3.1) |

## What the test does

A 200-line C program that mirrors a real-world pattern observed in production
code (`BackgroundGLUploader` in GoodNotes Renderer):

1. **Main thread**: opens an EGL display, chooses a GLES3 + PBUFFER config, creates context A, makes it current on a 1×1 pbuffer surface.
2. **Worker thread** (`pthread`): creates EGL context B with `share_context = A` (so server-side GL objects are visible across both contexts), makes it current on its own 1×1 pbuffer.
3. Worker allocates a 256×256 `GL_RGBA8` texture via `glTexStorage2D`, then uploads opaque red pixels:
   - `glGenBuffers` a PBO
   - `glBufferData(PIXEL_UNPACK_BUFFER, ..., GL_STREAM_DRAW)`
   - `glMapBufferRange(..., WRITE | INVALIDATE_BUFFER)`
   - memcpy `(255, 0, 0, 255)` for every pixel
   - `glUnmapBuffer`
   - `glTexSubImage2D` from PBO offset 0
4. Worker calls `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)`, `glFlush`, releases its context, exits.
5. Main thread `pthread_join`s the worker, then `glWaitSync(fence, 0, GL_TIMEOUT_IGNORED)`, `glDeleteSync(fence)`.
6. Main thread binds the worker-uploaded texture, attaches a fresh 512×512 `GL_RGBA8` color texture to a freshly created FBO, draws a full-NDC textured quad with normal alpha blend (`GL_ONE`, `GL_ONE_MINUS_SRC_ALPHA`), `glFinish`, `glReadPixels`.

The expected output is uniform `(255, 0, 0, 255)`. On `-gpu swiftshader`, it is uniform `(0, 0, 0, 0)` — as if the worker's texture upload was never visible to the main context, despite the fence sync.

## Reproducing

CI runs the test automatically on `push` and on `workflow_dispatch`. See
`.github/workflows/repro.yml`. Two matrix jobs run the same APK on the same
emulator with different `-gpu` flags so the contrast is immediate; each uploads
its raw `glReadPixels` output as a PNG artefact (`repro-{swiftshader,swangle}`).

Locally:

```
./gradlew :app:assembleDebug
# install on a Linux x86_64 emulator booted with `-gpu swiftshader`
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.swsrepro/.MainActivity
adb exec-out run-as com.example.swsrepro cat cache/output.png > output.png
```

The summary line in logcat (`adb logcat -s swsrepro:I`) prints the raw center +
corner RGBA values directly from `glReadPixels` — no Bitmap conversion, so
premultiplied-alpha quirks can't mask anything.

## Environment

- Ubuntu 22.04 x86_64 (GitHub-hosted runner)
- Android emulator 36.4.10.0 (`emulator -version`)
- System image: `system-images;android-35;google_apis;x86_64`
- NDK 28.2.13676358
- Swift / Kotlin / AGP / Gradle: standard, see `gradle/libs.versions.toml`

## What's already been ruled out

This single test was distilled from a long investigation. The branch history
contains 18 progressively-more-renderer-like variants we built up to isolate
the bug: clear-only readback, shader-rasterised quad, MSAA blit-resolve, sRGB
FBO, offset quad with `.copy` blend, texture sampling, `glTexSubImage2D` upload,
off-thread (fresh-context) GL, chained per-frame pipeline, 50-iteration loop,
instanced draw with both `gl_InstanceID` and `vertexAttribDivisor`,
negative-Y viewport, state-pollution across passes, `sampler2D[N]` dynamic
indexing, and `sampler2DArray` with per-instance layer.

**All 18 of those variants pass on `-gpu swiftshader`.** Only the
SHARED-EGL-CONTEXT cross-thread upload + fence-sync + sample pattern (the
remaining test) fails.

See the commit history before `repro-minimal` for the full bisection trail.
