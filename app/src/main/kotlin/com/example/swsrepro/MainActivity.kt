package com.example.swsrepro

import android.app.Activity
import android.graphics.Bitmap
import android.os.Bundle
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer

class MainActivity : Activity() {
    private val tag = "swsrepro"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        runRepro()
        finish()
    }

    private fun runRepro() {
        val outDir = cacheDir
        outDir.listFiles()?.forEach { it.delete() }
        Log.i(tag, "output dir = ${outDir.absolutePath}")

        // Phase 1 — preamble: spawn an off-thread fresh-context GLES2 draw.
        // SwiftShader on Linux x86_64 emulator needs this prior activity for the
        // subsequent cross-context test to fail.
        val preambleSummary = ReproNative.parse(ReproNative.runPreamble())
        Log.i(tag, "preamble: success=${preambleSummary.success} err='${preambleSummary.error}'")

        // Phase 2 — the actual test.
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(ReproNative.runTest(pixels))
        Log.i(
            tag,
            "test: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}",
        )
        if (!summary.success) return

        val bmp = Bitmap.createBitmap(
            ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888
        )
        bmp.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))
        val out = File(outDir, "output.png")
        FileOutputStream(out).use { os ->
            bmp.compress(Bitmap.CompressFormat.PNG, 100, os)
        }
        Log.i(tag, "wrote ${out.absolutePath} (${out.length()} bytes)")
    }
}
