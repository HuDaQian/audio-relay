package com.audiorelay.flutter.audio_relay_flutter.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import android.os.Process
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Captures microphone audio using [AudioRecord] at 48kHz 16-bit PCM.
 * Automatically engages hardware [AcousticEchoCanceler] and [NoiseSuppressor] when available.
 * Delivers PCM chunks (typically 10-20ms) via a high-performance callback.
 *
 * NOTE: In one-way "microphone mode" the phone plays nothing, so
 * [AcousticEchoCanceler] has no far-end reference signal and cannot actually
 * cancel anything. It is attached only as a no-op best-effort. The real
 * far-end echo (the phone's mic picking up the computer's speakers) is
 * mitigated by telling the user to wear headphones — see the microphone-mode
 * guidance in the UI. AEC becomes meaningful only in future full-duplex mode.
 */
class AudioRecorder(
    private val sampleRate: Int = 48000,
    private val channels: Int = 1,
    private val onPcmChunk: (ByteArray, Int) -> Unit,
    private val onAudioLevel: ((Float) -> Unit)? = null,
) {
    companion object {
        private const val TAG = "AudioRecorder"
        // 10ms at 48kHz mono = 480 samples = 960 bytes
        private const val SAMPLES_PER_CHUNK = 480
    }

    private val isRecording = AtomicBoolean(false)
    private var recordJob: Job? = null
    private var audioRecord: AudioRecord? = null
    private var aec: AcousticEchoCanceler? = null
    private var ns: NoiseSuppressor? = null

    @SuppressLint("MissingPermission")
    fun start(scope: CoroutineScope) {
        if (isRecording.getAndSet(true)) return

        val channelConfig = if (channels == 1) {
            AudioFormat.CHANNEL_IN_MONO
        } else {
            AudioFormat.CHANNEL_IN_STEREO
        }
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufferSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        val chunkByteSize = SAMPLES_PER_CHUNK * channels * 2
        val bufferSize = maxOf(minBufferSize * 2, chunkByteSize * 4)

        try {
            // Prefer VOICE_COMMUNICATION for voice/conferencing (applies hardware AEC & AGC)
            var record: AudioRecord? = null
            try {
                record = AudioRecord(
                    MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed to create AudioRecord with VOICE_COMMUNICATION, falling back to MIC", e)
            }

            if (record == null || record.state != AudioRecord.STATE_INITIALIZED) {
                record?.release()
                record = AudioRecord(
                    MediaRecorder.AudioSource.MIC,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize
                )
            }

            if (record.state != AudioRecord.STATE_INITIALIZED) {
                record.release()
                isRecording.set(false)
                Log.e(TAG, "AudioRecord initialization failed")
                return
            }

            val sessionId = record.audioSessionId
            // Note: Hardware AcousticEchoCanceler (AEC) cancels acoustic loopback between phone's own
            // speaker playback and microphone. In Phase 1 (single-direction mic), phone does not play audio,
            // so AEC acts as a no-op placeholder prepared for Phase 2 full-duplex.
            // External PC speaker echo is prevented via PC headphones (guided in UI).
            if (AcousticEchoCanceler.isAvailable()) {
                try {
                    aec = AcousticEchoCanceler.create(sessionId)?.apply {
                        enabled = true
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to enable AcousticEchoCanceler", e)
                }
            }

            if (NoiseSuppressor.isAvailable()) {
                try {
                    ns = NoiseSuppressor.create(sessionId)?.apply {
                        enabled = true
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to enable NoiseSuppressor", e)
                }
            }

            audioRecord = record
            record.startRecording()

            recordJob = scope.launch(Dispatchers.IO) {
                Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
                val readBuffer = ByteArray(chunkByteSize)
                var levelTick = 0

                try {
                    while (isActive && isRecording.get()) {
                        var bytesReadTotal = 0
                        while (bytesReadTotal < chunkByteSize && isActive && isRecording.get()) {
                            val read = record.read(
                                readBuffer,
                                bytesReadTotal,
                                chunkByteSize - bytesReadTotal
                            )
                            if (read > 0) {
                                bytesReadTotal += read
                            } else if (read < 0) {
                                Log.w(TAG, "AudioRecord read error: $read")
                                break
                            }
                        }

                        if (bytesReadTotal > 0) {
                            onPcmChunk(readBuffer, bytesReadTotal)
                            levelTick++
                            if (levelTick % 5 == 0) { // ~50ms interval for UI level updates
                                onAudioLevel?.invoke(AudioLevel.fromPcm16(readBuffer, bytesReadTotal))
                            }
                        }
                    }
                } catch (e: CancellationException) {
                    // Normal coroutine cancellation
                } catch (e: Exception) {
                    Log.e(TAG, "Error in AudioRecord loop", e)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AudioRecorder", e)
            stop()
        }
    }

    fun stop() {
        if (!isRecording.getAndSet(false)) return
        recordJob?.cancel()
        recordJob = null

        try {
            audioRecord?.apply {
                if (recordingState == AudioRecord.RECORDSTATE_RECORDING) {
                    stop()
                }
                release()
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error releasing AudioRecord", e)
        }
        audioRecord = null

        try {
            aec?.release()
            ns?.release()
        } catch (e: Exception) {
            Log.w(TAG, "Error releasing audio effects", e)
        }
        aec = null
        ns = null
    }
}
