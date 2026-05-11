package com.example.swsrepro

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 512
    const val HEIGHT = 512

    /**
     * Shared-context cross-thread upload + main-thread sample.
     *
     * Main thread initialises EGL and creates a GLES3 context A. A worker
     * pthread creates context B with `share_context = A`, allocates a
     * 256×256 RGBA8 texture, uploads opaque-red pixels via PBO +
     * `glTexSubImage2D`, fences (`glFenceSync`), flushes, releases. Main
     * thread `glWaitSync`s the fence, binds the worker-uploaded texture,
     * draws a full-NDC quad into a 512×512 FBO, reads back.
     *
     * Expected output: every pixel `(255, 0, 0, 255)`.
     * On `-gpu swiftshader`: every pixel `(0, 0, 0, 0)`.
     *
     * `outPixels` must be at least WIDTH * HEIGHT * 4 bytes. Returns a
     * "<success>|<error>|<centerR,G,B,A>|<cornerR,G,B,A>" summary string.
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
