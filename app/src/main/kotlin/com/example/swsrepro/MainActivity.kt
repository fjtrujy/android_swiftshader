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
        runAllVariants()
    }

    private fun runAllVariants() {
        val outDir = getExternalFilesDir(null) ?: filesDir
        Log.i(tag, "output dir = ${outDir.absolutePath}")

        runByteArrayVariant("variant2", outDir, ReproNative::runVariant2)
        runBitmapVariant("variant3", outDir)
        runByteArrayVariant("variant4", outDir, ReproNative::runVariant4)
        runByteArrayVariant("variant5", outDir, ReproNative::runVariant5)
        runByteArrayVariant("variant6", outDir, ReproNative::runVariant6)

        Log.i(tag, "all variants finished")
        finish()
    }

    private fun runByteArrayVariant(name: String, outDir: File, fn: (ByteArray) -> String) {
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(fn(pixels))
        Log.i(tag, "$name summary: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}")
        if (!summary.success) return

        val bmp = Bitmap.createBitmap(ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888)
        bmp.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))
        savePng(bmp, File(outDir, "$name.png"))
    }

    private fun runBitmapVariant(name: String, outDir: File) {
        val bmp = Bitmap.createBitmap(ReproNative.WIDTH, ReproNative.HEIGHT, Bitmap.Config.ARGB_8888)
        val summary = ReproNative.parse(ReproNative.runVariant3(bmp))
        Log.i(tag, "$name summary: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}")
        if (!summary.success) return
        savePng(bmp, File(outDir, "$name.png"))
    }

    private fun savePng(bmp: Bitmap, file: File) {
        FileOutputStream(file).use { os ->
            bmp.compress(Bitmap.CompressFormat.PNG, 100, os)
        }
        Log.i(tag, "wrote ${file.absolutePath} (${file.length()} bytes)")
    }
}
