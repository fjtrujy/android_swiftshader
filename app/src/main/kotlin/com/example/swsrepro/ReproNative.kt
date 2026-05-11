package com.example.swsrepro

object ReproNative {
    init { System.loadLibrary("swsrepro") }

    const val WIDTH = 512
    const val HEIGHT = 512

    /**
     * Runs the standalone minimal repro of a SwiftShader Android-emulator render bug.
     *
     * On success the supplied `outPixels` ByteArray (must be at least WIDTH*HEIGHT*4
     * bytes) is filled with the raw RGBA8 output of the main thread's `glReadPixels`.
     *
     * Returns a pipe-delimited summary string for easy parsing:
     *   `<success:0|1>|<error>|<centerR,G,B,A>|<cornerR,G,B,A>`
     */
    @JvmStatic external fun run(outPixels: ByteArray): String

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
