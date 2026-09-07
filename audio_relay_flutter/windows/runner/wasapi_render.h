#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "jitter_buffer.h"

namespace audio_relay {

struct AudioOutputDeviceInfo {
    std::string id;
    std::string name;
    bool is_virtual = false;
    bool is_default = false;
};

class WasapiRender {
public:
    WasapiRender();
    ~WasapiRender();

    static std::vector<AudioOutputDeviceInfo> EnumerateOutputDevices();

    bool Start(const std::string& device_id = "");
    void Stop();
    bool IsRunning() const { return is_running_.load(); }

    // Enqueues one decoded 16-bit PCM packet to be rendered to audio output.
    // Downmix/resample is applied here (where the device mix format is known),
    // then the resulting mono frames are pushed into the jitter buffer.
    void WritePcmChunk(uint32_t sequence, const uint8_t* pcm, size_t size, int channels = 1, int sample_rate = 48000);

private:
    void RenderLoop(std::string device_id);

    std::atomic<bool> is_running_{false};
    std::atomic<bool> should_stop_{false};
    std::thread worker_thread_;
    HANDLE stop_event_{nullptr};

    JitterBuffer jitter_;

    IMMDeviceEnumerator* enumerator_{nullptr};
    IMMDevice* device_{nullptr};
    IAudioClient* audio_client_{nullptr};
    IAudioRenderClient* render_client_{nullptr};
    WAVEFORMATEX* mix_format_{nullptr};
};

} // namespace audio_relay
