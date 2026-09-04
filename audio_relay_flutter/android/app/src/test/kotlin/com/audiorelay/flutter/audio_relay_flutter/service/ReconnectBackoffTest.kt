package com.audiorelay.flutter.audio_relay_flutter.service

import org.junit.Assert.assertEquals
import org.junit.Test

class ReconnectBackoffTest {

    @Test
    fun `initial attempt returns 1000ms`() {
        assertEquals(1000L, ReconnectBackoff.delayMsFor(0))
    }

    @Test
    fun `attempts double sequentially up to ceiling`() {
        assertEquals(1000L, ReconnectBackoff.delayMsFor(0))
        assertEquals(2000L, ReconnectBackoff.delayMsFor(1))
        assertEquals(4000L, ReconnectBackoff.delayMsFor(2))
        assertEquals(8000L, ReconnectBackoff.delayMsFor(3))
        assertEquals(16000L, ReconnectBackoff.delayMsFor(4))
    }

    @Test
    fun `attempts at or above shift ceiling cap at 30000ms`() {
        // 1000L shl 5 is 32000L, which is capped at 30000L
        assertEquals(30000L, ReconnectBackoff.delayMsFor(5))
        assertEquals(30000L, ReconnectBackoff.delayMsFor(6))
        assertEquals(30000L, ReconnectBackoff.delayMsFor(10))
        assertEquals(30000L, ReconnectBackoff.delayMsFor(100))
    }

    @Test
    fun `negative attempt clamps to 0 (1000ms)`() {
        assertEquals(1000L, ReconnectBackoff.delayMsFor(-1))
        assertEquals(1000L, ReconnectBackoff.delayMsFor(-10))
    }
}
