package com.audiorelay.flutter.audio_relay_flutter.network

import android.os.SystemClock
import android.util.Log
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

/**
 * Encapsulates PCM audio into encrypted AudioPackets and sends them to the desktop
 * over UDP (Wi-Fi) or TCP (USB ADB).
 */
class MicAudioSender(
    private val sessionKey: ByteArray,
    private val sessionId: ByteArray,
    private val targetAddress: InetAddress,
    private val targetUdpPort: Int = 45108,
    private val tcpOutputStream: OutputStream? = null,
    private val sampleRateHz: Int = 48000,
    private val channels: Int = 1,
) {
    companion object {
        private const val TAG = "MicAudioSender"
    }

    private var sequence: UInt = 0u
    private val startTimeMs = SystemClock.elapsedRealtime()
    private val udpSocket: DatagramSocket? = if (tcpOutputStream == null) DatagramSocket() else null
    private val headerBuffer = ByteArray(AudioPacket.HEADER_LEN)

    fun sendChunk(pcm: ByteArray, length: Int) {
        sequence++
        val timestampMs = (SystemClock.elapsedRealtime() - startTimeMs).toUInt()

        AudioPacket.encodeHeader(
            sequence = sequence,
            timestampMs = timestampMs,
            sampleRateHz = sampleRateHz,
            channels = channels,
            dest = headerBuffer,
            offset = 0,
        )

        val encryptedPayload: ByteArray
        try {
            encryptedPayload = Crypto.encryptPayload(
                key = sessionKey,
                sessionId = sessionId,
                sequence = sequence,
                headerAad = headerBuffer,
                plaintext = pcm,
                plaintextOffset = 0,
                plaintextLength = length,
            )
        } catch (e: Exception) {
            Log.w(TAG, "Failed to encrypt mic audio chunk", e)
            return
        }

        val datagramSize = AudioPacket.HEADER_LEN + encryptedPayload.size
        val packetBytes = ByteArray(datagramSize)
        System.arraycopy(headerBuffer, 0, packetBytes, 0, AudioPacket.HEADER_LEN)
        System.arraycopy(encryptedPayload, 0, packetBytes, AudioPacket.HEADER_LEN, encryptedPayload.size)

        if (tcpOutputStream != null) {
            // USB mode: send 2-byte big-endian length prefix followed by packet bytes
            try {
                val lenPrefix = byteArrayOf(
                    ((datagramSize ushr 8) and 0xFF).toByte(),
                    (datagramSize and 0xFF).toByte()
                )
                synchronized(tcpOutputStream) {
                    tcpOutputStream.write(lenPrefix)
                    tcpOutputStream.write(packetBytes)
                    tcpOutputStream.flush()
                }
            } catch (e: Exception) {
                Log.w(TAG, "TCP mic send error: ${e.message}")
            }
        } else if (udpSocket != null) {
            // Wi-Fi UDP mode
            try {
                val packet = DatagramPacket(
                    packetBytes,
                    packetBytes.size,
                    targetAddress,
                    targetUdpPort
                )
                udpSocket.send(packet)
            } catch (e: Exception) {
                Log.w(TAG, "UDP mic send error: ${e.message}")
            }
        }
    }

    fun close() {
        try {
            udpSocket?.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing udp socket", e)
        }
    }
}
