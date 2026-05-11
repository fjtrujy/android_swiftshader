package com.example.swsrepro

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 512
    const val HEIGHT = 512

    /**
     * Runs the preamble: a single off-thread fresh-context GLES2 draw session.
     * Returns a summary string. The pixel values are produced inside the worker
     * thread and discarded — they're not needed by the caller, only the side
     * effect of the EGL/GL activity matters.
     */
    @JvmStatic external fun runPreamble(): String

    /**
     * Runs the cross-context texture-upload test.
     *
     * Main thread creates EGL context A. Worker pthread creates context B with
     * `share_context = A`, uploads a 256x256 red RGBA8 texture via PBO +
     * `glTexSubImage2D`, `glFenceSync` + `glFlush`, releases. Main thread
     * `glWaitSync`s the fence, binds the shared texture, draws into a 512x512
     * FBO, `glReadPixels`. Fills `outPixels` (must be 512*512*4 bytes).
     */
    @JvmStatic external fun runTest(outPixels: ByteArray): String

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
