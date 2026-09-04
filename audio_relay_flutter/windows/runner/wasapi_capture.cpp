#include "wasapi_capture.h"
#include <avrt.h>
#include <iostream>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace audio_relay {

WasapiCapture::WasapiCapture() {
    stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

WasapiCapture::~WasapiCapture() {
    Stop();
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

bool WasapiCapture::Start(AudioChunkCallback callback) {
    if (is_running_.load()) {
        return true;
    }

    callback_ = std::move(callback);
    should_stop_.store(false);
    if (stop_event_) {
        ResetEvent(stop_event_);
    }

    worker_thread_ = std::thread(&WasapiCapture::CaptureLoop, this);
    return true;
}

void WasapiCapture::Stop() {
    should_stop_.store(true);
    if (stop_event_) {
        SetEvent(stop_event_);
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    is_running_.store(false);
}

void WasapiCapture::CaptureLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_initialized = SUCCEEDED(hr);

    // 1. Get default audio endpoint for playback
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) {
        if (co_initialized) CoUninitialize();
        return;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        enumerator_->Release();
        enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    // 2. Activate IAudioClient
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr)) {
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    // 3. Get mix format
    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    // 4. Initialize in shared loopback mode with event-driven callback
    // 20ms buffer duration
    REFERENCE_TIME hnsBufferDuration = 200000; // 20ms
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration,
        0,
        mix_format_,
        nullptr
    );

    if (FAILED(hr)) {
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    HANDLE hAudioEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!hAudioEvent) {
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    hr = audio_client_->SetEventHandle(hAudioEvent);
    if (FAILED(hr)) {
        CloseHandle(hAudioEvent);
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    // 5. Get capture client
    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr)) {
        CloseHandle(hAudioEvent);
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    // 6. Set MMCSS priority for low-latency audio capture
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        CloseHandle(hAudioEvent);
        capture_client_->Release(); capture_client_ = nullptr;
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    is_running_.store(true);

    const int inSampleRate = mix_format_->nSamplesPerSec;
    const int inChannels = mix_format_->nChannels;
    const bool isFloat = (mix_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        ((mix_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    // Accumulate output PCM samples to form ~5ms - 10ms chunks (240-480 samples @ 48kHz = 960-1920 bytes)
    std::vector<uint8_t> pcm_accum;
    pcm_accum.reserve(4096);

    HANDLE wait_handles[2] = { stop_event_, hAudioEvent };

    while (!should_stop_.load()) {
        DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, 1000);
        if (wait_res == WAIT_OBJECT_0) {
            // stop_event_ signaled
            break;
        }
        if (wait_res != WAIT_OBJECT_0 + 1) {
            // Timeout or error; continue checking should_stop_
            continue;
        }

        UINT32 packetLength = 0;
        hr = capture_client_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength > 0 && !should_stop_.load()) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = capture_client_->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFrames > 0) {
                // Convert current frames to 48000Hz 16-bit Stereo PCM
                // Step A: Extract Left and Right float samples
                std::vector<float> left_samples(numFrames);
                std::vector<float> right_samples(numFrames);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(left_samples.begin(), left_samples.end(), 0.0f);
                    std::fill(right_samples.begin(), right_samples.end(), 0.0f);
                } else if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 f = 0; f < numFrames; f++) {
                        left_samples[f] = fData[f * inChannels];
                        right_samples[f] = (inChannels > 1) ? fData[f * inChannels + 1] : fData[f * inChannels];
                    }
                } else if (mix_format_->wBitsPerSample == 16) {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 f = 0; f < numFrames; f++) {
                        left_samples[f] = sData[f * inChannels] / 32768.0f;
                        right_samples[f] = (inChannels > 1) ? (sData[f * inChannels + 1] / 32768.0f) : (sData[f * inChannels] / 32768.0f);
                    }
                }

                // Step B: Resample to 48000Hz if necessary
                UINT32 outFrames = (UINT32)((uint64_t)numFrames * 48000 / inSampleRate);
                if (outFrames == 0 && numFrames > 0) outFrames = 1;

                for (UINT32 i = 0; i < outFrames; i++) {
                    float srcIdx = (float)i * (float)inSampleRate / 48000.0f;
                    UINT32 idx0 = (UINT32)srcIdx;
                    UINT32 idx1 = std::min(idx0 + 1, numFrames - 1);
                    float frac = srcIdx - (float)idx0;

                    float l = (1.0f - frac) * left_samples[idx0] + frac * left_samples[idx1];
                    float r = (1.0f - frac) * right_samples[idx0] + frac * right_samples[idx1];

                    int16_t sL = (int16_t)std::clamp(l * 32767.0f, -32768.0f, 32767.0f);
                    int16_t sR = (int16_t)std::clamp(r * 32767.0f, -32768.0f, 32767.0f);

                    pcm_accum.push_back((uint8_t)(sL & 0xFF));
                    pcm_accum.push_back((uint8_t)((sL >> 8) & 0xFF));
                    pcm_accum.push_back((uint8_t)(sR & 0xFF));
                    pcm_accum.push_back((uint8_t)((sR >> 8) & 0xFF));
                }

                // Emit audio chunks if we have >= 960 bytes (240 stereo frames = 5ms at 48kHz)
                while (pcm_accum.size() >= 960) {
                    size_t chunk_len = (pcm_accum.size() >= 1920) ? 1920 : 960;
                    std::vector<uint8_t> chunk(pcm_accum.begin(), pcm_accum.begin() + chunk_len);
                    pcm_accum.erase(pcm_accum.begin(), pcm_accum.begin() + chunk_len);
                    if (callback_) {
                        callback_(chunk);
                    }
                }
            }

            capture_client_->ReleaseBuffer(numFrames);
            hr = capture_client_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }

    audio_client_->Stop();
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    CloseHandle(hAudioEvent);

    capture_client_->Release(); capture_client_ = nullptr;
    CoTaskMemFree(mix_format_); mix_format_ = nullptr;
    audio_client_->Release(); audio_client_ = nullptr;
    device_->Release(); device_ = nullptr;
    enumerator_->Release(); enumerator_ = nullptr;

    if (co_initialized) CoUninitialize();
    is_running_.store(false);
}

} // namespace audio_relay
