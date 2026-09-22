#include "audio_engine.h"
#include "settings.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

using namespace ad;

namespace {

constexpr wchar_t kMainClass[] = L"AudioDuplicateMain";
constexpr wchar_t kPaneClass[] = L"AudioDuplicateOutputPane";
constexpr wchar_t kSettingsClass[] = L"AudioDuplicateSettings";
constexpr UINT_PTR kAutoRefreshTimer = 1;
constexpr int kTimerMs = 3000;
constexpr int kRowHeight = 48;

constexpr COLORREF kBg = RGB(18, 22, 27);
constexpr COLORREF kPanel = RGB(25, 30, 37);
constexpr COLORREF kText = RGB(235, 239, 244);
constexpr COLORREF kMuted = RGB(153, 163, 175);
constexpr COLORREF kAccent = RGB(16, 185, 129);

HFONT gFont = nullptr;
HFONT gFontBold = nullptr;
HBRUSH gBgBrush = nullptr;
HBRUSH gPanelBrush = nullptr;

struct OutputRow {
    HWND device = nullptr;
    HWND mode = nullptr;
    HWND remove = nullptr;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND refreshButton = nullptr;
    HWND settingsButton = nullptr;
    HWND sourceDevice = nullptr;
    HWND sourceMode = nullptr;
    HWND addButton = nullptr;
    HWND pane = nullptr;
    HWND status = nullptr;
    HWND startStop = nullptr;
    std::vector<OutputRow> rows;
    std::vector<AudioDevice> devices;
    AppSettings settings;
    AudioEngine engine;
    int scrollPos = 0;
    bool uiRunning = false;
};

AppState gApp;

bool IsEnglish() { return gApp.settings.language == Language::English; }
const wchar_t* Tr(const wchar_t* ru, const wchar_t* en) { return IsEnglish() ? en : ru; }

void SetDarkTitle(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

void SetFont(HWND hwnd, bool bold = false) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? gFontBold : gFont), TRUE);
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, HWND parent, int id = 0) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SetFont(c);
    if (wcscmp(cls, WC_COMBOBOXW) == 0 || wcscmp(cls, L"BUTTON") == 0) {
        SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
    }
    return c;
}

int ComboSelected(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

std::wstring ComboDeviceId(HWND combo) {
    int sel = ComboSelected(combo);
    if (sel < 0) return {};
    auto idx = static_cast<size_t>(SendMessageW(combo, CB_GETITEMDATA, sel, 0));
    if (idx >= gApp.devices.size()) return {};
    return gApp.devices[idx].id;
}

void FillModeCombo(HWND combo, ChannelMode selected) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(L"Стерео", L"Stereo")));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(L"Левый канал", L"Left channel")));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(L"Правый канал", L"Right channel")));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}


void EnsureRememberedDevice(const std::wstring& id) {
    if (id.empty()) return;
    auto it = std::find_if(gApp.devices.begin(), gApp.devices.end(), [&](const AudioDevice& d) { return d.id == id; });
    if (it == gApp.devices.end()) {
        AudioDevice missing;
        missing.id = id;
        missing.name = Tr(L"Устройство недоступно", L"Device unavailable");
        gApp.devices.push_back(std::move(missing));
    }
}

void FillDeviceCombo(HWND combo, const std::wstring& selectedId) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int select = -1;
    for (size_t i = 0; i < gApp.devices.size(); ++i) {
        std::wstring name = gApp.devices[i].name;
        if (gApp.devices[i].isDefault) name += Tr(L"  (по умолчанию)", L"  (default)");
        int pos = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())));
        SendMessageW(combo, CB_SETITEMDATA, pos, static_cast<LPARAM>(i));
        if (gApp.devices[i].id == selectedId) select = pos;
    }
    if (select < 0 && !gApp.devices.empty()) select = 0;
    SendMessageW(combo, CB_SETCURSEL, select, 0);
}

void LayoutRows();

void RefreshDevices() {
    std::wstring sourceId = ComboDeviceId(gApp.sourceDevice);
    std::vector<std::wstring> outputIds;
    outputIds.reserve(gApp.rows.size());
    for (auto& row : gApp.rows) outputIds.push_back(ComboDeviceId(row.device));

    gApp.devices = AudioEngine::EnumerateRenderDevices();
    const std::wstring sourceToKeep = sourceId.empty() ? gApp.settings.sourceDeviceId : sourceId;
    EnsureRememberedDevice(sourceToKeep);
    for (const auto& id : outputIds) EnsureRememberedDevice(id);
    FillDeviceCombo(gApp.sourceDevice, sourceToKeep);
    for (size_t i = 0; i < gApp.rows.size(); ++i) {
        FillDeviceCombo(gApp.rows[i].device, outputIds[i]);
    }
}

void UpdatePaneScroll() {
    if (!gApp.pane) return;
    RECT rc{};
    GetClientRect(gApp.pane, &rc);
    const int content = static_cast<int>(gApp.rows.size()) * kRowHeight + 8;
    const int page = rc.bottom - rc.top;
    const int maxPos = std::max(0, content - page);
    gApp.scrollPos = std::clamp(gApp.scrollPos, 0, maxPos);

    SCROLLINFO si{sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(content - 1, 0);
    si.nPage = std::max(page, 1);
    si.nPos = gApp.scrollPos;
    SetScrollInfo(gApp.pane, SB_VERT, &si, TRUE);
}

void LayoutRows() {
    if (!gApp.pane) return;
    RECT rc{};
    GetClientRect(gApp.pane, &rc);
    const int width = rc.right - rc.left;
    const int margin = 0;
    const int modeW = 135;
    const int buttonW = 38;
    const int gap = 10;
    const int deviceW = std::max(180, width - modeW - buttonW - gap * 2 - 18);

    for (size_t i = 0; i < gApp.rows.size(); ++i) {
        int y = static_cast<int>(i) * kRowHeight - gApp.scrollPos + 4;
        // CBS_DROPDOWNLIST uses the window height for the drop-down list as well.
        // A height of 34px makes the list effectively one item tall.
        MoveWindow(gApp.rows[i].device, margin, y, deviceW, 240, TRUE);
        MoveWindow(gApp.rows[i].mode, margin + deviceW + gap, y, modeW, 180, TRUE);
        MoveWindow(gApp.rows[i].remove, margin + deviceW + gap + modeW + gap, y, buttonW, 34, TRUE);
    }
    UpdatePaneScroll();
}

void AddRow(const OutputRoute* initial = nullptr) {
    OutputRow row;
    row.device = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 200, 300, gApp.pane);
    row.mode = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 120, 200, gApp.pane);
    row.remove = MakeControl(L"BUTTON", L"−", BS_PUSHBUTTON, 0, 0, 36, 34, gApp.pane);
    std::wstring initialDevice = initial ? initial->deviceId : L"";
    if (!initial && initialDevice.empty()) {
        const std::wstring sourceId = ComboDeviceId(gApp.sourceDevice);
        auto it = std::find_if(gApp.devices.begin(), gApp.devices.end(),
                               [&](const AudioDevice& d) { return d.id != sourceId; });
        if (it != gApp.devices.end()) initialDevice = it->id;
    }
    FillDeviceCombo(row.device, initialDevice);
    FillModeCombo(row.mode, initial ? initial->mode : ChannelMode::Stereo);
    gApp.rows.push_back(row);
    LayoutRows();
}

void RemoveRow(HWND button) {
    auto it = std::find_if(gApp.rows.begin(), gApp.rows.end(), [button](const OutputRow& r) { return r.remove == button; });
    if (it == gApp.rows.end()) return;
    DestroyWindow(it->device);
    DestroyWindow(it->mode);
    DestroyWindow(it->remove);
    gApp.rows.erase(it);
    LayoutRows();
}

AppSettings GatherSettings() {
    AppSettings s = gApp.settings;
    s.sourceDeviceId = ComboDeviceId(gApp.sourceDevice);
    int sm = ComboSelected(gApp.sourceMode);
    s.sourceMode = sm >= 0 ? static_cast<ChannelMode>(sm) : ChannelMode::Stereo;
    s.outputs.clear();
    for (const auto& row : gApp.rows) {
        OutputRoute r;
        r.deviceId = ComboDeviceId(row.device);
        int m = ComboSelected(row.mode);
        r.mode = m >= 0 ? static_cast<ChannelMode>(m) : ChannelMode::Stereo;
        if (!r.deviceId.empty()) s.outputs.push_back(std::move(r));
    }
    return s;
}

void SetStatus(const wchar_t* text, bool /*active*/) {
    SetWindowTextW(gApp.status, text);
}

void UpdateRunUi() {
    const bool running = gApp.engine.IsRunning();
    gApp.uiRunning = running;
    SetWindowTextW(gApp.startStop, running ? Tr(L"Остановить", L"Stop") : Tr(L"Запустить", L"Start"));
    EnableWindow(gApp.sourceDevice, !running);
    EnableWindow(gApp.sourceMode, !running);
    EnableWindow(gApp.addButton, !running);
    EnableWindow(gApp.refreshButton, !running);
    for (auto& row : gApp.rows) {
        EnableWindow(row.device, !running);
        EnableWindow(row.mode, !running);
        EnableWindow(row.remove, !running);
    }
    SetStatus(running ? Tr(L"●  Работает", L"●  Running") : Tr(L"●  Готов к работе", L"●  Ready"), running);
}

void ToggleAudio() {
    if (gApp.engine.IsRunning()) {
        gApp.engine.Stop();
        UpdateRunUi();
        return;
    }
    AppSettings s = GatherSettings();
    EngineConfig c{s.sourceDeviceId, s.sourceMode, s.outputs};
    std::wstring error;
    if (!gApp.engine.Start(c, error)) {
        MessageBoxW(gApp.hwnd, error.c_str(), L"Audio Duplicate", MB_ICONERROR | MB_OK);
        return;
    }
    gApp.settings = s;
    SaveSettings(gApp.settings);
    UpdateRunUi();
}

void ApplyLanguage() {
    SetWindowTextW(gApp.hwnd, L"Audio Duplicate");
    SetWindowTextW(GetDlgItem(gApp.hwnd, 1101), Tr(L"Основной выход", L"Main output"));
    SetWindowTextW(GetDlgItem(gApp.hwnd, 1102), Tr(L"Дополнительные выходы", L"Additional outputs"));
    FillModeCombo(gApp.sourceMode, static_cast<ChannelMode>(std::max(0, ComboSelected(gApp.sourceMode))));
    for (auto& row : gApp.rows) {
        auto mode = static_cast<ChannelMode>(std::max(0, ComboSelected(row.mode)));
        FillModeCombo(row.mode, mode);
    }
    RefreshDevices();
    UpdateRunUi();
    InvalidateRect(gApp.hwnd, nullptr, TRUE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND autoRefresh = nullptr;
    static HWND save = nullptr;
    static HWND lang = nullptr;
    switch (msg) {
    case WM_CREATE: {
        SetDarkTitle(hwnd);
        MakeControl(L"STATIC", Tr(L"Настройки", L"Settings"), SS_LEFT, 24, 20, 300, 30, hwnd);
        autoRefresh = MakeControl(L"BUTTON", Tr(L"Автообновлять список устройств", L"Auto-refresh device list"), BS_AUTOCHECKBOX, 24, 66, 330, 26, hwnd);
        save = MakeControl(L"BUTTON", Tr(L"Сохранять настройки рядом с EXE", L"Save settings beside EXE"), BS_AUTOCHECKBOX, 24, 104, 330, 26, hwnd);
        MakeControl(L"STATIC", Tr(L"Язык", L"Language"), SS_LEFT, 24, 150, 100, 24, hwnd);
        lang = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 120, 144, 220, 150, hwnd);
        SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский"));
        SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(lang, CB_SETCURSEL, static_cast<WPARAM>(gApp.settings.language), 0);
        SendMessageW(autoRefresh, BM_SETCHECK, gApp.settings.autoRefreshDevices ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(save, BM_SETCHECK, gApp.settings.saveBesideExe ? BST_CHECKED : BST_UNCHECKED, 0);
        MakeControl(L"BUTTON", Tr(L"Сохранить", L"Save"), BS_DEFPUSHBUTTON, 172, 206, 100, 36, hwnd, IDOK);
        MakeControl(L"BUTTON", Tr(L"Отмена", L"Cancel"), BS_PUSHBUTTON, 282, 206, 90, 36, hwnd, IDCANCEL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            gApp.settings.autoRefreshDevices = SendMessageW(autoRefresh, BM_GETCHECK, 0, 0) == BST_CHECKED;
            gApp.settings.saveBesideExe = SendMessageW(save, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int l = ComboSelected(lang);
            if (l >= 0) gApp.settings.language = static_cast<Language>(l);
            gApp.settings = GatherSettings();
            // GatherSettings preserves general settings from gApp.settings.
            SaveSettings(gApp.settings);
            if (gApp.settings.autoRefreshDevices) SetTimer(gApp.hwnd, kAutoRefreshTimer, kTimerMs, nullptr);
            else KillTimer(gApp.hwnd, kAutoRefreshTimer);
            ApplyLanguage();
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText); SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(gBgBrush);
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(gBgBrush);
    case WM_CLOSE:
        DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenSettings() {
    HWND existing = FindWindowW(kSettingsClass, nullptr);
    if (existing) { SetForegroundWindow(existing); return; }
    RECT rc{}; GetWindowRect(gApp.hwnd, &rc);
    CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass, Tr(L"Настройки", L"Settings"),
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    rc.left + 80, rc.top + 80, 420, 300,
                    gApp.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK PaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= kRowHeight; break;
        case SB_LINEDOWN: pos += kRowHeight; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        default: break;
        }
        int maxPos = std::max(0, si.nMax - static_cast<int>(si.nPage) + 1);
        gApp.scrollPos = std::clamp(pos, 0, maxPos);
        LayoutRows();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        gApp.scrollPos = std::max(0, gApp.scrollPos - (delta / WHEEL_DELTA) * kRowHeight);
        LayoutRows();
        return 0;
    }
    case WM_SIZE:
        LayoutRows(); return 0;
    case WM_COMMAND:
        // Controls in the scroll pane notify the pane, not the main window.
        return SendMessageW(GetParent(hwnd), WM_COMMAND, wp, lp);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText); SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(gPanelBrush);
    }
    case WM_ERASEBKGND: {
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, gPanelBrush);
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void LayoutMain(HWND hwnd) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    MoveWindow(gApp.refreshButton, w - 102, 12, 38, 34, TRUE);
    MoveWindow(gApp.settingsButton, w - 56, 12, 38, 34, TRUE);
    MoveWindow(gApp.sourceDevice, 24, 104, std::max(260, w - 240), 220, TRUE);
    MoveWindow(gApp.sourceMode, w - 190, 104, 166, 180, TRUE);
    MoveWindow(gApp.addButton, w - 62, 166, 38, 34, TRUE);
    MoveWindow(gApp.pane, 24, 212, w - 48, std::max(80, h - 292), TRUE);
    MoveWindow(gApp.status, 24, h - 58, 220, 30, TRUE);
    MoveWindow(gApp.startStop, w - 164, h - 66, 140, 38, TRUE);
    LayoutRows();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        gApp.hwnd = hwnd;
        SetDarkTitle(hwnd);
        gApp.settings = LoadSettings();
        gApp.devices = AudioEngine::EnumerateRenderDevices();
        EnsureRememberedDevice(gApp.settings.sourceDeviceId);
        for (const auto& route : gApp.settings.outputs) EnsureRememberedDevice(route.deviceId);

        gApp.refreshButton = MakeControl(L"BUTTON", L"↻", BS_PUSHBUTTON, 0, 0, 38, 34, hwnd, 1001);
        gApp.settingsButton = MakeControl(L"BUTTON", L"⚙", BS_PUSHBUTTON, 0, 0, 38, 34, hwnd, 1002);

        auto title1 = MakeControl(L"STATIC", Tr(L"Основной выход", L"Main output"), SS_LEFT, 24, 70, 300, 26, hwnd, 1101);
        SetFont(title1, true);
        gApp.sourceDevice = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 104, 420, 220, hwnd);
        gApp.sourceMode = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 460, 104, 150, 160, hwnd);
        FillDeviceCombo(gApp.sourceDevice, gApp.settings.sourceDeviceId);
        FillModeCombo(gApp.sourceMode, gApp.settings.sourceMode);

        auto title2 = MakeControl(L"STATIC", Tr(L"Дополнительные выходы", L"Additional outputs"), SS_LEFT, 24, 172, 300, 26, hwnd, 1102);
        SetFont(title2, true);
        gApp.addButton = MakeControl(L"BUTTON", L"+", BS_PUSHBUTTON, 0, 0, 38, 34, hwnd, 1003);
        gApp.pane = CreateWindowExW(0, kPaneClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                    24, 212, 600, 180, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        gApp.status = MakeControl(L"STATIC", Tr(L"●  Готов к работе", L"●  Ready"), SS_LEFT, 24, 0, 240, 30, hwnd);
        gApp.startStop = MakeControl(L"BUTTON", Tr(L"Запустить", L"Start"), BS_DEFPUSHBUTTON, 0, 0, 140, 38, hwnd, 1004);

        if (gApp.settings.outputs.empty()) AddRow();
        else for (const auto& r : gApp.settings.outputs) AddRow(&r);

        if (gApp.settings.autoRefreshDevices) SetTimer(hwnd, kAutoRefreshTimer, kTimerMs, nullptr);
        SetTimer(hwnd, 2, 250, nullptr);
        LayoutMain(hwnd);
        return 0;
    }
    case WM_SIZE:
        LayoutMain(hwnd); return 0;
    case WM_TIMER:
        if (wp == kAutoRefreshTimer && !gApp.engine.IsRunning()) RefreshDevices();
        if (wp == 2 && gApp.uiRunning && !gApp.engine.IsRunning()) {
            UpdateRunUi();
            const std::wstring err = gApp.engine.LastError();
            if (!err.empty()) MessageBoxW(hwnd, err.c_str(), L"Audio Duplicate", MB_ICONERROR | MB_OK);
        }
        return 0;
    case WM_COMMAND: {
        HWND src = reinterpret_cast<HWND>(lp);
        switch (LOWORD(wp)) {
        case 1001: RefreshDevices(); return 0;
        case 1002: OpenSettings(); return 0;
        case 1003: AddRow(); return 0;
        case 1004: ToggleAudio(); return 0;
        default:
            if (src) {
                for (auto& r : gApp.rows) if (r.remove == src) { RemoveRow(src); return 0; }
            }
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText); SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(gBgBrush);
    }
    case WM_ERASEBKGND: {
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, gBgBrush);
        return 1;
    }
    case WM_CLOSE:
        gApp.engine.Stop();
        gApp.settings = GatherSettings();
        SaveSettings(gApp.settings);
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    gBgBrush = CreateSolidBrush(kBg);
    gPanelBrush = CreateSolidBrush(kPanel);
    gFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gFontBold = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = gBgBrush;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{sizeof(pc)};
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.hbrBackground = gPanelBrush;
    pc.lpfnWndProc = PaneProc;
    pc.lpszClassName = kPaneClass;
    RegisterClassExW(&pc);

    WNDCLASSEXW sc{sizeof(sc)};
    sc.hInstance = hInst;
    sc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    sc.hbrBackground = gBgBrush;
    sc.lpfnWndProc = SettingsProc;
    sc.lpszClassName = kSettingsClass;
    RegisterClassExW(&sc);

    HWND hwnd = CreateWindowExW(0, kMainClass, L"Audio Duplicate",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 760, 620,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(gFont);
    DeleteObject(gFontBold);
    DeleteObject(gBgBrush);
    DeleteObject(gPanelBrush);
    return static_cast<int>(msg.wParam);
}
