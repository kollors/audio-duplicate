#include "config.hpp"

#include <windows.h>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace audiodup {
namespace {

std::string HexEncodeWide(const std::wstring& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 4);
    for (wchar_t wc : value) {
        const uint16_t v = static_cast<uint16_t>(wc);
        out.push_back(hex[(v >> 12) & 0xF]);
        out.push_back(hex[(v >> 8) & 0xF]);
        out.push_back(hex[(v >> 4) & 0xF]);
        out.push_back(hex[v & 0xF]);
    }
    return out;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

bool HexDecodeWide(const std::string& value, std::wstring& out) {
    if (value.size() % 4 != 0) return false;
    std::wstring decoded;
    decoded.reserve(value.size() / 4);
    for (size_t i = 0; i < value.size(); i += 4) {
        int a = HexValue(value[i]);
        int b = HexValue(value[i + 1]);
        int c = HexValue(value[i + 2]);
        int d = HexValue(value[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        uint16_t v = static_cast<uint16_t>((a << 12) | (b << 8) | (c << 4) | d);
        decoded.push_back(static_cast<wchar_t>(v));
    }
    out = std::move(decoded);
    return true;
}

ChannelMode ParseMode(const std::string& s) {
    if (s == "1") return ChannelMode::Left;
    if (s == "2") return ChannelMode::Right;
    return ChannelMode::Stereo;
}

int ModeValue(ChannelMode mode) {
    return static_cast<int>(mode);
}

} // namespace

std::wstring ConfigPathNextToExe() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, len);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) result.resize(slash + 1);
    else result.clear();
    result += L"AudioDuplicate.cfg";
    return result;
}

bool LoadConfig(AppConfig& config) {
    const std::wstring path = ConfigPathNextToExe();
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;

    std::string header;
    std::getline(in, header);
    if (header != "AUDIODUPLICATE1") return false;

    AppConfig loaded;
    loaded.outputs.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == "language") {
            loaded.language = value == "en" ? UiLanguage::English : UiLanguage::Russian;
        } else if (key == "autoRefresh") {
            loaded.autoRefresh = value != "0";
        } else if (key == "save") {
            loaded.saveSettings = value == "1";
        } else if (key == "sourceId") {
            HexDecodeWide(value, loaded.sourceDeviceId);
        } else if (key == "sourceMode") {
            loaded.sourceMode = ParseMode(value);
        } else if (key == "output") {
            const size_t comma = value.find(',');
            if (comma == std::string::npos) continue;
            SavedOutput output;
            if (!HexDecodeWide(value.substr(0, comma), output.deviceId)) continue;
            output.mode = ParseMode(value.substr(comma + 1));
            loaded.outputs.push_back(std::move(output));
        }
    }

    if (!loaded.saveSettings) return false;
    config = std::move(loaded);
    return true;
}

bool SaveConfig(const AppConfig& config) {
    if (!config.saveSettings) return false;
    const std::wstring path = ConfigPathNextToExe();
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "AUDIODUPLICATE1\n";
    out << "language=" << (config.language == UiLanguage::English ? "en" : "ru") << "\n";
    out << "autoRefresh=" << (config.autoRefresh ? 1 : 0) << "\n";
    out << "save=1\n";
    out << "sourceId=" << HexEncodeWide(config.sourceDeviceId) << "\n";
    out << "sourceMode=" << ModeValue(config.sourceMode) << "\n";
    for (const auto& output : config.outputs) {
        out << "output=" << HexEncodeWide(output.deviceId) << ',' << ModeValue(output.mode) << "\n";
    }
    return static_cast<bool>(out);
}

void DeleteConfig() {
    DeleteFileW(ConfigPathNextToExe().c_str());
}

} // namespace audiodup
