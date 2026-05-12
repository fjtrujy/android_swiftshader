package com.example.swsrepro

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 256
    const val HEIGHT = 256

    /**
     * Render an immutable, single-level RGBA8 source texture (filled with red)
     * into a destination FBO via a textured-quad shader, then glReadPixels back.
     * The source texture's min filter is left at the default
     * GL_NEAREST_MIPMAP_LINEAR.
     *
     * Expected (GLES3 §8.17): every pixel `(255, 0, 0, 255)`.
     * On `-gpu swiftshader`: every pixel `(0, 0, 0, 0)`.
     *
     * Returns "<success>|<error>|<centerR,G,B,A>|<cornerR,G,B,A>".
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
