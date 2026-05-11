package com.example.swsrepro

import android.graphics.Bitmap

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 256
    const val HEIGHT = 256

    // Variant 7 uses a larger framebuffer (mirrors the renderer's canonical scene).
    const val WIDTH_V7 = 512
    const val HEIGHT_V7 = 512

    /**
     * Returns a summary string: "<success>|<error>|<centerR,G,B,A>|<cornerR,G,B,A>".
     * On success, `outPixels` is filled with WIDTH*HEIGHT*4 RGBA bytes.
     */
    @JvmStatic external fun runVariant2(outPixels: ByteArray): String

    /**
     * Returns the same summary string. The given Bitmap must be ARGB_8888 of size WIDTH x HEIGHT.
     * On success, the bitmap's pixels are filled via AndroidBitmap_lockPixels + glReadPixels.
     */
    @JvmStatic external fun runVariant3(bitmap: Bitmap): String

    /** Variant 4: shader-rasterized fullscreen quad, opaque green. */
    @JvmStatic external fun runVariant4(outPixels: ByteArray): String

    /** Variant 5: MSAA RGBA8 renderbuffer -> glBlitFramebuffer resolve -> readback. */
    @JvmStatic external fun runVariant5(outPixels: ByteArray): String

    /** Variant 6: GL_SRGB8_ALPHA8 color texture FBO + clear + readback. */
    @JvmStatic external fun runVariant6(outPixels: ByteArray): String

    /**
     * Variant 7: 512x512 FBO, clear to (0, 0, 0, 0), draw 256x256 red quad at offset (128, 128)
     * with `.copy` blend (`glDisable(GL_BLEND); glBlendFunc(GL_ONE, GL_ZERO)`). Mirrors the
     * GoodNotes Renderer's `drawColoredRectangle` call. `outPixels` must be 512*512*4 bytes.
     */
    @JvmStatic external fun runVariant7(outPixels: ByteArray): String

    /** Variant 8: source texture pre-filled with red, sampled to target FBO. */
    @JvmStatic external fun runVariant8(outPixels: ByteArray): String

    /** Variant 9: CPU buffer -> glTexSubImage2D -> FBO -> glReadPixels. */
    @JvmStatic external fun runVariant9(outPixels: ByteArray): String

    /** Variant 10: variant 7 driven on a worker pthread, off the main thread. */
    @JvmStatic external fun runVariant10(outPixels: ByteArray): String

    /** Variant 11: full chained per-frame pipeline (CPU upload + texture sample + .copy blend). */
    @JvmStatic external fun runVariant11(outPixels: ByteArray): String

    /** Variant 12: chained-frame loop x50 against the same GL context. */
    @JvmStatic external fun runVariant12(outPixels: ByteArray): String

    /**
     * Variant 13: 512x512 target FBO + 256x256 red source texture, sampled by a 4x4 grid
     * of 16 instances via drawArraysInstanced, with normal alpha blend. Mirrors the
     * renderer's failing draw call. `outPixels` must be 512*512*4 bytes.
     */
    @JvmStatic external fun runVariant13(outPixels: ByteArray): String

    /**
     * Variant 14: 256x256 FBO with `glViewport(0, -256, 512, 512)` — viewport origin
     * negative, viewport extends past the framebuffer. Mirrors the renderer's atlas-
     * write viewport trick. `outPixels` is the standard 256*256*4.
     */
    @JvmStatic external fun runVariant14(outPixels: ByteArray): String

    /**
     * Variant 15: drawArraysInstanced with per-instance vertex attribute (divisor=1)
     * instead of gl_InstanceID. Mirrors the renderer's stroke / textured-rectangle
     * instancing pattern. `outPixels` must be 512*512*4 bytes.
     */
    @JvmStatic external fun runVariant15(outPixels: ByteArray): String

    /**
     * Variant 16: state-pollution — pass-1 draws with negative-Y viewport, then pass-2
     * draws with normal viewport. Tests whether SwiftShader carries bad state across.
     * `outPixels` is the standard 256*256*4.
     */
    @JvmStatic external fun runVariant16(outPixels: ByteArray): String

    /**
     * Variant 17: drawArraysInstanced sampling from `sampler2D[2]` indexed by per-instance
     * int attribute. Mirrors the renderer's drawTexturedRectangles shader. `outPixels`
     * must be 512*512*4 bytes.
     */
    @JvmStatic external fun runVariant17(outPixels: ByteArray): String

    /** Variant 18: drawArraysInstanced + sampler2DArray with per-instance layer. */
    @JvmStatic external fun runVariant18(outPixels: ByteArray): String

    data class Summary(
        val success: Boolean,
        val error: String,
        val center: IntArray,
        val corner: IntArray,
    )

    fun parse(summary: String): Summary {
        val parts = summary.split("|")
        require(parts.size == 4) { "malformed summary: '$summary'" }
        return Summary(
            success = parts[0] == "1",
            error = parts[1],
            center = parts[2].split(",").map { it.toInt() }.toIntArray(),
            corner = parts[3].split(",").map { it.toInt() }.toIntArray(),
        )
    }
}
