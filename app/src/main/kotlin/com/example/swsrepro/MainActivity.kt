package com.example.swsrepro

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Bundle
import android.util.Log
import java.io.File
import java.io.FileOutputStream

class MainActivity : Activity() {
    private val tag = "swsrepro"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        runAllVariants()
    }

    private fun runAllVariants() {
        val outDir = getExternalFilesDir(null) ?: filesDir
        Log.i(tag, "output dir = ${outDir.absolutePath}")

        runVariant2(outDir)
        runVariant3(outDir)

        Log.i(tag, "all variants finished")
        finish()
    }

    private fun runVariant2(outDir: File) {
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(ReproNative.runVariant2(pixels))
        Log.i(tag, "variant2 summary: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}")
        if (!summary.success) return

        // Wrap the raw RGBA bytes in a Bitmap for PNG encoding.
        val bmp = Bitmap.createBitmap(ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888)
        bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(pixels))
        savePng(bmp, File(outDir, "variant2.png"))
    }

    private fun runVariant3(outDir: File) {
        val bmp = Bitmap.createBitmap(ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888)
        val summary = ReproNative.parse(ReproNative.runVariant3(bmp))
        Log.i(tag, "variant3 summary: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}")
        if (!summary.success) return

        savePng(bmp, File(outDir, "variant3.png"))
    }

    private fun savePng(bmp: Bitmap, file: File) {
        FileOutputStream(file).use { os ->
            bmp.compress(Bitmap.CompressFormat.PNG, 100, os)
        }
        Log.i(tag, "wrote ${file.absolutePath} (${file.length()} bytes)")
    }
}
