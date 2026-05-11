# android_swiftshader

Minimal Android NDK reproducer for a SwiftShader Android-emulator render bug on
Linux x86_64.

## TL;DR

`MainActivity` calls into JNI twice. After both phases run on `-gpu swiftshader`,
the second phase's `glReadPixels` returns uniform `(0, 0, 0, 0)` instead of red
— as if the worker-thread `glTexSubImage2D` (via shared EGL context, fenced
with `glFenceSync` / `glWaitSync`) had never made the texture data visible to
the main thread.

The same APK on the same emulator with `-gpu swangle` returns the expected red.

```
                            -gpu swiftshader            -gpu swangle
variant19 center=(256, 256):    (0, 0, 0, 0)               (255, 0, 0, 255)
variant19 corner=(0, 0):        (0, 0, 0, 0)               (255, 0, 0, 255)
```

| `-gpu` flag | GL_RENDERER |
|---|---|
| `swiftshader` | `Android Emulator OpenGL ES Translator (Google SwiftShader)` (SwiftShader 4.0.0.1, GLES 3.0) |
| `swangle` | `ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero)), SwiftShader driver-5.0.0)` (GLES 3.1) |

## What the test invokes

`MainActivity.onCreate` calls into JNI exactly twice, in order:

### Phase 1 — `runVariant10` (preamble)

Spawns a worker `pthread` which:

```
eglGetDisplay(EGL_DEFAULT_DISPLAY)
eglInitialize
eglChooseConfig(EGL_OPENGL_ES2_BIT | EGL_PBUFFER_BIT, RGBA8888)
eglCreateContext(CLIENT_VERSION=2, share_context=EGL_NO_CONTEXT)   ← FRESH (NOT shared)
eglCreatePbufferSurface(1×1)
eglMakeCurrent
compile vs + fs (uniform-color)
glLinkProgram
glGenTextures / glTexImage2D(RGBA8, 512×512) / glGenFramebuffers / glFramebufferTexture2D
glUseProgram / glViewport / glDisable(GL_BLEND) / glBlendFunc(ONE, ZERO)
glClearColor(0, 0, 0, 0) / glClear
glUniform4f(red) / glDrawArrays(TRIANGLE_STRIP, 0, 4) / glFinish
glReadPixels                                                       (output discarded)
eglMakeCurrent(NO_CONTEXT) / eglDestroy* / eglTerminate
pthread_join
```

Just doing this once on a worker pthread is sufficient priming. No specific
pixel content matters — only the side effect of an EGL session on a worker
thread.

### Phase 2 — `runVariant19` (the actual test)

```
[main thread]
eglInitialize
eglChooseConfig(EGL_OPENGL_ES3_BIT | EGL_PBUFFER_BIT, RGBA8888)
eglCreateContext A (CLIENT_VERSION=3, share_context=EGL_NO_CONTEXT)
eglCreatePbufferSurface(1×1) / eglMakeCurrent

spawn pthread W:
  [worker thread]
  eglCreateContext B (CLIENT_VERSION=3, share_context=A)            ← SHARED with main
  eglCreatePbufferSurface(1×1) / eglMakeCurrent
  glGenTextures / glBindTexture(GL_TEXTURE_2D, t)
  glTexStorage2D(t, 1, GL_RGBA8, 256, 256)
  glGenBuffers(pbo) / glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo)
  glBufferData(256·256·4, GL_STREAM_DRAW)
  glMapBufferRange(WRITE | INVALIDATE_BUFFER)
    memcpy (255, 0, 0, 255) into mapped buffer
  glUnmapBuffer
  glTexSubImage2D(t, 0, 0, 0, 256, 256, RGBA, UNSIGNED_BYTE, offset 0)
  glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)
  glFlush
  eglMakeCurrent(NO_CONTEXT) / eglDestroy*
pthread_join

[main thread]
glWaitSync(fence, 0, GL_TIMEOUT_IGNORED) / glDeleteSync(fence)
glGenTextures + glTexImage2D(512×512) / glGenFramebuffers / glFramebufferTexture2D
compile vs + fs (textured-quad) / glLinkProgram / glUseProgram
glBindTexture(GL_TEXTURE_2D, t)                                    ← worker's texture
glUniform1i(u_tex, 0)
glViewport(0, 0, 512, 512)
glEnable(GL_BLEND) / glBlendFunc(ONE, ONE_MINUS_SRC_ALPHA)
glClearColor(0, 0, 0, 0) / glClear
glDrawArrays(TRIANGLE_STRIP, 0, 4) / glFinish
glReadPixels                                                       (the value we test)
```

Expected: every output pixel `(255, 0, 0, 255)`.
On `-gpu swiftshader` (after Phase 1 has primed the process): every pixel
reads as `(0, 0, 0, 0)` — the cleared background, as if the worker's
`glTexSubImage2D` writes were never visible to the main context's sampler.

## Phase-ordering dependency

The bug only manifests when both phases run, in that order, in the same process.

- **Phase 2 alone** → swiftshader returns red. No bug.
- **Phase 1 alone** → no comparable output. No bug.
- **Phase 1 then Phase 2** → swiftshader returns `(0, 0, 0, 0)`. **Bug.**

## Library-size dependency (notable side-finding)

The library `libswsrepro.so` contains additional C code that is *compiled* but
never *invoked*: 17 historical variants from the bisection that found this
bug. They are deliberately left in.

**Independent observation**: aggressive stripping that removes those unused
functions also makes the bug stop reproducing — even though they are never
called. We have no theory for this. Plausibly a heap-layout, page-mapping, or
static-init-order sensitivity in SwiftShader's process-wide state. This may
itself be a useful clue.

## Hypotheses to investigate

We could not narrow further from within the test harness — the failure surface
is somewhere inside SwiftShader / the emulator's GLES translator. From the
outside, the prime suspects are:

1. **Shared-EGL-context object visibility.** The worker thread creates context
   B with `share_context = A`. Server-side GL objects (textures, sync) should
   be visible across both contexts. The bug suggests texture *contents* written
   in context B may not be visible in context A even after `glWaitSync`. Worth
   auditing:
   - SwiftShader-direct's per-context texture cache / dirty bits.
   - Whether the GLES translator's host-side share-group bookkeeping correctly
     publishes the worker's `glTexSubImage2D` write to the main thread's view of
     the texture.

2. **`glFenceSync` / `glWaitSync` cross-context semantics.** The fence is
   created on context B and waited on context A. The wait succeeds (no GL
   error). But the sample reads as if the data weren't there. Suggests the
   fence is signalling *before* (or independent of) the texture write being
   visible. Possible places:
   - The fence is implemented as a CPU-side synchronisation only and doesn't
     actually ensure write-visibility of texture memory across contexts.
   - The texture data sits in a host-side staging buffer that's owned by
     context B and never copied to a location context A samples from.

3. **PBO + `glTexSubImage2D(offset)` upload path specifically.** The worker
   uses a PBO-backed upload (`glMapBufferRange` + `memcpy` + `glUnmapBuffer` +
   `glTexSubImage2D` with `pixels = (void*)0` meaning "offset 0 into the bound
   PBO"). Worth checking whether the PBO-based path stores texture data
   somewhere the share group doesn't pick up.

   Note: variants 8/9 in the investigation tested non-PBO `glTexSubImage2D`
   uploads on a single context — those passed cleanly. So the bug requires
   *(PBO upload) ∧ (cross-context share) ∧ (fence sync)*.

4. **Process-wide initialisation primed by Phase 1.** Phase 1's only effect is
   creating and destroying one EGL session on a worker thread. Yet without it
   Phase 2 succeeds. So SwiftShader-direct must be reaching a different
   internal state on its second-or-later EGL initialise/terminate cycle. The
   library-size dependency above might be related: maybe an init path with a
   process-wide pool, mmap region, or pthread-key allocation that hits some
   trigger after enough activity.

5. **EGL config: GLES2 → GLES3 transition.** Phase 1 uses an `EGL_OPENGL_ES2_BIT`
   config with `CLIENT_VERSION = 2`. Phase 2 uses `EGL_OPENGL_ES3_BIT` /
   `CLIENT_VERSION = 3`. Variants where both phases used the same major
   version were not exhaustively tested; the renderer's real code path also
   crosses this version boundary, so this may matter.

The fact that the failure manifests as the texture sampling silently returning
zero (rather than producing a GL error, returning random memory, or crashing)
points to a "the texture exists on this context but its data is unset" path —
i.e., something allocated server-side but where the upload-side commit / fence-
signalled data-publish didn't land on the sampler's view.

## What our 18 negative-control variants ruled out

All passed in isolation on `-gpu swiftshader`:

| # | What it tested |
|---|---|
| 2 | RGBA8 texture-FBO + `glClear` + `glReadPixels` to malloc |
| 3 | Same + `glReadPixels` into AndroidBitmap-locked buffer |
| 4 | RGBA8 texture-FBO + shader-rasterised full-viewport quad |
| 5 | MSAA renderbuffer (4 samples, GL_RGBA8) + `glBlitFramebuffer` resolve |
| 6 | `GL_SRGB8_ALPHA8` texture-FBO + clear |
| 7 | Offset 256×256 red quad on 512×512 FBO with `.copy` blend |
| 8 | Texture sampling (source texture pre-filled with red) |
| 9 | Non-PBO `glTexSubImage2D` upload + readback |
| 10 | "Variant 7" run on a worker pthread (fresh, non-shared context) |
| 11 | Chained CPU upload + sample + `.copy` blend (single frame) |
| 12 | Variant 11 looped 50 times |
| 13 | `glDrawArraysInstanced` (`gl_InstanceID` math) + texture sampling + normal blend |
| 14 | `glViewport(0, -N, 2N, 2N)` (negative-Y, extends past FB) |
| 15 | `glDrawArraysInstanced` with per-instance vertex attribute (`glVertexAttribDivisor`) |
| 16 | Two-pass: negative-Y-viewport draw then normal-viewport instanced draw |
| 17 | `sampler2D[N]` dynamic-indexed in fragment shader (note: ANGLE rejected this at compile) |
| 18 | `sampler2DArray` with per-instance layer index |

Only the combination of all of:
- a prior off-thread fresh-context EGL session,
- and a subsequent shared-EGL-context cross-thread `glTexSubImage2D`-via-PBO upload,
- with `glFenceSync` / `glWaitSync` synchronisation,
- and a sample on the main thread,

— produces the failure.

## Reproducing

CI runs the test automatically on `push` and `workflow_dispatch`
(`.github/workflows/repro.yml`). Two matrix jobs run the same APK on the same
emulator with different `-gpu` flags; each uploads:

- `variant10.png`, `variant19.png` — raw `glReadPixels` output via Bitmap.
- `swsrepro-logcat.txt` — the in-app summary lines (raw RGBA center+corner).

Latest green runs (variant19 swiftshader → `(0,0,0,0)`, swangle → red) are linked
from the README of each commit's run artefact list.

Locally:

```
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.swsrepro/.MainActivity
adb logcat -s swsrepro:I     # summary lines directly from glReadPixels (no Bitmap)
adb exec-out run-as com.example.swsrepro cat cache/variant19.png > variant19.png
```

The logcat summary is authoritative — it prints raw RGBA center + corner
directly from `glReadPixels`, before any AndroidBitmap conversion, so
premultiplied-alpha quirks can't mask anything.

## Environment

- Ubuntu 22.04 x86_64 (GitHub-hosted runner)
- Android emulator 36.4.10.0 (`emulator -version`)
- System image: `system-images;android-35;google_apis;x86_64`
- NDK 28.2.13676358

## Files of interest for review

| File | Lines | Purpose |
|---|---|---|
| **`app/src/main/cpp/repro.c`** | ~430 | Read this. Defines the two test entry points and the helpers/shaders they share. |
| **`app/src/main/cpp/repro_internal.h`** | ~45 | Shared declarations between `repro.c` and `padding.c`. |
| `app/src/main/cpp/padding.c` | ~1490 | Historical variants 2-9, 11-18 from the bisection. Never invoked but **do not strip** — see "Library-size dependency" above. |
| `app/src/main/cpp/jni_glue.c` | ~250 | JNI bindings; only `runVariant10` and `runVariant19` are called from Kotlin. |
| `app/src/main/kotlin/com/example/swsrepro/MainActivity.kt` | ~50 | The two-call sequence (Phase 1, Phase 2, PNG save). |
| `.github/workflows/repro.yml` | ~110 | Matrix CI (`swiftshader` vs `swangle`). |

In `repro.c` the entry points are:

- `repro_variant10_offthread_gl` — **Phase 1 (preamble)**.
- `repro_variant19_shared_context_upload_and_sample` (calls `v19_main_path` +
  `v19_worker`) — **Phase 2 (the test)**.
