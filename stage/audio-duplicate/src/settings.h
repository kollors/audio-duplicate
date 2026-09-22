#pragma once

#include "audio_engine.h"
#include <string>
#include <vector>

namespace ad {

enum class Language { Russian = 0, English = 1 };

struct AppSettings {
    bool autoRefreshDevices = true;
    bool saveBesideExe = true;
    Language language = Language::Russian;
    std::wstring sourceDeviceId;
    ChannelMode sourceMode = ChannelMode::Stereo;
    std::vector<OutputRoute> outputs;
};

std::wstring SettingsPath();
AppSettings LoadSettings();
void SaveSettings(const AppSettings& settings);

} // namespace ad
