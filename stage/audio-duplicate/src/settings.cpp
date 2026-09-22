#include "settings.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ad {

static std::wstring ExeDir() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return std::filesystem::path(path).parent_path().wstring();
}

std::wstring SettingsPath() {
    return (std::filesystem::path(ExeDir()) / L"AudioDuplicate.ini").wstring();
}

static std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'=') out += L"\\=";
        else out += c;
    }
    return out;
}

static std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    bool esc = false;
    for (wchar_t c : s) {
        if (esc) {
            out += (c == L'n') ? L'\n' : c;
            esc = false;
        } else if (c == L'\\') esc = true;
        else out += c;
    }
    if (esc) out += L'\\';
    return out;
}

static size_t FindUnescapedEq(const std::wstring& s) {
    bool esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (esc) { esc = false; continue; }
        if (s[i] == L'\\') { esc = true; continue; }
        if (s[i] == L'=') return i;
    }
    return std::wstring::npos;
}

AppSettings LoadSettings() {
    AppSettings s;
    std::wifstream in(SettingsPath());
    if (!in) return s;

    std::wstring line;
    while (std::getline(in, line)) {
        const size_t p = FindUnescapedEq(line);
        if (p == std::wstring::npos) continue;
        const auto key = line.substr(0, p);
        const auto val = Unescape(line.substr(p + 1));
        if (key == L"autoRefresh") s.autoRefreshDevices = val == L"1";
        else if (key == L"saveBesideExe") s.saveBesideExe = val == L"1";
        else if (key == L"language") s.language = val == L"en" ? Language::English : Language::Russian;
        else if (key == L"sourceId") s.sourceDeviceId = val;
        else if (key == L"sourceMode") s.sourceMode = static_cast<ChannelMode>(_wtoi(val.c_str()));
        else if (key.rfind(L"output", 0) == 0) {
            // outputN=id|mode
            const size_t sep = val.rfind(L'|');
            if (sep != std::wstring::npos) {
                OutputRoute r;
                r.deviceId = val.substr(0, sep);
                r.mode = static_cast<ChannelMode>(_wtoi(val.substr(sep + 1).c_str()));
                if (!r.deviceId.empty()) s.outputs.push_back(std::move(r));
            }
        }
    }
    return s;
}

void SaveSettings(const AppSettings& s) {
    if (!s.saveBesideExe) {
        std::error_code ec;
        std::filesystem::remove(SettingsPath(), ec);
        return;
    }
    std::wofstream out(SettingsPath(), std::ios::trunc);
    if (!out) return;
    out << L"autoRefresh=" << (s.autoRefreshDevices ? 1 : 0) << L"\n";
    out << L"saveBesideExe=" << (s.saveBesideExe ? 1 : 0) << L"\n";
    out << L"language=" << (s.language == Language::English ? L"en" : L"ru") << L"\n";
    out << L"sourceId=" << Escape(s.sourceDeviceId) << L"\n";
    out << L"sourceMode=" << static_cast<int>(s.sourceMode) << L"\n";
    for (size_t i = 0; i < s.outputs.size(); ++i) {
        out << L"output" << i << L"=" << Escape(s.outputs[i].deviceId) << L"|" << static_cast<int>(s.outputs[i].mode) << L"\n";
    }
}

} // namespace ad
