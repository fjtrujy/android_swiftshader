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

        // Bisect step: SKIP the preamble entirely. If Phase 2 alone reproduces
        // the bug on swiftshader, the preamble was never load-bearing — the
        // bug is purely in Phase 2's shared-context texture path.
        Log.i(tag, "preamble: skipped (bisect)")

        // Phase 2 — the actual test.
        val testPixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val testSummary = ReproNative.parse(ReproNative.runTest(testPixels))
        Log.i(
            tag,
            "test: success=${testSummary.success} err='${testSummary.error}' " +
                "center=${testSummary.center.toList()} corner=${testSummary.corner.toList()}",
        )
        if (testSummary.success) {
            savePng(testPixels, File(outDir, "test.png"))
        }
    }

    private fun savePng(pixels: ByteArray, file: File) {
        val bmp = Bitmap.createBitmap(
            ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888
        )
        bmp.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))
        FileOutputStream(file).use { os ->
            bmp.compress(Bitmap.CompressFormat.PNG, 100, os)
        }
        Log.i(tag, "wrote ${file.absolutePath} (${file.length()} bytes)")
    }
}
