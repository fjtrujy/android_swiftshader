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
        // Use internal cache dir — always app-writable, no scoped-storage issues if
        // the AVD cache carries stale files from a previous run. Pulled by CI via
        // `adb exec-out run-as <pkg> cat cache/<file>.png`.
        val outDir = cacheDir
        outDir.listFiles()?.forEach { it.delete() }
        Log.i(tag, "output dir = ${outDir.absolutePath}")

        runByteArrayVariant("variant2", outDir, ReproNative::runVariant2)
        runBitmapVariant("variant3", outDir)
        runByteArrayVariant("variant4", outDir, ReproNative::runVariant4)
        runByteArrayVariant("variant5", outDir, ReproNative::runVariant5)
        runByteArrayVariant("variant6", outDir, ReproNative::runVariant6)
        runByteArrayVariant(
            "variant7", outDir, ReproNative::runVariant7,
            width = ReproNative.WIDTH_V7, height = ReproNative.HEIGHT_V7
        )
        runByteArrayVariant("variant8", outDir, ReproNative::runVariant8)
        runByteArrayVariant("variant9", outDir, ReproNative::runVariant9)
        runByteArrayVariant(
            "variant10", outDir, ReproNative::runVariant10,
            width = ReproNative.WIDTH_V7, height = ReproNative.HEIGHT_V7
        )
        runByteArrayVariant(
            "variant11", outDir, ReproNative::runVariant11,
            width = ReproNative.WIDTH_V7, height = ReproNative.HEIGHT_V7
        )
        runByteArrayVariant(
            "variant12", outDir, ReproNative::runVariant12,
            width = ReproNative.WIDTH_V7, height = ReproNative.HEIGHT_V7
        )
        runByteArrayVariant(
            "variant13", outDir, ReproNative::runVariant13,
            width = ReproNative.WIDTH_V7, height = ReproNative.HEIGHT_V7
        )
        runByteArrayVariant("variant14", outDir, ReproNative::runVariant14)

        Log.i(tag, "all variants finished")
        finish()
    }

    private fun runByteArrayVariant(
        name: String,
        outDir: File,
        fn: (ByteArray) -> String,
        width: Int = ReproNative.WIDTH,
        height: Int = ReproNative.HEIGHT,
    ) {
        val pixels = ByteArray(width * height * 4)
        val summary = ReproNative.parse(fn(pixels))
        Log.i(tag, "$name summary: success=${summary.success} err='${summary.error}' " +
                "center=${summary.center.toList()} corner=${summary.corner.toList()}")
        if (!summary.success) return

        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
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
