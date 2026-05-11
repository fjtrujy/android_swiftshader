# android_swiftshader

Minimal Android NDK reproducer for a SwiftShader Android-emulator render bug on
Linux x86_64.

## Symptom

```
                            -gpu swiftshader            -gpu swangle
variant19 center=(256, 256):    (0, 0, 0, 0)               (255, 0, 0, 255)
variant19 corner=(0, 0):        (0, 0, 0, 0)               (255, 0, 0, 255)
```

Same APK, same emulator, same scene — only the GPU mode flag differs.

| `-gpu` flag | GL_RENDERER |
|---|---|
| `swiftshader` | `Android Emulator OpenGL ES Translator (Google SwiftShader)` (SwiftShader 4.0.0.1, GLES 3.0) |
| `swangle` | `ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero)), SwiftShader driver-5.0.0)` (GLES 3.1) |

## What the test invokes

`MainActivity.onCreate` calls into JNI exactly twice, in order:

1. **`ReproNative.runVariant10(...)`** — *preamble*. Spawns a worker pthread
   which:
   - `eglGetDisplay`, `eglInitialize`,
   - `eglChooseConfig(EGL_OPENGL_ES2_BIT | EGL_PBUFFER_BIT)`,
   - `eglCreateContext(CLIENT_VERSION=2, share_context=EGL_NO_CONTEXT)` — **FRESH** (NOT shared) context,
   - `eglCreatePbufferSurface(1x1)`, `eglMakeCurrent`,
   - compiles a tiny vertex + fragment shader,
   - allocates a 512×512 RGBA8 texture + FBO,
   - draws an opaque-red quad with `glDisable(GL_BLEND); glBlendFunc(GL_ONE, GL_ZERO)`,
   - `glReadPixels`,
   - tears down (`eglDestroy*`, `eglTerminate`).

   The pixel output is discarded; only the side effect of having done this
   sequence on a worker pthread matters.

2. **`ReproNative.runVariant19(...)`** — *test*. The actual repro:
   - Main thread: `eglInitialize`, GLES3 context A, 1×1 pbuffer, makeCurrent.
   - Spawns a worker pthread which:
     - Creates GLES3 context B with `share_context = A` (SHARED).
     - `glGenTextures` + `glTexStorage2D` (256×256 RGBA8).
     - PBO upload: `glBufferData` → `glMapBufferRange` → `memcpy` red →
       `glUnmapBuffer` → `glTexSubImage2D` from PBO offset 0.
     - `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE)`, `glFlush`, release context.
   - Main thread: `pthread_join`, `glWaitSync(fence, 0, GL_TIMEOUT_IGNORED)`,
     `glDeleteSync(fence)`.
   - Main thread: binds the worker-uploaded texture, draws a full-NDC quad
     into a 512×512 FBO with `glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`,
     `glFinish`, `glReadPixels`.

   Expected output: every pixel `(255, 0, 0, 255)`.
   Observed on `-gpu swiftshader` after step 1 has run: every pixel `(0, 0, 0, 0)`.

## Phase-ordering dependency

The bug only manifests when both steps execute, in order, on the same process.

- **Step 2 alone** → swiftshader returns red. No bug.
- **Step 1 alone** → no output to compare. No bug.
- **Step 1 then Step 2** → swiftshader returns `(0, 0, 0, 0)`. **Bug.**

Whatever process-wide state SwiftShader-direct holds onto after Step 1's
fresh-context worker session corrupts Step 2's shared-context texture sharing.

## Library-size dependency

The library (`libswsrepro.so`) contains additional C code that is *compiled*
but never *invoked* — historical variants 2–9 and 11–18 from the bisection
that found this bug. They are deliberately left in.

Independent observation: aggressive stripping that removes those functions
*also* makes the bug stop reproducing, even though they are never called.
Compiling-but-not-invoking them is part of what triggers the failure. We do
not have a theory for this; it's plausibly a heap-layout or static-init-order
sensitivity in SwiftShader's process-wide state. **Reporting this as evidence —
it may itself be a useful clue.**

## Reproducing

CI runs the test automatically on `push` and `workflow_dispatch`
(`.github/workflows/repro.yml`). Two matrix jobs run the same APK on the same
emulator with different `-gpu` flags; each uploads:

- `variant10.png`, `variant19.png` — raw `glReadPixels` output via Bitmap.
- `swsrepro-logcat.txt` — the in-app summary lines (raw RGBA center+corner).

Locally:

```
./gradlew :app:assembleDebug
# install on a Linux x86_64 emulator booted with `-gpu swiftshader`
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.swsrepro/.MainActivity
adb exec-out run-as com.example.swsrepro cat cache/variant19.png > variant19.png
```

The summary lines in logcat (`adb logcat -s swsrepro:I`) print raw center +
corner RGBA values directly from `glReadPixels` — no Bitmap conversion, so
premultiplied-alpha quirks can't mask anything.

## Environment

- Ubuntu 22.04 x86_64 (GitHub-hosted runner)
- Android emulator 36.4.10.0 (`emulator -version`)
- System image: `system-images;android-35;google_apis;x86_64`
- NDK 28.2.13676358

## Files of interest for review

- `app/src/main/cpp/repro.c`:
  - `repro_variant10_offthread_gl` (preamble) — line ~698
  - `repro_variant19_shared_context_upload_and_sample` (test) — line ~1961
  - The helpers `run_with_gl`, `compile_shader`, `set_err` etc. they share.
- `app/src/main/cpp/jni_glue.c`: just two JNI exports get invoked (`runVariant10`, `runVariant19`); the rest exist for the library-size dependency.
- `app/src/main/kotlin/com/example/swsrepro/MainActivity.kt`: the two-call sequence.
- `.github/workflows/repro.yml`: matrix CI.
