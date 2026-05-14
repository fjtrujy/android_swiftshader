package com.example.swsrepro

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 256
    const val HEIGHT = 256

    /**
     * Render four color quadrants into an immutable RGBA8 texture, then read it
     * back through the same flipped-FBO-blit shape used by Goodnotes Android
     * snapshot recording.
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
