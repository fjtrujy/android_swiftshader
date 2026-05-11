package com.example.swsrepro

import android.graphics.Bitmap

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 256
    const val HEIGHT = 256

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
