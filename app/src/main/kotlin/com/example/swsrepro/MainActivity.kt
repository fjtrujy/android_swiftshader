package com.example.swsrepro

import android.app.Activity
import android.os.Bundle
import android.util.Log

class MainActivity : Activity() {
    private val tag = "swsrepro"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        runRepro()
        finish()
    }

    private fun runRepro() {
        val testPixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val testSummary = ReproNative.parse(ReproNative.runTest(testPixels))
        Log.i(
            tag,
            "test: success=${testSummary.success} err='${testSummary.error}' " +
                "center=${testSummary.center.toList()} corner=${testSummary.corner.toList()}",
        )
    }
}
