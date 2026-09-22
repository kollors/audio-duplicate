#include "audio_engine.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace ad {

namespace {

std::wstring HrText(const wchar_t* prefix, HRESULT hr) {
    return std::format(L"{} (HRESULT 0x{:08X})", prefix, static_cast<unsigned>(hr));
}

bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool IsPcmFormat(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    return false;
}

float ReadSample(const uint8_t* p, int bits, bool isFloat) {
    if (isFloat && bits == 32) {
        float v;
        std::memcpy(&v, p, sizeof(v));
        return std::clamp(v, -1.0f, 1.0f);
    }
    if (bits == 16) {
        int16_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 32768.0f;
    }
    if (bits == 24) {
        int32_t v = static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);
        if (v & 0x00800000) v |= 0xFF000000;
        return static_cast<float>(v) / 8388608.0f;
    }
    if (bits == 32) {
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 2147483648.0f;
    }
    return 0.0f;
}

void WriteSample(uint8_t* p, int bits, bool isFloat, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    if (isFloat && bits == 32) {
        std::memcpy(p, &value, sizeof(value));
        return;
    }
    if (bits == 16) {
        const int16_t v = static_cast<int16_t>(value * 32767.0f);
        std::memcpy(p, &v, sizeof(v));
        return;
    }
    if (bits == 24) {
        const int32_t v = static_cast<int32_t>(value * 8388607.0f);
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        return;
    }
    if (bits == 32) {
        const int32_t v = static_cast<int32_t>(value * 2147483647.0f);
        std::memcpy(p, &v, sizeof(v));
    }
}

std::vector<uint8_t> RouteAudio(const uint8_t* src,
                                UINT32 frames,
                                const WAVEFORMATEX* wf,
                                ChannelMode sourceMode,
                                ChannelMode outputMode,
                                bool silent) {
    const UINT32 channels = wf->nChannels;
    const int bits = wf->wBitsPerSample;
    const UINT32 bytesPerSample = bits / 8;
    const UINT32 frameBytes = wf->nBlockAlign;
    const bool isFloat = IsFloatFormat(wf);
    const bool isPcm = IsPcmFormat(wf);

    std::vector<uint8_t> out(static_cast<size_t>(frames) * frameBytes, 0);
    if (silent || !src) return out;
    if ((!isFloat && !isPcm) || channels == 0 || bytesPerSample == 0) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }

    for (UINT32 f = 0; f < frames; ++f) {
        const uint8_t* inFrame = src + static_cast<size_t>(f) * frameBytes;
        uint8_t* outFrame = out.data() + static_cast<size_t>(f) * frameBytes;

        const float left = ReadSample(inFrame, bits, isFloat);
        const float right = channels >= 2
            ? ReadSample(inFrame + bytesPerSample, bits, isFloat)
            : left;

        float srcL = left;
        float srcR = right;
        bool sourceIsMonoSelection = false;
        float selected = 0.0f;

        if (sourceMode == ChannelMode::Left) {
            selected = left;
            sourceIsMonoSelection = true;
        } else if (sourceMode == ChannelMode::Right) {
            selected = right;
            sourceIsMonoSelection = true;
        }

        float dstL = 0.0f;
        float dstR = 0.0f;

        if (sourceIsMonoSelection) {
            if (outputMode == ChannelMode::Stereo) {
                dstL = selected;
                dstR = selected;
            } else if (outputMode == ChannelMode::Left) {
                dstL = selected;
            } else {
                dstR = selected;
            }
        } else {
            if (outputMode == ChannelMode::Stereo) {
                dstL = srcL;
                dstR = srcR;
            } else {
                const float mono = (srcL + srcR) * 0.5f;
                if (outputMode == ChannelMode::Left) dstL = mono;
                else dstR = mono;
            }
        }

        WriteSample(outFrame, bits, isFloat, dstL);
        if (channels >= 2) WriteSample(outFrame + bytesPerSample, bits, isFloat, dstR);

        // For endpoints exposing >2 channels, leave remaining channels silent.
    }
    return out;
}

ComPtr<IMMDevice> FindDeviceById(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return {};
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDevice(id.c_str(), &dev))) return {};
    return dev;
}

} // namespace

struct AudioEngine::Renderer {
    std::wstring deviceId;
    ChannelMode mode = ChannelMode::Stereo;

    std::thread thread;
    std::atomic<bool> stop{false};

    std::mutex queueMutex;
    std::deque<std::vector<uint8_t>> queue;
    size_t queuedBytes = 0;

    std::mutex initMutex;
    std::condition_variable initCv;
    bool initDone = false;
    bool initOk = false;
    std::wstring initError;

    std::vector<uint8_t> formatBytes;
    UINT32 frameBytes = 0;

    ~Renderer() { Shutdown(); }

    bool Init(const WAVEFORMATEX* sourceFormat, std::wstring& error) {
        if (!sourceFormat) {
            error = L"Некорректный формат основного аудиоустройства.";
            return false;
        }

        const size_t formatSize = sizeof(WAVEFORMATEX) + sourceFormat->cbSize;
        formatBytes.resize(formatSize);
        std::memcpy(formatBytes.data(), sourceFormat, formatSize);
        frameBytes = sourceFormat->nBlockAlign;

        thread = std::thread([this] { Run(); });

        std::unique_lock lock(initMutex);
        initCv.wait(lock, [this] { return initDone; });
        if (!initOk) {
            error = initError;
            lock.unlock();
            Shutdown();
            return false;
        }
        return true;
    }

    void Push(std::vector<uint8_t> data) {
        if (data.empty() || stop.load()) return;
        std::scoped_lock lock(queueMutex);

        const auto* format = reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
        const size_t bytesPerSecond = format && format->nAvgBytesPerSec
            ? static_cast<size_t>(format->nAvgBytesPerSec)
            : static_cast<size_t>(frameBytes) * 48000;
        const size_t hardLimit = std::max<size_t>(bytesPerSecond / 2, frameBytes * 256);

        while (!queue.empty() && queuedBytes + data.size() > hardLimit) {
            queuedBytes -= queue.front().size();
            queue.pop_front();
        }
        queuedBytes += data.size();
        queue.push_back(std::move(data));
    }

    void SignalInit(bool ok, std::wstring error = {}) {
        {
            std::scoped_lock lock(initMutex);
            initOk = ok;
            initError = std::move(error);
            initDone = true;
        }
        initCv.notify_one();
    }

    void Run() {
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitCom = SUCCEEDED(comInit);
        if (FAILED(comInit) && comInit != RPC_E_CHANGED_MODE) {
            SignalInit(false, HrText(L"COM initialization failed", comInit));
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        ComPtr<IMMDevice> dev = FindDeviceById(deviceId);
        if (!dev) {
            SignalInit(false, L"Не удалось открыть дополнительное аудиоустройство.");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        ComPtr<IAudioClient> client;
        HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) {
            SignalInit(false, HrText(L"IAudioClient::Activate", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            SignalInit(false, L"Не удалось создать WASAPI event.");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        const auto* sourceFormat = reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, sourceFormat, nullptr);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            SignalInit(false, HrText(L"Не удалось открыть дополнительный выход", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            SignalInit(false, HrText(L"IAudioClient::SetEventHandle", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            SignalInit(false, HrText(L"IAudioClient::GetBufferSize", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        ComPtr<IAudioRenderClient> render;
        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            SignalInit(false, HrText(L"IAudioClient::GetService(render)", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            SignalInit(false, HrText(L"IAudioClient::Start(render)", hr));
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitCom) CoUninitialize();
            return;
        }

        SignalInit(true);

        std::vector<uint8_t> pending;
        size_t offset = 0;

        while (!stop.load()) {
            DWORD wr = WaitForSingleObject(eventHandle, 200);
            if (wr != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) continue;

            const UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
            if (!available) continue;

            BYTE* dst = nullptr;
            hr = render->GetBuffer(available, &dst);
            if (FAILED(hr)) continue;

            const size_t need = static_cast<size_t>(available) * frameBytes;
            size_t written = 0;
            while (written < need) {
                if (offset >= pending.size()) {
                    std::scoped_lock lock(queueMutex);
                    if (queue.empty()) break;
                    pending = std::move(queue.front());
                    queue.pop_front();
                    queuedBytes -= pending.size();
                    offset = 0;
                }

                const size_t chunk = std::min(need - written, pending.size() - offset);
                std::memcpy(dst + written, pending.data() + offset, chunk);
                written += chunk;
                offset += chunk;
            }

            if (written < need) std::memset(dst + written, 0, need - written);
            render->ReleaseBuffer(available, 0);
        }

        client->Stop();
        render.Reset();
        client.Reset();
        CloseHandle(eventHandle);

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (uninitCom) CoUninitialize();
    }

    void Shutdown() {
        stop.store(true);
        if (thread.joinable()) thread.join();
    }
};

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

std::vector<AudioDevice> AudioEngine::EnumerateRenderDevices() {
    std::vector<AudioDevice> result;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        if (uninit) CoUninitialize();
        return result;
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDev;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDev->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        if (uninit) CoUninitialize();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id) continue;
        AudioDevice item;
        item.id = id;
        item.isDefault = item.id == defaultId;
        CoTaskMemFree(id);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                item.name = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
        if (item.name.empty()) item.name = L"Аудиоустройство";
        result.push_back(std::move(item));
    }

    std::stable_sort(result.begin(), result.end(), [](const AudioDevice& a, const AudioDevice& b) {
        if (a.isDefault != b.isDefault) return a.isDefault > b.isDefault;
        return a.name < b.name;
    });

    if (uninit) CoUninitialize();
    return result;
}

bool AudioEngine::Start(const EngineConfig& config, std::wstring& error) {
    Stop();
    SetLastError(L"");
    if (config.sourceDeviceId.empty()) {
        error = L"Выберите основной выход.";
        return false;
    }
    if (config.outputs.empty()) {
        error = L"Добавьте хотя бы один дополнительный выход.";
        return false;
    }
    std::vector<std::wstring> usedOutputs;
    for (const auto& out : config.outputs) {
        if (out.deviceId.empty()) { error = L"Выберите устройство для каждого дополнительного выхода."; return false; }
        if (out.deviceId == config.sourceDeviceId) { error = L"Основной и дополнительный выход не должны быть одним устройством."; return false; }
        if (std::find(usedOutputs.begin(), usedOutputs.end(), out.deviceId) != usedOutputs.end()) {
            error = L"Одно дополнительное устройство выбрано несколько раз.";
            return false;
        }
        usedOutputs.push_back(out.deviceId);
    }

    stopRequested_.store(false);
    running_.store(true);
    captureThread_ = std::thread([this, config] { CaptureLoop(config); });
    return true;
}

void AudioEngine::Stop() {
    stopRequested_.store(true);
    if (captureThread_.joinable()) captureThread_.join();
    running_.store(false);
}

void AudioEngine::SetLastError(std::wstring text) {
    std::scoped_lock lock(errorMutex_);
    lastError_ = std::move(text);
}

std::wstring AudioEngine::LastError() const {
    std::scoped_lock lock(errorMutex_);
    return lastError_;
}

void AudioEngine::CaptureLoop(EngineConfig config) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto finish = [&] {
        running_.store(false);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (uninit) CoUninitialize();
    };

    auto source = FindDeviceById(config.sourceDeviceId);
    if (!source) { SetLastError(L"Основное аудиоустройство недоступно."); finish(); return; }

    ComPtr<IAudioClient> captureClient;
    HRESULT hr = source->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &captureClient);
    if (FAILED(hr)) { SetLastError(HrText(L"Не удалось открыть основной выход", hr)); finish(); return; }

    WAVEFORMATEX* mix = nullptr;
    hr = captureClient->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { SetLastError(HrText(L"Не удалось получить формат основного выхода", hr)); finish(); return; }
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixHolder(mix, &CoTaskMemFree);

    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent) { SetLastError(L"Не удалось создать событие захвата."); finish(); return; }

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, nullptr);
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        SetLastError(HrText(L"Не удалось запустить WASAPI Loopback", hr));
        finish(); return;
    }
    hr = captureClient->SetEventHandle(captureEvent);
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        SetLastError(HrText(L"Не удалось настроить событие WASAPI", hr));
        finish(); return;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = captureClient->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        SetLastError(HrText(L"Не удалось получить интерфейс захвата", hr));
        finish(); return;
    }

    std::vector<std::unique_ptr<Renderer>> renderers;
    for (const auto& route : config.outputs) {
        auto r = std::make_unique<Renderer>();
        r->deviceId = route.deviceId;
        r->mode = route.mode;
        std::wstring err;
        if (!r->Init(mix, err)) {
            CloseHandle(captureEvent);
            SetLastError(err);
            finish(); return;
        }
        renderers.push_back(std::move(r));
    }

    hr = captureClient->Start();
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        SetLastError(HrText(L"Не удалось начать захват", hr));
        finish(); return;
    }

    while (!stopRequested_.load()) {
        DWORD wr = WaitForSingleObject(captureEvent, 200);
        if (wr != WAIT_OBJECT_0) continue;

        UINT32 nextFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&nextFrames)) && nextFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD packetFlags = 0;
            hr = capture->GetBuffer(&data, &frames, &packetFlags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const bool silent = (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (size_t i = 0; i < renderers.size(); ++i) {
                auto converted = RouteAudio(data, frames, mix, config.sourceMode,
                                            config.outputs[i].mode, silent);
                renderers[i]->Push(std::move(converted));
            }
            capture->ReleaseBuffer(frames);
        }
    }

    captureClient->Stop();
    for (auto& r : renderers) r->Shutdown();
    CloseHandle(captureEvent);
    finish();
}

} // namespace ad
