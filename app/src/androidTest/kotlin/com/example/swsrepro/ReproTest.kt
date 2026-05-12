package com.example.swsrepro

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Hard pass/fail assertion of the SwiftShader render bug.
 *
 * Sampling an immutable, single-level RGBA8 texture filled with opaque red
 * MUST return opaque red (per GLES3 §8.17 — immutable textures are always
 * complete). On the Android emulator:
 *
 *   - `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan):  test passes.
 *   - `-gpu swiftshader`: test fails with red pixels coming back as (0, 0, 0, 0).
 *
 * The failure on swiftshader is the bug being demonstrated. See ../README.md.
 */
@RunWith(AndroidJUnit4::class)
class ReproTest {

    /** Returns the RGBA bytes of `pixels` at `(x, y)` as `[r, g, b, a]` in 0..255. */
    private fun pixelAt(pixels: ByteArray, x: Int, y: Int): IntArray {
        val offset = (y * ReproNative.WIDTH + x) * 4
        return IntArray(4) { pixels[offset + it].toInt() and 0xFF }
    }

    @Test
    fun immutableTextureSamplesAsRed() {
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(ReproNative.runTest(pixels))
        assertTrue(
            "native runTest reported failure: '${summary.error}'",
            summary.success,
        )

        val expected = intArrayOf(255, 0, 0, 255)

        // (0, 0) — first pixel of the readback buffer. Inside the textured
        // quad (which covers the full NDC viewport), so it must sample to
        // opaque red.
        val first = pixelAt(pixels, 0, 0)
        assertArrayEquals(
            "pixel (0, 0): expected ${expected.toList()}, got ${first.toList()}",
            expected,
            first,
        )

        // Center of the 256x256 framebuffer. Same expectation.
        val center = pixelAt(pixels, ReproNative.WIDTH / 2, ReproNative.HEIGHT / 2)
        assertArrayEquals(
            "pixel (${ReproNative.WIDTH / 2}, ${ReproNative.HEIGHT / 2}) " +
                "(GLES3 §8.17 says the immutable texture is complete; " +
                "expected ${expected.toList()}, got ${center.toList()})",
            expected,
            center,
        )
    }
}
