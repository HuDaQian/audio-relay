#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functional>
#include <vector>
#include <thread>
#include <atomic>

namespace audio_relay {

class WasapiCapture {
public:
    using AudioChunkCallback = std::function<void(const std::vector<uint8_t>& pcm_s16le)>;

    WasapiCapture();
    ~WasapiCapture();

    bool Start(AudioChunkCallback callback);
    void Stop();
    bool IsRunning() const { return is_running_.load(); }

private:
    void CaptureLoop();

    AudioChunkCallback callback_;
    std::thread worker_thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<bool> should_stop_{false};

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    WAVEFORMATEX* mix_format_ = nullptr;
};

} // namespace audio_relay
