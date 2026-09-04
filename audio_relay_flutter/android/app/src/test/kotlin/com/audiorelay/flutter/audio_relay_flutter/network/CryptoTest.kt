package com.audiorelay.flutter.audio_relay_flutter.network

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

class CryptoTest {

    @Test
    fun `toHex and hexToBytes roundtrip`() {
        val original = byteArrayOf(0x00, 0x01, 0x0a, 0x0f, 0x10, 0x7f, 0x80.toByte(), 0xff.toByte())
        val hex = Crypto.toHex(original)
        assertEquals("00010a0f107f80ff", hex)
        val decoded = Crypto.hexToBytes(hex)
        assertArrayEquals(original, decoded)
    }

    @Test
    fun `empty byte array hex encoding and decoding`() {
        val empty = byteArrayOf()
        val hex = Crypto.toHex(empty)
        assertEquals("", hex)
        assertArrayEquals(empty, Crypto.hexToBytes(hex))
    }

    @Test
    fun `computePairProof computes valid HMAC-SHA256 hex`() {
        val code = "654321"
        val phoneId = "phone-abc-123"
        val nonce = "fedcba9876543210"

        val proof = Crypto.computePairProof(code, phoneId, nonce)
        assertNotNull(proof)
        assertEquals(64, proof.length) // 32 bytes hex-encoded = 64 chars

        // Independently verify HMAC-SHA256
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(code.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        mac.update((phoneId + nonce).toByteArray(Charsets.UTF_8))
        val expected = mac.doFinal().joinToString("") { "%02x".format(it) }

        assertEquals(expected, proof)
    }

    @Test
    fun `computeRepairProof computes valid HMAC-SHA256 with key`() {
        val key = ByteArray(32) { it.toByte() }
        val deviceId = "device-xyz-789"
        val nonce = "1122334455667788"

        val proof = Crypto.computeRepairProof(key, deviceId, nonce)
        assertEquals(64, proof.length)

        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        mac.update((deviceId + nonce).toByteArray(Charsets.UTF_8))
        val expected = mac.doFinal().joinToString("") { "%02x".format(it) }

        assertEquals(expected, proof)
    }

    @Test
    fun `deriveSessionKey generates 32-byte key and is deterministic`() {
        val code = "123456"
        val phoneId = "phone-1"
        val laptopId = "laptop-1"

        val key1 = Crypto.deriveSessionKey(code, phoneId, laptopId)
        val key2 = Crypto.deriveSessionKey(code, phoneId, laptopId)

        assertEquals(32, key1.size)
        assertArrayEquals(key1, key2)

        // Changing any input must produce a different key
        val keyDifferentCode = Crypto.deriveSessionKey("123457", phoneId, laptopId)
        assertFalse(key1.contentEquals(keyDifferentCode))

        val keyDifferentPhone = Crypto.deriveSessionKey(code, "phone-2", laptopId)
        assertFalse(key1.contentEquals(keyDifferentPhone))

        val keyDifferentLaptop = Crypto.deriveSessionKey(code, phoneId, "laptop-2")
        assertFalse(key1.contentEquals(keyDifferentLaptop))
    }

    @Test
    fun `randomSessionId produces 8-byte identifier`() {
        val s1 = Crypto.randomSessionId()
        val s2 = Crypto.randomSessionId()
        assertEquals(8, s1.size)
        assertEquals(8, s2.size)
        // Two random session IDs should almost certainly not be identical
        assertFalse(s1.contentEquals(s2))
    }
}
