package com.audiorelay.flutter.audio_relay_flutter.network

import android.os.SystemClock
import android.util.Log
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Encapsulates PCM audio into encrypted AudioPackets and sends them to the desktop
 * over UDP (Wi-Fi) or TCP (USB ADB).
 *
 * The UDP path sends synchronously (DatagramSocket.send is non-blocking), but the
 * TCP path is deliberately decoupled from the capture loop: a bounded queue plus a
 * dedicated sender thread means a stalled USB connection can never block the
 * AudioRecord loop — frames are dropped instead, and audio recovers when the link
 * clears.
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

        // ~160ms of audio at 10ms packets. Big enough to absorb a short stall,
        // small enough to keep latency bounded and never block the capture loop.
        private const val MAX_QUEUED_PACKETS = 16
        private const val TCP_POLL_TIMEOUT_MS = 500L
    }

    private var sequence: UInt = 0u
    private val startTimeMs = SystemClock.elapsedRealtime()
    private val udpSocket: DatagramSocket? = if (tcpOutputStream == null) DatagramSocket() else null
    private val headerBuffer = ByteArray(AudioPacket.HEADER_LEN)

    @Volatile
    private var closed = false

    private val tcpSendQueue: LinkedBlockingQueue<ByteArray>? =
        if (tcpOutputStream != null) LinkedBlockingQueue<ByteArray>(MAX_QUEUED_PACKETS) else null

    private val tcpSenderThread: Thread? = if (tcpOutputStream != null) {
        Thread({ runTcpSender() }, "MicAudioTcpSender").apply {
            isDaemon = true
            start()
        }
    } else {
        null
    }

    fun sendChunk(pcm: ByteArray, length: Int) {
        if (closed) return

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

        val queue = tcpSendQueue
        if (queue != null) {
            // Non-blocking: drop this frame if the TCP sender is falling behind.
            if (!queue.offer(packetBytes)) {
                Log.w(TAG, "mic send queue full; dropping frame")
            }
        } else {
            udpSocket?.let { socket ->
                try {
                    val packet = DatagramPacket(
                        packetBytes,
                        packetBytes.size,
                        targetAddress,
                        targetUdpPort,
                    )
                    socket.send(packet)
                } catch (e: Exception) {
                    Log.w(TAG, "UDP mic send error: ${e.message}")
                }
            }
        }
    }

    private fun runTcpSender() {
        val stream = tcpOutputStream ?: return
        val queue = tcpSendQueue ?: return

        while (!closed) {
            val packet = try {
                queue.poll(TCP_POLL_TIMEOUT_MS, TimeUnit.MILLISECONDS) ?: continue
            } catch (e: InterruptedException) {
                return
            }

            try {
                synchronized(stream) {
                    val lenPrefix = byteArrayOf(
                        ((packet.size ushr 8) and 0xFF).toByte(),
                        (packet.size and 0xFF).toByte(),
                    )
                    stream.write(lenPrefix)
                    stream.write(packet)
                    stream.flush()
                }
            } catch (e: Exception) {
                // Drop the backlog so we never replay stale audio after a stall;
                // the desktop jitter buffer already treats these as lost packets.
                Log.w(TAG, "TCP mic send error: ${e.message}")
                queue.clear()
            }
        }
    }

    fun close() {
        closed = true
        tcpSenderThread?.interrupt()
        try {
            udpSocket?.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing udp socket", e)
        }
    }
}
