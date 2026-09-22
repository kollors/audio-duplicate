#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ad {

enum class ChannelMode { Stereo = 0, Left = 1, Right = 2 };

struct AudioDevice {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct OutputRoute {
    std::wstring deviceId;
    ChannelMode mode = ChannelMode::Stereo;
};

struct EngineConfig {
    std::wstring sourceDeviceId;
    ChannelMode sourceMode = ChannelMode::Stereo;
    std::vector<OutputRoute> outputs;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    static std::vector<AudioDevice> EnumerateRenderDevices();

    bool Start(const EngineConfig& config, std::wstring& error);
    void Stop();
    bool IsRunning() const noexcept { return running_.load(); }
    std::wstring LastError() const;

private:
    struct Renderer;

    bool StartCapture(const EngineConfig& config, std::wstring& error);
    void CaptureLoop(EngineConfig config);
    void SetLastError(std::wstring text);

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread captureThread_;
    mutable std::mutex errorMutex_;
    std::wstring lastError_;
};

} // namespace ad
