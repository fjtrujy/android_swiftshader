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

## The two phases

The test runs in two phases. Each is small and well-defined, but **both** are
needed to trigger the bug — neither alone reproduces it. Whatever process-wide
state SwiftShader-direct holds onto after Phase 1 corrupts Phase 2's
cross-context texture sharing.

### Phase 1 — preamble (off-thread fresh-context GLES2)

```
spawn pthread P:
  P: eglGetDisplay
  P: eglInitialize
  P: eglChooseConfig(EGL_OPENGL_ES2_BIT)
  P: eglCreateContext(CLIENT_VERSION=2, share_context=EGL_NO_CONTEXT)   ← FRESH, NOT shared
  P: eglCreatePbufferSurface(1x1)
  P: eglMakeCurrent
  P: glCreateShader vs/fs; glCompileShader; glLinkProgram
  P: glGenTextures, glGenFramebuffers, glFramebufferTexture2D (RGBA8)
  P: glViewport, glDisable(GL_BLEND), glClearColor, glClear
  P: glDrawArrays(GL_TRIANGLE_STRIP, 0, 4) -> red quad
  P: glReadPixels
  P: eglMakeCurrent(NO_CONTEXT); eglDestroy*; eglTerminate
pthread_join(P)
```

Just one of these on a worker thread is enough preamble; no specific draw
content matters.

### Phase 2 — test (shared-context cross-thread upload + main-thread sample)

```
main: eglGetDisplay; eglInitialize
main: eglChooseConfig(EGL_OPENGL_ES3_BIT)
main: eglCreateContext A (CLIENT_VERSION=3, share_context=EGL_NO_CONTEXT)
main: eglCreatePbufferSurface(1x1); eglMakeCurrent
main: spawn pthread W:
  W: eglCreateContext B (CLIENT_VERSION=3, share_context=A)              ← SHARED with main
  W: eglCreatePbufferSurface(1x1); eglMakeCurrent
  W: glGenTextures; glBindTexture(GL_TEXTURE_2D, t)
  W: glTexStorage2D(1, GL_RGBA8, 256, 256)
  W: glGenBuffers(pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo)
  W: glBufferData(256*256*4, GL_STREAM_DRAW)
  W: glMapBufferRange(WRITE | INVALIDATE_BUFFER)
  W:   memcpy red (255, 0, 0, 255) into mapped buffer
  W: glUnmapBuffer
  W: glTexSubImage2D(t, 0, 0, 0, 256, 256, RGBA, UNSIGNED_BYTE, offset 0 into PBO)
  W: glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE); glFlush
  W: eglMakeCurrent(NO_CONTEXT); eglDestroy*
pthread_join(W)
main: glWaitSync(fence, 0, GL_TIMEOUT_IGNORED); glDeleteSync(fence)
main: glGenTextures + glTexImage2D 512x512; glGenFramebuffers; glFramebufferTexture2D
main: glCreateShader (textured-quad shader); glLinkProgram; glUseProgram
main: glBindTexture(t)                                       ← the worker's texture
main: glUniform1i(u_tex, 0)
main: glViewport(0, 0, 512, 512); glEnable(GL_BLEND); glBlendFunc(ONE, ONE_MINUS_SRC_ALPHA)
main: glClearColor(0,0,0,0); glClear; glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish
main: glReadPixels
```

Expected: every output pixel `(255, 0, 0, 255)`. The shared texture has red
pixels (we wrote them); the fence has been waited on; the draw samples from it.

On `-gpu swiftshader` (after Phase 1 has primed the process), every output pixel
reads as `(0, 0, 0, 0)` — as if the worker's `glTexSubImage2D` writes were never
visible to the main context, despite the fence.

## Reproducing

CI runs the test automatically on `push` and on `workflow_dispatch`. See
`.github/workflows/repro.yml`. Two matrix jobs run the same APK on the same
emulator with different `-gpu` flags; each uploads its raw `glReadPixels` output
as a PNG (`repro-{swiftshader,swangle}`).

Locally:

```
./gradlew :app:assembleDebug
# install on a Linux x86_64 emulator booted with `-gpu swiftshader`
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.swsrepro/.MainActivity
adb exec-out run-as com.example.swsrepro cat cache/output.png > output.png
```

The summary lines in logcat (`adb logcat -s swsrepro:I`) print raw center +
corner RGBA values directly from `glReadPixels` — no Bitmap conversion, so
premultiplied-alpha quirks can't mask anything.

## Environment

- Ubuntu 22.04 x86_64 (GitHub-hosted runner)
- Android emulator 36.4.10.0 (`emulator -version`)
- System image: `system-images;android-35;google_apis;x86_64`
- NDK 28.2.13676358

## Note: phase-ordering is load-bearing

Running only Phase 2 (without Phase 1) on `-gpu swiftshader` produces the
correct output. Running only Phase 1 produces the correct output. Running
Phase 1 then Phase 2 produces `(0, 0, 0, 0)`. The bug is in some process-wide
state that Phase 1 leaves behind and Phase 2 then trips over.
