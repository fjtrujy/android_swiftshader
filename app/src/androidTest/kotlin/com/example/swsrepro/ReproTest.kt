package com.example.swsrepro

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Hard pass/fail assertion for the tiny valid GLES3 repro.
 *
 * Native code draws a solid red rectangle into an EGL pbuffer default
 * framebuffer using a gl_VertexID-generated triangle strip, then reads it
 * directly with glReadPixels.
 */
@RunWith(AndroidJUnit4::class)
class ReproTest {

    /** Returns the RGBA bytes of `pixels` at `(x, y)` as `[r, g, b, a]` in 0..255. */
    private fun pixelAt(pixels: ByteArray, x: Int, y: Int): IntArray {
        val offset = (y * ReproNative.WIDTH + x) * 4
        return IntArray(4) { pixels[offset + it].toInt() and 0xFF }
    }

    @Test
    fun validVertexIdQuadDrawsRedPixels() {
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(ReproNative.runTest(pixels))
        assertTrue(
            "native runTest reported failure: '${summary.error}'",
            summary.success,
        )

        val expected = intArrayOf(255, 0, 0, 255)

        val first = pixelAt(pixels, 0, 0)
        assertArrayEquals(
            "pixel (0, 0): expected ${expected.toList()}, got ${first.toList()}",
            expected,
            first,
        )

        val center = pixelAt(pixels, ReproNative.WIDTH / 2, ReproNative.HEIGHT / 2)
        assertArrayEquals(
            "pixel (${ReproNative.WIDTH / 2}, ${ReproNative.HEIGHT / 2}): " +
                "expected ${expected.toList()}, got ${center.toList()}",
            expected,
            center,
        )
    }
}
