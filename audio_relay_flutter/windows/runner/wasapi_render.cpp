#include "wasapi_render.h"
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace audio_relay {

namespace {

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

bool ContainsCaseInsensitive(const std::string& str, const std::string& sub) {
    auto it = std::search(
        str.begin(), str.end(),
        sub.begin(), sub.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return (it != str.end());
}

} // namespace

WasapiRender::WasapiRender() {
    stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

WasapiRender::~WasapiRender() {
    Stop();
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

std::vector<AudioOutputDeviceInfo> WasapiRender::EnumerateOutputDevices() {
    std::vector<AudioOutputDeviceInfo> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_init = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        if (co_init) CoUninitialize();
        return devices;
    }

    IMMDevice* defaultDevice = nullptr;
    LPWSTR defaultId = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
        defaultDevice->GetId(&defaultId);
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(collection->Item(i, &dev)) && dev) {
                LPWSTR devId = nullptr;
                dev->GetId(&devId);
                std::string idStr = devId ? WideToUtf8(devId) : "";

                std::string nameStr = "Unknown Device";
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
                        nameStr = WideToUtf8(var.pwszVal);
                    }
                    PropVariantClear(&var);
                    props->Release();
                }

                AudioOutputDeviceInfo info;
                info.id = idStr;
                info.name = nameStr;
                info.is_virtual = ContainsCaseInsensitive(nameStr, "cable") ||
                                  ContainsCaseInsensitive(nameStr, "vb-audio") ||
                                  ContainsCaseInsensitive(nameStr, "virtual") ||
                                  ContainsCaseInsensitive(nameStr, "voicemeeter");
                info.is_default = (defaultId && devId && wcscmp(defaultId, devId) == 0);

                devices.push_back(info);

                if (devId) CoTaskMemFree(devId);
                dev->Release();
            }
        }
        collection->Release();
    }

    if (defaultId) CoTaskMemFree(defaultId);
    enumerator->Release();
    if (co_init) CoUninitialize();

    return devices;
}

bool WasapiRender::Start(const std::string& device_id) {
    if (is_running_.load()) return true;

    should_stop_.store(false);
    if (stop_event_) {
        ResetEvent(stop_event_);
    }

    jitter_.reset();

    worker_thread_ = std::thread(&WasapiRender::RenderLoop, this, device_id);
    return true;
}

void WasapiRender::Stop() {
    should_stop_.store(true);
    if (stop_event_) {
        SetEvent(stop_event_);
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    is_running_.store(false);
}

void WasapiRender::WritePcmChunk(uint32_t sequence, const uint8_t* pcm, size_t size, int channels, int sample_rate) {
    if (!is_running_.load() || size == 0) return;

    // Convert incoming 16-bit PCM bytes to mono int16 samples
    size_t sample_count = size / 2;
    const int16_t* in_samples = reinterpret_cast<const int16_t*>(pcm);

    std::vector<int16_t> mono_samples;
    mono_samples.reserve(sample_count / std::max(1, channels));

    if (channels <= 1) {
        mono_samples.assign(in_samples, in_samples + sample_count);
    } else {
        // Downmix stereo to mono
        for (size_t i = 0; i + 1 < sample_count; i += 2) {
            int32_t mixed = ((int32_t)in_samples[i] + (int32_t)in_samples[i + 1]) / 2;
            mono_samples.push_back((int16_t)mixed);
        }
    }

    // Resample if incoming sample rate does not match device mix format rate
    int target_rate = 48000;
    if (mix_format_ && mix_format_->nSamplesPerSec > 0) {
        target_rate = mix_format_->nSamplesPerSec;
    }

    std::vector<int16_t> resampled;
    if (sample_rate > 0 && sample_rate != target_rate && !mono_samples.empty()) {
        double ratio = (double)target_rate / (double)sample_rate;
        size_t out_count = (size_t)(mono_samples.size() * ratio);
        resampled.resize(out_count);
        for (size_t i = 0; i < out_count; i++) {
            double src_idx = i / ratio;
            size_t idx0 = (size_t)src_idx;
            size_t idx1 = std::min(idx0 + 1, mono_samples.size() - 1);
            double frac = src_idx - idx0;
            double interpolated = mono_samples[idx0] * (1.0 - frac) + mono_samples[idx1] * frac;
            resampled[i] = (int16_t)std::clamp(interpolated, -32768.0, 32767.0);
        }
    } else {
        resampled = std::move(mono_samples);
    }

    jitter_.push(sequence, resampled.data(), resampled.size());
}

void WasapiRender::RenderLoop(std::string device_id) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_initialized = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) {
        if (co_initialized) CoUninitialize();
        return;
    }

    if (!device_id.empty()) {
        std::wstring wide_id = Utf8ToWide(device_id);
        hr = enumerator_->GetDevice(wide_id.c_str(), &device_);
    }

    if (FAILED(hr) || !device_) {
        // Auto-select virtual device if present
        auto devList = EnumerateOutputDevices();
        std::string chosenId = "";
        for (const auto& d : devList) {
            if (d.is_virtual) {
                chosenId = d.id;
                break;
            }
        }
        if (!chosenId.empty()) {
            std::wstring wide_id = Utf8ToWide(chosenId);
            hr = enumerator_->GetDevice(wide_id.c_str(), &device_);
        }
    }

    if (FAILED(hr) || !device_) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    }

    if (FAILED(hr) || !device_) {
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr) || !audio_client_) {
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    REFERENCE_TIME hnsBufferDuration = 200000; // 20ms
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
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

    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_client_);
    if (FAILED(hr) || !render_client_) {
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    UINT32 bufferFrameCount = 0;
    audio_client_->GetBufferSize(&bufferFrameCount);

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        render_client_->Release(); render_client_ = nullptr;
        CoTaskMemFree(mix_format_); mix_format_ = nullptr;
        audio_client_->Release(); audio_client_ = nullptr;
        device_->Release(); device_ = nullptr;
        enumerator_->Release(); enumerator_ = nullptr;
        if (co_initialized) CoUninitialize();
        return;
    }

    is_running_.store(true);

    const int outSampleRate = mix_format_->nSamplesPerSec;
    const int outChannels = mix_format_->nChannels;
    const bool isFloat = (mix_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        ((mix_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    while (!should_stop_.load()) {
        UINT32 padding = 0;
        hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 framesAvailable = bufferFrameCount - padding;
        if (framesAvailable == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        BYTE* pData = nullptr;
        hr = render_client_->GetBuffer(framesAvailable, &pData);
        if (FAILED(hr)) break;

        // Zero-filled: frames `pop` doesn't fill stay silent (pre-buffer /
        // concealment / underflow).
        std::vector<int16_t> samplesToPlay(framesAvailable, 0);
        jitter_.pop(samplesToPlay.data(), framesAvailable);

        // Fill buffer
        if (isFloat) {
            float* fOut = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < framesAvailable; i++) {
                float val = (i < samplesToPlay.size()) ? (samplesToPlay[i] / 32768.0f) : 0.0f;
                for (int ch = 0; ch < outChannels; ch++) {
                    fOut[i * outChannels + ch] = val;
                }
            }
        } else if (mix_format_->wBitsPerSample == 16) {
            int16_t* sOut = reinterpret_cast<int16_t*>(pData);
            for (UINT32 i = 0; i < framesAvailable; i++) {
                int16_t val = (i < samplesToPlay.size()) ? samplesToPlay[i] : 0;
                for (int ch = 0; ch < outChannels; ch++) {
                    sOut[i * outChannels + ch] = val;
                }
            }
        }

        render_client_->ReleaseBuffer(framesAvailable, 0);
    }

    audio_client_->Stop();
    render_client_->Release(); render_client_ = nullptr;
    CoTaskMemFree(mix_format_); mix_format_ = nullptr;
    audio_client_->Release(); audio_client_ = nullptr;
    device_->Release(); device_ = nullptr;
    enumerator_->Release(); enumerator_ = nullptr;
    if (co_initialized) CoUninitialize();
    is_running_.store(false);
}

} // namespace audio_relay
