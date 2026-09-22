#include "audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace audiodup {
namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const { return ptr_; }
    T** Put() {
        Reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

struct CoInitGuard {
    HRESULT hr = E_FAIL;
    CoInitGuard() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~CoInitGuard() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

struct MmcssGuard {
    HANDLE handle = nullptr;
    MmcssGuard() {
        DWORD taskIndex = 0;
        handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    }
    ~MmcssGuard() {
        if (handle) {
            AvRevertMmThreadCharacteristics(handle);
        }
    }
};

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

bool IsPcmFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    return false;
}

float ClampSample(float v) {
    return std::max(-1.0f, std::min(1.0f, v));
}

float ReadPcmSample(const BYTE* sample, int bits) {
    switch (bits) {
        case 8:
            return (static_cast<int>(*sample) - 128) / 128.0f;
        case 16: {
            int16_t v = 0;
            std::memcpy(&v, sample, sizeof(v));
            return static_cast<float>(v / 32768.0);
        }
        case 24: {
            int32_t v = (static_cast<int32_t>(sample[0]) |
                         (static_cast<int32_t>(sample[1]) << 8) |
                         (static_cast<int32_t>(sample[2]) << 16));
            if (v & 0x00800000) v |= 0xFF000000;
            return static_cast<float>(v / 8388608.0);
        }
        case 32: {
            int32_t v = 0;
            std::memcpy(&v, sample, sizeof(v));
            return static_cast<float>(v / 2147483648.0);
        }
        default:
            return 0.0f;
    }
}

float ReadSample(const BYTE* frame, int channel, const WAVEFORMATEX* format) {
    const int channels = format->nChannels;
    if (channels <= 0) return 0.0f;
    channel = std::max(0, std::min(channel, channels - 1));

    const int bytesPerSample = format->wBitsPerSample / 8;
    const BYTE* sample = frame + channel * bytesPerSample;

    if (IsFloatFormat(format) && format->wBitsPerSample == 32) {
        float v = 0.0f;
        std::memcpy(&v, sample, sizeof(v));
        return std::isfinite(v) ? ClampSample(v) : 0.0f;
    }
    if (IsPcmFormat(format)) {
        return ClampSample(ReadPcmSample(sample, format->wBitsPerSample));
    }
    return 0.0f;
}

void WritePcmSample(BYTE* sample, int bits, float value) {
    value = ClampSample(value);
    switch (bits) {
        case 8: {
            const int v = static_cast<int>(std::lround(value * 127.0f + 128.0f));
            *sample = static_cast<BYTE>(std::max(0, std::min(255, v)));
            break;
        }
        case 16: {
            const int v = static_cast<int>(std::lround(value * 32767.0f));
            const int16_t s = static_cast<int16_t>(std::max(-32768, std::min(32767, v)));
            std::memcpy(sample, &s, sizeof(s));
            break;
        }
        case 24: {
            int32_t v = static_cast<int32_t>(std::lround(value * 8388607.0f));
            v = std::max(-8388608, std::min(8388607, v));
            sample[0] = static_cast<BYTE>(v & 0xFF);
            sample[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            sample[2] = static_cast<BYTE>((v >> 16) & 0xFF);
            break;
        }
        case 32: {
            double scaled = static_cast<double>(value) * 2147483647.0;
            scaled = std::max(-2147483648.0, std::min(2147483647.0, scaled));
            const int32_t s = static_cast<int32_t>(std::llround(scaled));
            std::memcpy(sample, &s, sizeof(s));
            break;
        }
        default:
            break;
    }
}

void WriteSample(BYTE* frame, int channel, const WAVEFORMATEX* format, float value) {
    const int channels = format->nChannels;
    if (channel < 0 || channel >= channels) return;
    const int bytesPerSample = format->wBitsPerSample / 8;
    BYTE* sample = frame + channel * bytesPerSample;

    if (IsFloatFormat(format) && format->wBitsPerSample == 32) {
        value = ClampSample(value);
        std::memcpy(sample, &value, sizeof(value));
    } else if (IsPcmFormat(format)) {
        WritePcmSample(sample, format->wBitsPerSample, value);
    }
}

bool IsSupportedMixFormat(const WAVEFORMATEX* format) {
    if (!format || format->nChannels == 0 || format->nSamplesPerSec == 0) return false;
    if (IsFloatFormat(format)) return format->wBitsPerSample == 32;
    if (IsPcmFormat(format)) {
        return format->wBitsPerSample == 8 || format->wBitsPerSample == 16 ||
               format->wBitsPerSample == 24 || format->wBitsPerSample == 32;
    }
    return false;
}

class StereoRingBuffer {
public:
    explicit StereoRingBuffer(size_t capacityFrames)
        : capacityFrames_(std::max<size_t>(capacityFrames, 1024)),
          data_(capacityFrames_ * 2, 0.0f) {}

    void Push(const float* stereo, size_t frames) {
        if (!stereo || frames == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);

        if (frames >= capacityFrames_) {
            stereo += (frames - capacityFrames_) * 2;
            frames = capacityFrames_;
            readFrame_ = 0;
            writeFrame_ = 0;
            countFrames_ = 0;
            phase_ = 0.0;
        }

        const size_t overflow = countFrames_ + frames > capacityFrames_
                                    ? countFrames_ + frames - capacityFrames_
                                    : 0;
        if (overflow) {
            readFrame_ = (readFrame_ + overflow) % capacityFrames_;
            countFrames_ -= overflow;
            phase_ = 0.0;
        }

        for (size_t i = 0; i < frames; ++i) {
            const size_t dst = ((writeFrame_ + i) % capacityFrames_) * 2;
            data_[dst] = stereo[i * 2];
            data_[dst + 1] = stereo[i * 2 + 1];
        }
        writeFrame_ = (writeFrame_ + frames) % capacityFrames_;
        countFrames_ += frames;
    }

    size_t AvailableFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return countFrames_;
    }

    size_t PopResampled(float* outStereo,
                        size_t outputFrames,
                        double baseSourceFramesPerOutputFrame,
                        size_t targetFillFrames,
                        size_t startupFrames) {
        if (!outStereo || outputFrames == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        if (!armed_) {
            if (countFrames_ < startupFrames) return 0;
            armed_ = true;
        }

        if (countFrames_ < 2) return 0;

        const size_t hardMax = std::max<size_t>(targetFillFrames * 4, targetFillFrames + 1024);
        if (countFrames_ > hardMax) {
            const size_t drop = countFrames_ - targetFillFrames;
            readFrame_ = (readFrame_ + drop) % capacityFrames_;
            countFrames_ -= drop;
            phase_ = 0.0;
        }

        const double target = static_cast<double>(std::max<size_t>(targetFillFrames, 1));
        const double error = (static_cast<double>(countFrames_) - target) / target;
        const double correction = std::max(-0.01, std::min(0.01, error * 0.004));
        const double ratio = baseSourceFramesPerOutputFrame * (1.0 + correction);

        size_t produced = 0;
        double localPhase = phase_;
        for (; produced < outputFrames; ++produced) {
            const size_t i0 = static_cast<size_t>(localPhase);
            const size_t i1 = i0 + 1;
            if (i1 >= countFrames_) break;

            const float frac = static_cast<float>(localPhase - static_cast<double>(i0));
            const size_t p0 = ((readFrame_ + i0) % capacityFrames_) * 2;
            const size_t p1 = ((readFrame_ + i1) % capacityFrames_) * 2;
            outStereo[produced * 2] = data_[p0] + (data_[p1] - data_[p0]) * frac;
            outStereo[produced * 2 + 1] = data_[p0 + 1] + (data_[p1 + 1] - data_[p0 + 1]) * frac;
            localPhase += ratio;
        }

        const size_t consume = static_cast<size_t>(localPhase);
        if (consume > 0) {
            const size_t safeConsume = std::min(consume, countFrames_);
            readFrame_ = (readFrame_ + safeConsume) % capacityFrames_;
            countFrames_ -= safeConsume;
            localPhase -= static_cast<double>(safeConsume);
        }
        phase_ = localPhase;
        return produced;
    }

private:
    size_t capacityFrames_ = 0;
    std::vector<float> data_;
    mutable std::mutex mutex_;
    size_t readFrame_ = 0;
    size_t writeFrame_ = 0;
    size_t countFrames_ = 0;
    double phase_ = 0.0;
    bool armed_ = false;
};

HRESULT ActivateAudioClient(const std::wstring& deviceId, ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr)) return hr;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(deviceId.c_str(), device.Put());
    if (FAILED(hr)) return hr;

    return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(client.Put()));
}

} // namespace

std::wstring HResultMessage(HRESULT hr) {
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr),
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (len && raw) {
        std::wstring text(raw, len);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
        ss << L" — " << text;
        LocalFree(raw);
    }
    return ss.str();
}

std::wstring GetDefaultRenderDeviceId() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.Put())))) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put()))) {
        return {};
    }
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

std::vector<AudioDeviceInfo> EnumerateRenderDevices() {
    std::vector<AudioDeviceInfo> result;
    const std::wstring defaultId = GetDefaultRenderDeviceId();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr)) return result;

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.Put());
    if (FAILED(hr)) return result;

    UINT count = 0;
    collection->GetCount(&count);
    result.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.Put()))) continue;

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;

        std::wstring name = L"Audio device";
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.Put()))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) {
                name = value.pwszVal;
            }
            PropVariantClear(&value);
        }

        AudioDeviceInfo info;
        info.id = id;
        info.name = name;
        info.isDefault = (info.id == defaultId);
        CoTaskMemFree(id);
        result.push_back(std::move(info));
    }

    std::stable_sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        if (a.isDefault != b.isDefault) return a.isDefault > b.isDefault;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}

DeviceNotificationClient::DeviceNotificationClient(ChangedCallback callback)
    : callback_(std::move(callback)) {}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::AddRef() {
    return ++refs_;
}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::Release() {
    const ULONG value = --refs_;
    if (value == 0) delete this;
    return value;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) {
    if (flow == eRender && callback_) callback_();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceAdded(LPCWSTR) {
    if (callback_) callback_();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceRemoved(LPCWSTR) {
    if (callback_) callback_();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceStateChanged(LPCWSTR, DWORD) {
    if (callback_) callback_();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    if (callback_) callback_();
    return S_OK;
}

class AudioEngine::OutputEndpoint {
public:
    OutputEndpoint(std::wstring deviceId,
                   ChannelMode mode,
                   uint32_t sourceRate,
                   AudioErrorCallback errorCallback)
        : deviceId_(std::move(deviceId)),
          mode_(mode),
          sourceRate_(sourceRate),
          ring_(static_cast<size_t>(sourceRate) / 2),
          errorCallback_(std::move(errorCallback)) {}

    ~OutputEndpoint() { Stop(); }

    void Start() {
        stop_.store(false);
        thread_ = std::thread([this] { ThreadMain(); });
    }

    void Stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    void Push(const float* stereo, size_t frames) {
        ring_.Push(stereo, frames);
    }

private:
    void Fail(const std::wstring& where, HRESULT hr) {
        if (errorCallback_) {
            errorCallback_(where + L": " + HResultMessage(hr));
        }
    }

    void ThreadMain() {
        CoInitGuard co;
        if (FAILED(co.hr)) {
            Fail(L"COM initialization for output failed", co.hr);
            return;
        }
        MmcssGuard mmcss;

        ComPtr<IAudioClient> client;
        HRESULT hr = ActivateAudioClient(deviceId_, client);
        if (FAILED(hr)) {
            Fail(L"Unable to open output device", hr);
            return;
        }

        WAVEFORMATEX* format = nullptr;
        hr = client->GetMixFormat(&format);
        if (FAILED(hr) || !format) {
            Fail(L"Unable to get output format", hr);
            return;
        }
        auto freeFormat = [&]() {
            if (format) {
                CoTaskMemFree(format);
                format = nullptr;
            }
        };

        if (!IsSupportedMixFormat(format)) {
            freeFormat();
            if (errorCallback_) errorCallback_(L"Output device uses an unsupported audio format.");
            return;
        }

        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            freeFormat();
            if (errorCallback_) errorCallback_(L"Unable to create the output audio event.");
            return;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                0, 0, format, nullptr);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            freeFormat();
            Fail(L"Unable to initialize output WASAPI", hr);
            return;
        }

        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            freeFormat();
            Fail(L"Unable to configure output WASAPI event", hr);
            return;
        }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            freeFormat();
            Fail(L"Unable to query output buffer", hr);
            return;
        }

        ComPtr<IAudioRenderClient> render;
        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.Put()));
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            freeFormat();
            Fail(L"Unable to get WASAPI render service", hr);
            return;
        }

        BYTE* initial = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &initial))) {
            render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = client->Start();
        if (FAILED(hr)) {
            CloseHandle(eventHandle);
            freeFormat();
            Fail(L"Unable to start output device", hr);
            return;
        }

        const uint32_t targetRate = format->nSamplesPerSec;
        const size_t targetFill = std::max<size_t>(static_cast<size_t>(sourceRate_) * 12 / 1000, 64);
        const size_t startupFill = std::max<size_t>(static_cast<size_t>(sourceRate_) * 4 / 1000, 32);
        const double baseRatio = static_cast<double>(sourceRate_) / static_cast<double>(targetRate);
        std::vector<float> stereo;

        while (!stop_.load()) {
            const DWORD wait = WaitForSingleObject(eventHandle, 100);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) continue;
            if (stop_.load()) break;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                Fail(L"Output device stopped responding", hr);
                break;
            }
            if (padding >= bufferFrames) continue;

            const UINT32 framesToWrite = bufferFrames - padding;
            BYTE* buffer = nullptr;
            hr = render->GetBuffer(framesToWrite, &buffer);
            if (FAILED(hr)) {
                Fail(L"Unable to access output audio buffer", hr);
                break;
            }

            std::memset(buffer, 0, static_cast<size_t>(framesToWrite) * format->nBlockAlign);
            stereo.assign(static_cast<size_t>(framesToWrite) * 2, 0.0f);
            const size_t produced = ring_.PopResampled(stereo.data(), framesToWrite, baseRatio,
                                                       targetFill, startupFill);

            for (size_t i = 0; i < produced; ++i) {
                const float left = stereo[i * 2];
                const float right = stereo[i * 2 + 1];
                const float mono = (left + right) * 0.5f;
                BYTE* frame = buffer + i * format->nBlockAlign;

                if (format->nChannels == 1) {
                    WriteSample(frame, 0, format, mono);
                    continue;
                }

                switch (mode_) {
                    case ChannelMode::Stereo:
                        WriteSample(frame, 0, format, left);
                        WriteSample(frame, 1, format, right);
                        break;
                    case ChannelMode::Left:
                        WriteSample(frame, 0, format, mono);
                        WriteSample(frame, 1, format, 0.0f);
                        break;
                    case ChannelMode::Right:
                        WriteSample(frame, 0, format, 0.0f);
                        WriteSample(frame, 1, format, mono);
                        break;
                }
            }

            hr = render->ReleaseBuffer(framesToWrite, 0);
            if (FAILED(hr)) {
                Fail(L"Unable to submit audio to output device", hr);
                break;
            }
        }

        client->Stop();
        CloseHandle(eventHandle);
        freeFormat();
    }

    std::wstring deviceId_;
    ChannelMode mode_ = ChannelMode::Stereo;
    uint32_t sourceRate_ = 48000;
    StereoRingBuffer ring_;
    AudioErrorCallback errorCallback_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::ProbeSourceSampleRate(const std::wstring& deviceId,
                                        uint32_t& sampleRate,
                                        std::wstring& error) {
    ComPtr<IAudioClient> client;
    HRESULT hr = ActivateAudioClient(deviceId, client);
    if (FAILED(hr)) {
        error = L"Unable to open the main output: " + HResultMessage(hr);
        return false;
    }

    WAVEFORMATEX* format = nullptr;
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) {
        error = L"Unable to query the main output format: " + HResultMessage(hr);
        return false;
    }

    const bool supported = IsSupportedMixFormat(format);
    sampleRate = format->nSamplesPerSec;
    CoTaskMemFree(format);
    if (!supported) {
        error = L"The main output uses an unsupported audio format.";
        return false;
    }
    return true;
}

bool AudioEngine::Start(const std::wstring& sourceDeviceId,
                        ChannelMode sourceMode,
                        const std::vector<OutputConfig>& outputs,
                        AudioErrorCallback errorCallback,
                        std::wstring& error) {
    Stop();
    if (sourceDeviceId.empty()) {
        error = L"Main output is not selected.";
        return false;
    }
    if (outputs.empty()) {
        error = L"Add at least one additional output.";
        return false;
    }

    uint32_t sourceRate = 0;
    if (!ProbeSourceSampleRate(sourceDeviceId, sourceRate, error)) return false;

    stop_.store(false);
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        errorCallback_ = std::move(errorCallback);
    }

    try {
        outputs_.clear();
        outputs_.reserve(outputs.size());
        for (const auto& cfg : outputs) {
            auto endpoint = std::make_unique<OutputEndpoint>(
                cfg.deviceId, cfg.mode, sourceRate,
                [this](const std::wstring& message) { ReportError(message); });
            endpoint->Start();
            outputs_.push_back(std::move(endpoint));
        }

        captureThread_ = std::thread([this, sourceDeviceId, sourceMode] {
            CaptureThreadMain(sourceDeviceId, sourceMode);
        });
    } catch (...) {
        error = L"Unable to create audio worker threads.";
        Stop();
        return false;
    }

    return true;
}

void AudioEngine::Stop() {
    stop_.store(true);
    if (captureThread_.joinable()) captureThread_.join();
    for (auto& output : outputs_) {
        if (output) output->Stop();
    }
    outputs_.clear();
    running_.store(false);
}

void AudioEngine::ReportError(const std::wstring& message) {
    AudioErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = errorCallback_;
    }
    if (callback) callback(message);
}

void AudioEngine::CaptureThreadMain(std::wstring sourceDeviceId, ChannelMode sourceMode) {
    CoInitGuard co;
    if (FAILED(co.hr)) {
        ReportError(L"COM initialization for loopback capture failed: " + HResultMessage(co.hr));
        running_.store(false);
        return;
    }
    MmcssGuard mmcss;

    ComPtr<IAudioClient> client;
    HRESULT hr = ActivateAudioClient(sourceDeviceId, client);
    if (FAILED(hr)) {
        ReportError(L"Unable to open the main output: " + HResultMessage(hr));
        running_.store(false);
        return;
    }

    WAVEFORMATEX* format = nullptr;
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) {
        ReportError(L"Unable to get main output format: " + HResultMessage(hr));
        running_.store(false);
        return;
    }
    auto freeFormat = [&]() {
        if (format) {
            CoTaskMemFree(format);
            format = nullptr;
        }
    };

    if (!IsSupportedMixFormat(format)) {
        freeFormat();
        ReportError(L"The main output uses an unsupported audio format.");
        running_.store(false);
        return;
    }

    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        freeFormat();
        ReportError(L"Unable to create the loopback capture event.");
        running_.store(false);
        return;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            0, 0, format, nullptr);
    if (FAILED(hr)) {
        CloseHandle(eventHandle);
        freeFormat();
        ReportError(L"Unable to initialize WASAPI loopback: " + HResultMessage(hr));
        running_.store(false);
        return;
    }

    hr = client->SetEventHandle(eventHandle);
    if (FAILED(hr)) {
        CloseHandle(eventHandle);
        freeFormat();
        ReportError(L"Unable to configure the loopback capture event: " + HResultMessage(hr));
        running_.store(false);
        return;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.Put()));
    if (FAILED(hr)) {
        CloseHandle(eventHandle);
        freeFormat();
        ReportError(L"Unable to get WASAPI capture service: " + HResultMessage(hr));
        running_.store(false);
        return;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(eventHandle);
        freeFormat();
        ReportError(L"Unable to start loopback capture: " + HResultMessage(hr));
        running_.store(false);
        return;
    }

    std::vector<float> stereo;
    while (!stop_.load()) {
        const DWORD wait = WaitForSingleObject(eventHandle, 100);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) continue;
        if (stop_.load()) break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                ReportError(L"Unable to read loopback audio: " + HResultMessage(hr));
                stop_.store(true);
                break;
            }

            stereo.assign(static_cast<size_t>(frames) * 2, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE* frame = data + static_cast<size_t>(i) * format->nBlockAlign;
                    float left = ReadSample(frame, 0, format);
                    float right = format->nChannels > 1 ? ReadSample(frame, 1, format) : left;
                    if (sourceMode == ChannelMode::Left) {
                        right = left;
                    } else if (sourceMode == ChannelMode::Right) {
                        left = right;
                    }
                    stereo[static_cast<size_t>(i) * 2] = left;
                    stereo[static_cast<size_t>(i) * 2 + 1] = right;
                }
            }

            for (auto& output : outputs_) {
                if (output) output->Push(stereo.data(), frames);
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    CloseHandle(eventHandle);
    freeFormat();
    running_.store(false);
}

} // namespace audiodup
