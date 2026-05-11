# Prior Investigation — GoodNotes Renderer × SwiftShader

This document summarises a multi-day investigation we ran against the
GoodNotes Renderer test suite on Android CI before extracting this minimal
repro project. It records the *exact* failure mode, the *exact* operations
that triggered it, and the dead-ends we already ruled out.

Use this when deciding which variants to add to `app/src/main/cpp/repro.c`.

---

## TL;DR

| Environment | What we saw |
|---|---|
| **CI: Ubuntu x86_64 + Android emulator + `-gpu swiftshader`** | Renderer's output bitmap is **uniformly `(R=255, G=255, B=255, A=0)` everywhere**. No drawn elements visible. |
| **Local: Apple Silicon + arm64 emulator + `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan)** | Drawn elements *are* visible, but the page background is `(255, 255, 255, 0)` instead of the expected clear black `(0, 0, 0, 0)`. |
| **Hardware GPU on a real device** | Renders correctly. |

Same Renderer code, same Swift toolchain, same SDK/NDK — only the
SwiftShader build differs between host architectures and entry stacks.

The `(255, 255, 255, 0)` fill is significant: that's the byte pattern for
"opaque white encoded as transparent", a strong hint that whatever the
renderer is asking GL to do, SwiftShader is either silently dropping draw
calls or initialising the framebuffer to a sentinel and never overwriting
it.

---

## What scenarios we tested in the GoodNotes test suite

Four layers, designed to bisect from "is GL alive at all?" to "is the full
Renderer pipeline broken?".

### Layer 1 — Direct GL, no Renderer (PASSED)

Compile a vertex+fragment shader, upload a triangle that covers most of the
viewport, `glDrawArrays`, `glReadPixels` into a malloc buffer. Asserts that
the center is red and a corner is `(0, 0, 0, 0)`.

- Vertex shader: trivial `gl_Position = vec4(a_position, 0.0, 1.0);`.
- Fragment shader: emits `vec4(1.0, 0.0, 0.0, 1.0)`.
- 64×64 RGBA8 texture as a color attachment of an offscreen FBO.
- `glClearColor(0, 0, 0, 0)` before draw.

**Result on x86_64 SwiftShader CI:** ✔ Center is red, corner is transparent
black. Basic GL works.

### Layer 2 — Renderer's own `GPUContext` checkpoints (key narrowing tool)

Drive the Renderer's `GPUContext` abstraction (the wrapper it uses for all
its GL calls) through discrete operations, reading back the target texture
after each. Each checkpoint targets one Renderer-internal capability:

1. **`drawClearTexture(red)`** — fill an entire texture via a clear-shader.
   Expected: red center *and* corner.
2. **`beginDrawing(clearColor: red) + endDrawing`** — bind FBO with a clear
   load action, finish. Expected: red center and corner.
3. **`beginDrawing(clearColor: transparentBlack) + drawColoredRectangle(frame=middle 256×256, color=red, blendMode=.copy)`**
   — partial-extent solid-color draw with `.copy` blend mode (which means
   `glDisable(GL_BLEND)` + `glBlendFunc(GL_ONE, GL_ZERO)`).
   Expected: red center, transparent-black corner.
4. **`drawClearTexture(red)` on a 256×256 source texture** — clear a smaller
   texture, save for use by next checkpoint.
5. **`drawTexturedRectangles(sourceTexture, frame=middle, blendMode=.copy)`**
   — *sample the source texture* and draw it into the target. Tests texture
   sampling, not just clear.
6. **`copy(CPU buffer red square → GL texture)`** — `glTexSubImage2D`-style
   upload of a CPU-side buffer onto a GL texture. Tests CPU→GL upload.
7. **`repack(source texture → dest with offset)`** — texture-to-texture copy
   via a custom Renderer shader, with offset and resize.

**This is the layer that historically narrowed down the bug.** We
*didn't have evidence about exactly which checkpoint failed first* on
x86_64 SwiftShader — that data was never captured. But we know the
*aggregate* Renderer output via Layer 4 was always uniform white-alpha-0.

### Layer 3 — Renderer output texture, *before* AndroidBitmap conversion

Run the full Renderer pipeline against the canonical scene (see below),
read the *GL texture* directly (no AndroidBitmap involved). Asserts the
center pixel is red and the corner is transparent black.

This separates *"renderer GL output is wrong"* from *"bitmap conversion
is wrong"*.

### Layer 4 — Full Renderer pipeline through AndroidBitmap

Same scene as Layer 3, but read pixels via the actual production
AndroidBitmap path the Renderer uses (`tickTillNextOutputImage` →
`AndroidBitmap_lockPixels`).

If Layer 3 fails *and* Layer 4 fails with the same pixels, the bug is
inside the Renderer's GL pipeline (and not the bitmap conversion). That's
what we observed.

---

## The canonical failing scene

| Property | Value |
|---|---|
| Page size | 512 × 512 |
| Page background color | `PColor(r: 0, g: 0, b: 0, a: 0)` (transparent black — **NOT** `PColor.clear`, which is `(1, 1, 1, 0)` in our Geometry library) |
| Element | 256 × 256 red rectangle, origin `(128, 128)`, no rotation, no corner radius |
| Rasterizer | `.syncCPU` |

Expected output:

- Pixel at **(256, 256)** — page center, inside the element → **red `(255, 0, 0, 255)`**.
- Pixel at **(0, 0)** — far corner, outside the element → **transparent black `(0, 0, 0, 0)`**.

What x86_64 SwiftShader CI produced: uniform `(255, 255, 255, 0)` at both positions.

---

## Why earlier "non-zero" tests passed despite the bug

For a long time we had threshold-based tests like "at least X% of bytes
are non-zero". `(255, 255, 255, 0)` is **75% non-zero bytes**, so these
tests *silently passed* against the failing output. The lesson:

- **Assert specific pixel values at specific coordinates**, not byte
  counts or "image is not all zero".
- The `expectedCenter` / `expectedCorner` API in `gpuContextCheckpoints…`
  was explicitly built to avoid this trap.

Carry this rule into the repro app: any assertion must check exact RGBA
tuples at known positions, not aggregate statistics.

---

## What's already ruled out (do not waste time re-checking)

These were checked during the GoodNotes investigation and produced no
fix. Documented here so we don't loop on them.

| Hypothesis | Result |
|---|---|
| **PNG encoder bug** | Wrong. Encoded as PNG, JPEG, WEBP, raw BMP — all four serialised the same uniform `(255, 255, 255, 0)`. Encoders work. |
| **AndroidBitmap `lockPixels` write path** | Wrong. Read the locked pixels back immediately, without any encode round-trip — already wrong at that point. |
| **`setHasAlpha(false)` on the Bitmap** | Wrong. Appeared to "fix" snapshot byte-equality, but only because the snapshots became uniform white (matching themselves byte-for-byte). |
| **`BackgroundGLUploader` thread** | Disabled it for unit tests — no change. |
| **Basic GL on SwiftShader** | Works (Layer 1 test passes). |

---

## What's left to test in this minimal repro

The known-good baseline is **direct GL with shader-rasterised solid-color
triangles and `glReadPixels` to a malloc buffer**. The known-bad operation
is **the full Renderer pipeline producing an output bitmap**. The gap
between them is what this repro must close, one variant at a time.

Variants implemented so far in `app/src/main/cpp/repro.c`:

| # | What it tests | Status on x86_64 swiftshader CI |
|---|---|---|
| 2 | RGBA8 texture-FBO + `glClear(opaque-red)` + `glReadPixels` to malloc | ✔ passed |
| 3 | RGBA8 texture-FBO + `glClear(opaque-red)` + `glReadPixels` into AndroidBitmap-locked buffer | ✔ passed |
| 4 | RGBA8 texture-FBO + shader-rasterised fullscreen green quad | (pending) |
| 5 | MSAA renderbuffer (4 samples, RGBA8) → `glBlitFramebuffer` resolve → readback | (pending) |
| 6 | `GL_SRGB8_ALPHA8` color texture FBO + `glClear(opaque-red)` + readback | (pending) |

Recommended next variants based on what we know hurts the Renderer:

| Proposed # | What to test | Why |
|---|---|---|
| 7 | Partial-extent draw with `glDisable(GL_BLEND)` + `glBlendFunc(GL_ONE, GL_ZERO)` (mimicking `.copy` blend) — clear FBO to transparent black, draw a 256×256 red quad at offset `(128, 128)`, read back | Mirrors Layer 2 checkpoint 3 (`drawColoredRectangle`). The Renderer always uses `.copy` blend; that's the first divergence from basic GL. |
| 8 | Source-texture sampling — clear a small texture to red, sample it via a fragment shader, draw into a larger target | Mirrors Layer 2 checkpoint 5 (`drawTexturedRectangles`). Tests texture-as-input, not just clear/draw. |
| 9 | `glTexSubImage2D` from a CPU buffer onto a GL texture, then read back via FBO | Mirrors Layer 2 checkpoint 6 (Android CPU buffer upload). |
| 10 | Off-thread GL — initialise EGL on one thread, issue draw calls on another | The Renderer drives GL from a render thread, not the activity's main thread. SwiftShader on emulator has had thread-affinity bugs. |

Stop at the **first variant that produces `(255, 255, 255, 0)` everywhere**.
That variant becomes the title of the upstream bug report.

---

## Environment captured so far

| Backend | `GL_RENDERER` |
|---|---|
| `-gpu swiftshader` (Linux x86_64 CI) | `Android Emulator OpenGL ES Translator (Google SwiftShader)` — pure SwiftShader-GL, GLES 3.0, SwiftShader 4.0.0.1 |
| `-gpu swangle` (Linux x86_64 CI) | `ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver-5.0.0)` — ANGLE → Vulkan → SwiftShader-Vulkan, GLES 3.1 |
| `-gpu swangle_indirect` (Apple Silicon local) | `ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (LLVM 10.0.0)), SwiftShader driver-5.0.0)` |

Note the two SwiftShader code paths produce *different* `GL_RENDERER`
strings ("Google SwiftShader" vs "SwiftShader driver-5.0.0 / Subzero").
That's the user-visible signal that they are different builds with
potentially different bug surfaces.
