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

        // Phase 1 — preamble.
        val preamblePixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val preambleSummary = ReproNative.parse(ReproNative.runPreamble(preamblePixels))
        Log.i(
            tag,
            "preamble: success=${preambleSummary.success} err='${preambleSummary.error}' " +
                "center=${preambleSummary.center.toList()} corner=${preambleSummary.corner.toList()}",
        )
        if (preambleSummary.success) {
            savePng(preamblePixels, File(outDir, "preamble.png"))
        }

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
