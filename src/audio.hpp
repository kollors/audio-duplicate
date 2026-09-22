#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiodup {

enum class ChannelMode : int {
    Stereo = 0,
    Left = 1,
    Right = 2,
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct OutputConfig {
    std::wstring deviceId;
    ChannelMode mode = ChannelMode::Stereo;
};

using AudioErrorCallback = std::function<void(const std::wstring&)>;

std::vector<AudioDeviceInfo> EnumerateRenderDevices();
std::wstring GetDefaultRenderDeviceId();
std::wstring HResultMessage(HRESULT hr);

class DeviceNotificationClient final : public IMMNotificationClient {
public:
    using ChangedCallback = std::function<void()>;

    explicit DeviceNotificationClient(ChangedCallback callback);

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    std::atomic<ULONG> refs_{1};
    ChangedCallback callback_;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(const std::wstring& sourceDeviceId,
               ChannelMode sourceMode,
               const std::vector<OutputConfig>& outputs,
               AudioErrorCallback errorCallback,
               std::wstring& error);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    class OutputEndpoint;

    bool ProbeSourceSampleRate(const std::wstring& deviceId, uint32_t& sampleRate, std::wstring& error);
    void CaptureThreadMain(std::wstring sourceDeviceId, ChannelMode sourceMode);
    void ReportError(const std::wstring& message);

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread captureThread_;
    std::vector<std::unique_ptr<OutputEndpoint>> outputs_;
    AudioErrorCallback errorCallback_;
    std::mutex callbackMutex_;
};

} // namespace audiodup
