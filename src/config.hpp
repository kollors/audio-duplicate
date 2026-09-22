#pragma once

#include "audio.hpp"
#include <string>
#include <vector>

namespace audiodup {

enum class UiLanguage {
    Russian,
    English,
};

struct SavedOutput {
    std::wstring deviceId;
    ChannelMode mode = ChannelMode::Stereo;
};

struct AppConfig {
    UiLanguage language = UiLanguage::Russian;
    bool autoRefresh = true;
    bool saveSettings = false;
    std::wstring sourceDeviceId;
    ChannelMode sourceMode = ChannelMode::Stereo;
    std::vector<SavedOutput> outputs;
};

std::wstring ConfigPathNextToExe();
bool LoadConfig(AppConfig& config);
bool SaveConfig(const AppConfig& config);
void DeleteConfig();

} // namespace audiodup
