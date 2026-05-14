package com.example.swsrepro

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Hard pass/fail assertion for the Goodnotes Android snapshot readback shape.
 *
 * Native code renders four opaque quadrants into an immutable RGBA8 texture,
 * then mirrors the snapshot recorder: source texture attached to
 * READ_FRAMEBUFFER, Y-flipped glBlitFramebuffer into an RGBA8 renderbuffer,
 * glReadPixels into CPU memory.
 */
@RunWith(AndroidJUnit4::class)
class ReproTest {

    /** Returns the RGBA bytes of `pixels` at `(x, y)` as `[r, g, b, a]` in 0..255. */
    private fun pixelAt(pixels: ByteArray, x: Int, y: Int): IntArray {
        val offset = (y * ReproNative.WIDTH + x) * 4
        return IntArray(4) { pixels[offset + it].toInt() and 0xFF }
    }

    @Test
    fun flippedFramebufferBlitPreservesPixels() {
        val pixels = ByteArray(ReproNative.WIDTH * ReproNative.HEIGHT * 4)
        val summary = ReproNative.parse(ReproNative.runTest(pixels))
        assertTrue(
            "native runTest reported failure: '${summary.error}'",
            summary.success,
        )

        val row0Left = pixelAt(pixels, 0, 0)
        assertArrayEquals(
            "row0-left should contain source top-left after flipped blit",
            intArrayOf(0, 0, 255, 255),
            row0Left,
        )

        val row0Right = pixelAt(pixels, ReproNative.WIDTH - 1, 0)
        assertArrayEquals(
            "row0-right should contain source top-right after flipped blit",
            intArrayOf(255, 255, 255, 255),
            row0Right,
        )

        val lastRowLeft = pixelAt(pixels, 0, ReproNative.HEIGHT - 1)
        assertArrayEquals(
            "lastrow-left should contain source bottom-left after flipped blit",
            intArrayOf(255, 0, 0, 255),
            lastRowLeft,
        )

        val lastRowRight = pixelAt(pixels, ReproNative.WIDTH - 1, ReproNative.HEIGHT - 1)
        assertArrayEquals(
            "lastrow-right should contain source bottom-right after flipped blit",
            intArrayOf(0, 255, 0, 255),
            lastRowRight,
        )
    }
}
