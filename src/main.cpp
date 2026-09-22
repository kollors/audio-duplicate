#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WIN32_WINNT 0x0A00

#include "audio.hpp"
#include "config.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace audiodup {
namespace {

constexpr UINT WM_APP_DEVICES_CHANGED = WM_APP + 1;
constexpr UINT WM_APP_AUDIO_ERROR = WM_APP + 2;

constexpr int IDC_REFRESH = 1001;
constexpr int IDC_SETTINGS = 1002;
constexpr int IDC_SOURCE_DEVICE = 1003;
constexpr int IDC_SOURCE_MODE = 1004;
constexpr int IDC_ADD_OUTPUT = 1005;
constexpr int IDC_START = 1006;
constexpr int IDC_STOP = 1007;
constexpr int IDC_OUTPUT_DEVICE = 1101;
constexpr int IDC_OUTPUT_MODE = 1102;
constexpr int IDC_OUTPUT_REMOVE = 1103;

constexpr COLORREF kBackground = RGB(29, 32, 37);
constexpr COLORREF kPanel = RGB(36, 40, 46);
constexpr COLORREF kText = RGB(238, 241, 245);
constexpr COLORREF kMuted = RGB(170, 177, 186);

struct Texts {
    const wchar_t* mainOutput;
    const wchar_t* additionalOutputs;
    const wchar_t* stereo;
    const wchar_t* left;
    const wchar_t* right;
    const wchar_t* ready;
    const wchar_t* running;
    const wchar_t* start;
    const wchar_t* stop;
    const wchar_t* unavailable;
    const wchar_t* settings;
    const wchar_t* general;
    const wchar_t* autoRefresh;
    const wchar_t* saveNearExe;
    const wchar_t* interfaceSection;
    const wchar_t* language;
    const wchar_t* save;
    const wchar_t* cancel;
    const wchar_t* errorTitle;
    const wchar_t* selectSource;
    const wchar_t* addOutput;
    const wchar_t* sameAsSource;
    const wchar_t* duplicateOutput;
    const wchar_t* configSaveFailed;
};

const Texts kRu {
    L"Основной выход",
    L"Дополнительные выходы",
    L"Стерео",
    L"Левый канал",
    L"Правый канал",
    L"● Готов к работе",
    L"● Работает",
    L"Запустить",
    L"Остановить",
    L"Устройство недоступно",
    L"Настройки",
    L"Общие",
    L"Автообновлять список устройств",
    L"Сохранять настройки рядом с EXE",
    L"Интерфейс",
    L"Язык",
    L"Сохранить",
    L"Отмена",
    L"Audio Duplicate — ошибка",
    L"Выберите доступный основной выход.",
    L"Добавьте хотя бы один доступный дополнительный выход.",
    L"Дополнительный выход не может совпадать с основным.",
    L"Один и тот же дополнительный выход выбран несколько раз.",
    L"Не удалось сохранить настройки рядом с EXE. Проверьте права записи в папку программы."
};

const Texts kEn {
    L"Main output",
    L"Additional outputs",
    L"Stereo",
    L"Left channel",
    L"Right channel",
    L"● Ready",
    L"● Running",
    L"Start",
    L"Stop",
    L"Device unavailable",
    L"Settings",
    L"General",
    L"Auto-refresh device list",
    L"Save settings next to EXE",
    L"Interface",
    L"Language",
    L"Save",
    L"Cancel",
    L"Audio Duplicate — error",
    L"Select an available main output.",
    L"Add at least one available additional output.",
    L"An additional output cannot be the same device as the main output.",
    L"The same additional output is selected more than once.",
    L"Could not save settings next to the EXE. Check write permissions for the program folder."
};

const Texts& TextFor(UiLanguage lang) {
    return lang == UiLanguage::English ? kEn : kRu;
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL enable = TRUE;
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_VALUE = 20;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_VALUE, &enable, sizeof(enable));
}

void ApplyDarkTheme(HWND hwnd) {
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

HFONT CreateUiFont(int height = -18, int weight = FW_NORMAL) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SetFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

int ComboSelectedDeviceVectorIndex(HWND combo) {
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return -1;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (data <= 0 || data == CB_ERR) return -1;
    return static_cast<int>(data - 1);
}

ChannelMode ComboSelectedMode(HWND combo) {
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == 1) return ChannelMode::Left;
    if (sel == 2) return ChannelMode::Right;
    return ChannelMode::Stereo;
}

void PopulateModeCombo(HWND combo, const Texts& t, ChannelMode mode) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.stereo));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.left));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.right));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(mode), 0);
}

class ScrollPanel {
public:
    bool Create(HWND parent, HINSTANCE instance) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &ScrollPanel::WndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(kPanel);
            wc.lpszClassName = L"AudioDuplicateScrollPanel";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            registered = true;
        }

        hwnd_ = CreateWindowExW(0, L"AudioDuplicateScrollPanel", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                0, 0, 100, 100, parent, nullptr, instance, this);
        return hwnd_ != nullptr;
    }

    HWND hwnd() const { return hwnd_; }
    int offset() const { return scrollPos_; }

    void SetContentHeight(int height) {
        contentHeight_ = std::max(0, height);
        UpdateScrollInfo();
    }

    void ResetScroll() {
        scrollPos_ = 0;
        UpdateScrollInfo();
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        ScrollPanel* self = reinterpret_cast<ScrollPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<ScrollPanel*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
            case WM_SIZE:
                self->UpdateScrollInfo();
                return 0;
            case WM_VSCROLL:
                self->HandleVScroll(LOWORD(wp), HIWORD(wp));
                return 0;
            case WM_MOUSEWHEEL: {
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                self->ScrollBy(-(delta / WHEEL_DELTA) * 58);
                return 0;
            }
            case WM_COMMAND:
                return SendMessageW(GetParent(hwnd), msg, wp, lp);
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, kText);
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
            }
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                HBRUSH brush = CreateSolidBrush(kPanel);
                FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
                DeleteObject(brush);
                return 1;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void UpdateScrollInfo() {
        if (!hwnd_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int page = std::max(1, static_cast<int>(rc.bottom - rc.top));
        const int maxPos = std::max(0, contentHeight_ - page);
        if (scrollPos_ > maxPos) scrollPos_ = maxPos;

        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = std::max(0, contentHeight_ - 1);
        si.nPage = static_cast<UINT>(page);
        si.nPos = scrollPos_;
        SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
        ShowScrollBar(hwnd_, SB_VERT, contentHeight_ > page);
    }

    void HandleVScroll(int code, int trackPos) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int page = std::max(1, static_cast<int>(rc.bottom - rc.top));
        int next = scrollPos_;
        switch (code) {
            case SB_LINEUP: next -= 36; break;
            case SB_LINEDOWN: next += 36; break;
            case SB_PAGEUP: next -= page; break;
            case SB_PAGEDOWN: next += page; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: next = trackPos; break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = contentHeight_; break;
            default: return;
        }
        SetScrollPosition(next);
    }

    void ScrollBy(int delta) { SetScrollPosition(scrollPos_ + delta); }

    void SetScrollPosition(int next) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int page = std::max(1, static_cast<int>(rc.bottom - rc.top));
        const int maxPos = std::max(0, contentHeight_ - page);
        next = std::max(0, std::min(maxPos, next));
        if (next == scrollPos_) return;
        const int delta = scrollPos_ - next;
        scrollPos_ = next;
        ScrollWindowEx(hwnd_, 0, delta, nullptr, nullptr, nullptr, nullptr,
                       SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        UpdateScrollInfo();
        UpdateWindow(hwnd_);
    }

    HWND hwnd_ = nullptr;
    int contentHeight_ = 0;
    int scrollPos_ = 0;
};

class SettingsDialog {
public:
    bool Show(HWND owner, HINSTANCE instance, AppConfig& config) {
        owner_ = owner;
        instance_ = instance;
        config_ = &config;
        accepted_ = false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &SettingsDialog::WndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(kBackground);
            wc.lpszClassName = L"AudioDuplicateSettingsWindow";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            registered = true;
        }

        const Texts& t = TextFor(config.language);
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AudioDuplicateSettingsWindow", t.settings,
                                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 500, 360,
                                owner, nullptr, instance, this);
        if (!hwnd_) return false;
        ApplyDarkTitleBar(hwnd_);
        CenterToOwner();
        EnableWindow(owner, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (IsWindow(hwnd_) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        return accepted_;
    }

private:
    enum { IDC_AUTO = 2001, IDC_SAVE_NEAR = 2002, IDC_LANG = 2003, IDC_OK = 2004, IDC_CANCEL = 2005 };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsDialog* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<SettingsDialog*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
            case WM_CREATE: self->CreateControls(); return 0;
            case WM_COMMAND: self->OnCommand(LOWORD(wp)); return 0;
            case WM_CLOSE: DestroyWindow(hwnd); return 0;
            case WM_DESTROY:
                if (self->headingFont_) { DeleteObject(self->headingFont_); self->headingFont_ = nullptr; }
                if (self->font_) { DeleteObject(self->font_); self->font_ = nullptr; }
                if (self->brush_) { DeleteObject(self->brush_); self->brush_ = nullptr; }
                self->hwnd_ = nullptr;
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, kText);
                SetBkColor(dc, kBackground);
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(self->brush_);
            }
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, self->brush_);
                return 1;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CreateControls() {
        brush_ = CreateSolidBrush(kBackground);
        font_ = CreateUiFont();
        headingFont_ = CreateUiFont(-18, FW_SEMIBOLD);
        const Texts& t = TextFor(config_->language);

        auto label = [&](const wchar_t* text, int x, int y, int w, int h, int weight = FW_NORMAL) {
            HWND hnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                       x, y, w, h, hwnd_, nullptr, instance_, nullptr);
            if (weight == FW_BOLD) {
                SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
            } else {
                SetFont(hnd, font_);
            }
            return hnd;
        };

        label(t.general, 28, 24, 420, 24, FW_BOLD);
        autoRefresh_ = CreateWindowExW(0, L"BUTTON", t.autoRefresh,
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       30, 64, 420, 28, hwnd_, reinterpret_cast<HMENU>(IDC_AUTO), instance_, nullptr);
        saveNear_ = CreateWindowExW(0, L"BUTTON", t.saveNearExe,
                                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    30, 100, 420, 28, hwnd_, reinterpret_cast<HMENU>(IDC_SAVE_NEAR), instance_, nullptr);
        SetFont(autoRefresh_, font_);
        SetFont(saveNear_, font_);
        ApplyDarkTheme(autoRefresh_);
        ApplyDarkTheme(saveNear_);
        SendMessageW(autoRefresh_, BM_SETCHECK, config_->autoRefresh ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(saveNear_, BM_SETCHECK, config_->saveSettings ? BST_CHECKED : BST_UNCHECKED, 0);

        label(t.interfaceSection, 28, 158, 420, 24, FW_BOLD);
        label(t.language, 30, 202, 100, 28);
        language_ = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    150, 198, 290, 220, hwnd_, reinterpret_cast<HMENU>(IDC_LANG), instance_, nullptr);
        SetFont(language_, font_);
        ApplyDarkTheme(language_);
        SendMessageW(language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский"));
        SendMessageW(language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(language_, CB_SETCURSEL, config_->language == UiLanguage::English ? 1 : 0, 0);

        saveButton_ = CreateWindowExW(0, L"BUTTON", t.save,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      244, 274, 96, 36, hwnd_, reinterpret_cast<HMENU>(IDC_OK), instance_, nullptr);
        cancelButton_ = CreateWindowExW(0, L"BUTTON", t.cancel,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        350, 274, 96, 36, hwnd_, reinterpret_cast<HMENU>(IDC_CANCEL), instance_, nullptr);
        SetFont(saveButton_, font_);
        SetFont(cancelButton_, font_);
        ApplyDarkTheme(saveButton_);
        ApplyDarkTheme(cancelButton_);
    }

    void OnCommand(int id) {
        if (id == IDC_OK) {
            config_->autoRefresh = SendMessageW(autoRefresh_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            config_->saveSettings = SendMessageW(saveNear_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            config_->language = SendMessageW(language_, CB_GETCURSEL, 0, 0) == 1
                                    ? UiLanguage::English : UiLanguage::Russian;
            accepted_ = true;
            DestroyWindow(hwnd_);
        } else if (id == IDC_CANCEL) {
            DestroyWindow(hwnd_);
        }
    }

    void CenterToOwner() {
        RECT ownerRc{}, windowRc{};
        GetWindowRect(owner_, &ownerRc);
        GetWindowRect(hwnd_, &windowRc);
        const int w = windowRc.right - windowRc.left;
        const int h = windowRc.bottom - windowRc.top;
        const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2;
        const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND autoRefresh_ = nullptr;
    HWND saveNear_ = nullptr;
    HWND language_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH brush_ = nullptr;
    AppConfig* config_ = nullptr;
    bool accepted_ = false;
};

class MainWindow {
public:
    explicit MainWindow(HINSTANCE instance) : instance_(instance) {}
    ~MainWindow() = default;

    bool Create() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &MainWindow::WndProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = CreateSolidBrush(kBackground);
        wc.lpszClassName = L"AudioDuplicateMainWindow";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Audio Duplicate",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 820, 590,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;

        ApplyDarkTitleBar(hwnd_);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    HWND hwnd() const { return hwnd_; }

private:
    struct OutputRow {
        HWND deviceCombo = nullptr;
        HWND modeCombo = nullptr;
        HWND removeButton = nullptr;
        std::wstring deviceId;
        ChannelMode mode = ChannelMode::Stereo;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
            case WM_CREATE: return self->OnCreate() ? 0 : -1;
            case WM_SIZE: self->Layout(); return 0;
            case WM_COMMAND: self->OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp)); return 0;
            case WM_APP_DEVICES_CHANGED:
                if (self->config_.autoRefresh && !self->engine_.IsRunning()) self->RefreshDevices();
                return 0;
            case WM_APP_AUDIO_ERROR: {
                std::unique_ptr<std::wstring> message(reinterpret_cast<std::wstring*>(lp));
                self->StopAudio();
                MessageBoxW(hwnd, message ? message->c_str() : L"Audio error",
                            TextFor(self->config_.language).errorTitle, MB_OK | MB_ICONERROR);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, kText);
                SetBkColor(dc, kBackground);
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(self->backgroundBrush_);
            }
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, self->backgroundBrush_);
                return 1;
            }
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                self->OnDestroy();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool OnCreate() {
        backgroundBrush_ = CreateSolidBrush(kBackground);
        font_ = CreateUiFont();
        headingFont_ = CreateUiFont(-19, FW_SEMIBOLD);

        LoadConfig(config_);

        refreshButton_ = CreateWindowExW(0, L"BUTTON", L"↻", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         0, 0, 38, 34, hwnd_, reinterpret_cast<HMENU>(IDC_REFRESH), instance_, nullptr);
        settingsButton_ = CreateWindowExW(0, L"BUTTON", L"⚙", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          0, 0, 38, 34, hwnd_, reinterpret_cast<HMENU>(IDC_SETTINGS), instance_, nullptr);
        SetFont(refreshButton_, font_);
        SetFont(settingsButton_, font_);
        ApplyDarkTheme(refreshButton_);
        ApplyDarkTheme(settingsButton_);

        sourceLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       0, 0, 100, 24, hwnd_, nullptr, instance_, nullptr);
        SetFont(sourceLabel_, headingFont_);

        sourceCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 420, 300, hwnd_, reinterpret_cast<HMENU>(IDC_SOURCE_DEVICE), instance_, nullptr);
        sourceMode_ = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                      0, 0, 180, 200, hwnd_, reinterpret_cast<HMENU>(IDC_SOURCE_MODE), instance_, nullptr);
        SetFont(sourceCombo_, font_);
        SetFont(sourceMode_, font_);
        ApplyDarkTheme(sourceCombo_);
        ApplyDarkTheme(sourceMode_);

        outputsLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        0, 0, 220, 24, hwnd_, nullptr, instance_, nullptr);
        SetFont(outputsLabel_, headingFont_);
        addButton_ = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 38, 34, hwnd_, reinterpret_cast<HMENU>(IDC_ADD_OUTPUT), instance_, nullptr);
        SetFont(addButton_, headingFont_);
        ApplyDarkTheme(addButton_);

        if (!outputPanel_.Create(hwnd_, instance_)) return false;

        statusLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       0, 0, 240, 28, hwnd_, nullptr, instance_, nullptr);
        startButton_ = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       0, 0, 120, 38, hwnd_, reinterpret_cast<HMENU>(IDC_START), instance_, nullptr);
        stopButton_ = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      0, 0, 120, 38, hwnd_, reinterpret_cast<HMENU>(IDC_STOP), instance_, nullptr);
        SetFont(statusLabel_, font_);
        SetFont(startButton_, font_);
        SetFont(stopButton_, font_);
        ApplyDarkTheme(startButton_);
        ApplyDarkTheme(stopButton_);
        EnableWindow(stopButton_, FALSE);

        RefreshDevices();

        if (config_.sourceDeviceId.empty()) {
            config_.sourceDeviceId = GetDefaultRenderDeviceId();
            if (config_.sourceDeviceId.empty() && !devices_.empty()) config_.sourceDeviceId = devices_[0].id;
        }

        if (config_.outputs.empty()) {
            SavedOutput initial;
            for (const auto& device : devices_) {
                if (device.id != config_.sourceDeviceId) {
                    initial.deviceId = device.id;
                    break;
                }
            }
            config_.outputs.push_back(initial);
        }

        CreateRowsFromConfig();
        PopulateAllCombos();
        ApplyLanguage();
        RegisterNotifications();
        Layout();
        return true;
    }

    void OnDestroy() {
        StopAudio();
        CollectConfigFromUi();
        if (config_.saveSettings) {
            SaveConfig(config_);
        } else {
            DeleteConfig();
        }
        UnregisterNotifications();
        DestroyRows();
        if (headingFont_) DeleteObject(headingFont_);
        if (font_) DeleteObject(font_);
        if (backgroundBrush_) DeleteObject(backgroundBrush_);
        headingFont_ = nullptr;
        font_ = nullptr;
        backgroundBrush_ = nullptr;
    }

    void RegisterNotifications() {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&notificationEnumerator_));
        if (FAILED(hr) || !notificationEnumerator_) return;

        notificationClient_ = new DeviceNotificationClient([window = hwnd_] {
            if (IsWindow(window)) PostMessageW(window, WM_APP_DEVICES_CHANGED, 0, 0);
        });
        notificationEnumerator_->RegisterEndpointNotificationCallback(notificationClient_);
    }

    void UnregisterNotifications() {
        if (notificationEnumerator_ && notificationClient_) {
            notificationEnumerator_->UnregisterEndpointNotificationCallback(notificationClient_);
        }
        if (notificationClient_) {
            notificationClient_->Release();
            notificationClient_ = nullptr;
        }
        if (notificationEnumerator_) {
            notificationEnumerator_->Release();
            notificationEnumerator_ = nullptr;
        }
    }

    void CreateRowsFromConfig() {
        DestroyRows();
        for (const auto& saved : config_.outputs) AddOutputRow(saved.deviceId, saved.mode, false);
        if (rows_.empty()) AddOutputRow(L"", ChannelMode::Stereo, false);
    }

    void DestroyRows() {
        for (auto& row : rows_) {
            if (row.deviceCombo) DestroyWindow(row.deviceCombo);
            if (row.modeCombo) DestroyWindow(row.modeCombo);
            if (row.removeButton) DestroyWindow(row.removeButton);
        }
        rows_.clear();
    }

    void AddOutputRow(const std::wstring& deviceId = L"",
                      ChannelMode mode = ChannelMode::Stereo,
                      bool selectDefault = true) {
        OutputRow row;
        row.deviceId = deviceId;
        row.mode = mode;

        if (selectDefault && row.deviceId.empty()) {
            for (const auto& device : devices_) {
                if (device.id == config_.sourceDeviceId) continue;
                bool alreadyUsed = false;
                for (const auto& existing : rows_) {
                    if (existing.deviceId == device.id) { alreadyUsed = true; break; }
                }
                if (!alreadyUsed) {
                    row.deviceId = device.id;
                    break;
                }
            }
        }

        row.deviceCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                           0, 0, 420, 260, outputPanel_.hwnd(),
                                           reinterpret_cast<HMENU>(IDC_OUTPUT_DEVICE), instance_, nullptr);
        row.modeCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        0, 0, 180, 180, outputPanel_.hwnd(),
                                        reinterpret_cast<HMENU>(IDC_OUTPUT_MODE), instance_, nullptr);
        row.removeButton = CreateWindowExW(0, L"BUTTON", L"−",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                            0, 0, 38, 34, outputPanel_.hwnd(),
                                            reinterpret_cast<HMENU>(IDC_OUTPUT_REMOVE), instance_, nullptr);
        SetFont(row.deviceCombo, font_);
        SetFont(row.modeCombo, font_);
        SetFont(row.removeButton, headingFont_);
        ApplyDarkTheme(row.deviceCombo);
        ApplyDarkTheme(row.modeCombo);
        ApplyDarkTheme(row.removeButton);
        rows_.push_back(row);

        PopulateDeviceCombo(rows_.back().deviceCombo, rows_.back().deviceId);
        PopulateModeCombo(rows_.back().modeCombo, TextFor(config_.language), rows_.back().mode);
        LayoutRows();
    }

    void RemoveOutputRow(HWND control) {
        auto it = std::find_if(rows_.begin(), rows_.end(), [&](const OutputRow& row) {
            return row.removeButton == control;
        });
        if (it == rows_.end()) return;
        DestroyWindow(it->deviceCombo);
        DestroyWindow(it->modeCombo);
        DestroyWindow(it->removeButton);
        rows_.erase(it);
        if (rows_.empty()) AddOutputRow();
        outputPanel_.ResetScroll();
        LayoutRows();
    }

    void RefreshDevices() {
        std::wstring source = config_.sourceDeviceId;
        if (source.empty() && sourceCombo_) {
            int i = ComboSelectedDeviceVectorIndex(sourceCombo_);
            if (i >= 0 && i < static_cast<int>(devices_.size())) source = devices_[i].id;
        }
        for (auto& row : rows_) {
            int i = row.deviceCombo ? ComboSelectedDeviceVectorIndex(row.deviceCombo) : -1;
            if (i >= 0 && i < static_cast<int>(devices_.size())) row.deviceId = devices_[i].id;
        }

        devices_ = EnumerateRenderDevices();
        if (source.empty()) source = GetDefaultRenderDeviceId();
        if (source.empty() && !devices_.empty()) source = devices_[0].id;
        config_.sourceDeviceId = source;

        if (sourceCombo_) PopulateDeviceCombo(sourceCombo_, config_.sourceDeviceId);
        for (auto& row : rows_) {
            if (row.deviceCombo) PopulateDeviceCombo(row.deviceCombo, row.deviceId);
        }
    }

    void PopulateAllCombos() {
        PopulateDeviceCombo(sourceCombo_, config_.sourceDeviceId);
        PopulateModeCombo(sourceMode_, TextFor(config_.language), config_.sourceMode);
        for (auto& row : rows_) {
            PopulateDeviceCombo(row.deviceCombo, row.deviceId);
            PopulateModeCombo(row.modeCombo, TextFor(config_.language), row.mode);
        }
    }

    void PopulateDeviceCombo(HWND combo, const std::wstring& selectedId) {
        if (!combo) return;
        SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);

        int selected = -1;
        for (size_t i = 0; i < devices_.size(); ++i) {
            std::wstring label = devices_[i].name;
            if (devices_[i].isDefault) label += config_.language == UiLanguage::English ? L" (default)" : L" (по умолчанию)";
            const int item = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
            SendMessageW(combo, CB_SETITEMDATA, item, static_cast<LPARAM>(i + 1));
            if (devices_[i].id == selectedId) selected = item;
        }

        if (selected < 0 && !selectedId.empty()) {
            const int item = static_cast<int>(SendMessageW(combo, CB_INSERTSTRING, 0,
                                      reinterpret_cast<LPARAM>(TextFor(config_.language).unavailable)));
            SendMessageW(combo, CB_SETITEMDATA, item, 0);
            selected = item;
        }
        if (selected < 0 && !devices_.empty()) selected = 0;
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(combo, nullptr, TRUE);
    }

    bool IsDeviceAvailable(const std::wstring& id) const {
        return std::any_of(devices_.begin(), devices_.end(), [&](const AudioDeviceInfo& d) { return d.id == id; });
    }

    void ApplyLanguage() {
        const Texts& t = TextFor(config_.language);
        SetWindowTextW(sourceLabel_, t.mainOutput);
        SetWindowTextW(outputsLabel_, t.additionalOutputs);
        SetWindowTextW(startButton_, t.start);
        SetWindowTextW(stopButton_, t.stop);
        SetWindowTextW(statusLabel_, engine_.IsRunning() ? t.running : t.ready);

        const ChannelMode sourceMode = ComboSelectedMode(sourceMode_);
        PopulateModeCombo(sourceMode_, t, sourceMode);
        for (auto& row : rows_) {
            row.mode = ComboSelectedMode(row.modeCombo);
            PopulateModeCombo(row.modeCombo, t, row.mode);
        }
        PopulateDeviceCombo(sourceCombo_, config_.sourceDeviceId);
        for (auto& row : rows_) PopulateDeviceCombo(row.deviceCombo, row.deviceId);
    }

    void CollectConfigFromUi() {
        int sourceIndex = ComboSelectedDeviceVectorIndex(sourceCombo_);
        if (sourceIndex >= 0 && sourceIndex < static_cast<int>(devices_.size())) {
            config_.sourceDeviceId = devices_[sourceIndex].id;
        }
        config_.sourceMode = ComboSelectedMode(sourceMode_);
        config_.outputs.clear();
        for (auto& row : rows_) {
            int index = ComboSelectedDeviceVectorIndex(row.deviceCombo);
            if (index >= 0 && index < static_cast<int>(devices_.size())) row.deviceId = devices_[index].id;
            row.mode = ComboSelectedMode(row.modeCombo);
            config_.outputs.push_back({row.deviceId, row.mode});
        }
    }

    void OnCommand(int id, int code, HWND control) {
        if (id == IDC_REFRESH && code == BN_CLICKED) {
            if (!engine_.IsRunning()) RefreshDevices();
            return;
        }
        if (id == IDC_SETTINGS && code == BN_CLICKED) {
            if (engine_.IsRunning()) return;
            CollectConfigFromUi();
            SettingsDialog dialog;
            if (dialog.Show(hwnd_, instance_, config_)) {
                ApplyLanguage();
                if (config_.saveSettings) {
                    CollectConfigFromUi();
                    if (!SaveConfig(config_)) {
                        MessageBoxW(hwnd_, TextFor(config_.language).configSaveFailed,
                                    TextFor(config_.language).errorTitle, MB_OK | MB_ICONWARNING);
                    }
                } else {
                    DeleteConfig();
                }
                Layout();
            }
            return;
        }
        if (id == IDC_ADD_OUTPUT && code == BN_CLICKED) {
            AddOutputRow();
            return;
        }
        if (id == IDC_START && code == BN_CLICKED) {
            StartAudio();
            return;
        }
        if (id == IDC_STOP && code == BN_CLICKED) {
            StopAudio();
            return;
        }
        if (id == IDC_SOURCE_DEVICE && code == CBN_SELCHANGE) {
            const int index = ComboSelectedDeviceVectorIndex(sourceCombo_);
            if (index >= 0 && index < static_cast<int>(devices_.size())) config_.sourceDeviceId = devices_[index].id;
            return;
        }
        if (id == IDC_SOURCE_MODE && code == CBN_SELCHANGE) {
            config_.sourceMode = ComboSelectedMode(sourceMode_);
            return;
        }

        for (auto& row : rows_) {
            if (control == row.deviceCombo && code == CBN_SELCHANGE) {
                const int index = ComboSelectedDeviceVectorIndex(row.deviceCombo);
                if (index >= 0 && index < static_cast<int>(devices_.size())) row.deviceId = devices_[index].id;
                return;
            }
            if (control == row.modeCombo && code == CBN_SELCHANGE) {
                row.mode = ComboSelectedMode(row.modeCombo);
                return;
            }
            if (control == row.removeButton && code == BN_CLICKED) {
                RemoveOutputRow(control);
                return;
            }
        }
    }

    void StartAudio() {
        CollectConfigFromUi();
        const Texts& t = TextFor(config_.language);
        if (!IsDeviceAvailable(config_.sourceDeviceId)) {
            MessageBoxW(hwnd_, t.selectSource, t.errorTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        std::vector<OutputConfig> outputs;
        std::set<std::wstring> seen;
        for (const auto& row : rows_) {
            if (!IsDeviceAvailable(row.deviceId)) continue;
            if (row.deviceId == config_.sourceDeviceId) {
                MessageBoxW(hwnd_, t.sameAsSource, t.errorTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            if (!seen.insert(row.deviceId).second) {
                MessageBoxW(hwnd_, t.duplicateOutput, t.errorTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            outputs.push_back({row.deviceId, row.mode});
        }
        if (outputs.empty()) {
            MessageBoxW(hwnd_, t.addOutput, t.errorTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        std::wstring error;
        const bool ok = engine_.Start(
            config_.sourceDeviceId, config_.sourceMode, outputs,
            [window = hwnd_](const std::wstring& message) {
                if (!IsWindow(window)) return;
                auto* copy = new std::wstring(message);
                if (!PostMessageW(window, WM_APP_AUDIO_ERROR, 0, reinterpret_cast<LPARAM>(copy))) {
                    delete copy;
                }
            }, error);
        if (!ok) {
            MessageBoxW(hwnd_, error.c_str(), t.errorTitle, MB_OK | MB_ICONERROR);
            return;
        }

        SetRunningUi(true);
    }

    void StopAudio() {
        engine_.Stop();
        SetRunningUi(false);
    }

    void SetRunningUi(bool running) {
        const Texts& t = TextFor(config_.language);
        SetWindowTextW(statusLabel_, running ? t.running : t.ready);
        EnableWindow(startButton_, !running);
        EnableWindow(stopButton_, running);
        EnableWindow(refreshButton_, !running);
        EnableWindow(settingsButton_, !running);
        EnableWindow(sourceCombo_, !running);
        EnableWindow(sourceMode_, !running);
        EnableWindow(addButton_, !running);
        for (auto& row : rows_) {
            EnableWindow(row.deviceCombo, !running);
            EnableWindow(row.modeCombo, !running);
            EnableWindow(row.removeButton, !running);
        }
    }

    void Layout() {
        if (!hwnd_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int margin = 28;
        const int gap = 12;
        const int toolbarY = 16;

        SetWindowPos(settingsButton_, nullptr, width - margin - 38, toolbarY, 38, 34, SWP_NOZORDER);
        SetWindowPos(refreshButton_, nullptr, width - margin - 38 - 8 - 38, toolbarY, 38, 34, SWP_NOZORDER);

        const int sourceLabelY = 66;
        SetWindowPos(sourceLabel_, nullptr, margin, sourceLabelY, width - margin * 2, 26, SWP_NOZORDER);
        const int sourceY = sourceLabelY + 34;
        const int modeWidth = 172;
        const int contentWidth = width - margin * 2;
        const int deviceWidth = std::max(220, contentWidth - modeWidth - gap);
        SetWindowPos(sourceCombo_, nullptr, margin, sourceY, deviceWidth, 300, SWP_NOZORDER);
        SetWindowPos(sourceMode_, nullptr, margin + deviceWidth + gap, sourceY, modeWidth, 220, SWP_NOZORDER);

        const int outputsHeaderY = sourceY + 64;
        SetWindowPos(outputsLabel_, nullptr, margin, outputsHeaderY, contentWidth - 50, 28, SWP_NOZORDER);
        SetWindowPos(addButton_, nullptr, width - margin - 38, outputsHeaderY - 6, 38, 34, SWP_NOZORDER);

        const int footerHeight = 74;
        const int panelY = outputsHeaderY + 38;
        const int panelHeight = std::max(100, height - panelY - footerHeight);
        SetWindowPos(outputPanel_.hwnd(), nullptr, margin, panelY, contentWidth, panelHeight, SWP_NOZORDER);

        const int footerY = height - 56;
        SetWindowPos(statusLabel_, nullptr, margin, footerY + 8, 260, 28, SWP_NOZORDER);
        SetWindowPos(stopButton_, nullptr, width - margin - 120, footerY, 120, 38, SWP_NOZORDER);
        SetWindowPos(startButton_, nullptr, width - margin - 120 - gap - 120, footerY, 120, 38, SWP_NOZORDER);
        LayoutRows();
    }

    void LayoutRows() {
        if (!outputPanel_.hwnd()) return;
        RECT rc{};
        GetClientRect(outputPanel_.hwnd(), &rc);
        const int width = rc.right - rc.left;
        const int gap = 10;
        const int margin = 10;
        const int buttonWidth = 38;
        const int rowHeight = 54;
        const int contentHeight = 20 + static_cast<int>(rows_.size()) * rowHeight;
        const int panelHeight = rc.bottom - rc.top;
        const int rightReserve = contentHeight > panelHeight ? GetSystemMetrics(SM_CXVSCROLL) + 8 : 0;
        const int modeWidth = std::min(170, std::max(130, width / 4));
        const int deviceWidth = std::max(160, width - margin * 2 - rightReserve - buttonWidth - modeWidth - gap * 2);
        const int top = 10 - outputPanel_.offset();

        for (size_t i = 0; i < rows_.size(); ++i) {
            const int y = top + static_cast<int>(i) * rowHeight;
            SetWindowPos(rows_[i].deviceCombo, nullptr, margin, y, deviceWidth, 260, SWP_NOZORDER);
            SetWindowPos(rows_[i].modeCombo, nullptr, margin + deviceWidth + gap, y, modeWidth, 180, SWP_NOZORDER);
            SetWindowPos(rows_[i].removeButton, nullptr,
                         margin + deviceWidth + gap + modeWidth + gap, y, buttonWidth, 34, SWP_NOZORDER);
        }
        outputPanel_.SetContentHeight(contentHeight);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND sourceLabel_ = nullptr;
    HWND sourceCombo_ = nullptr;
    HWND sourceMode_ = nullptr;
    HWND outputsLabel_ = nullptr;
    HWND addButton_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND startButton_ = nullptr;
    HWND stopButton_ = nullptr;
    ScrollPanel outputPanel_;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;

    AppConfig config_;
    std::vector<AudioDeviceInfo> devices_;
    std::vector<OutputRow> rows_;
    AudioEngine engine_;

    IMMDeviceEnumerator* notificationEnumerator_ = nullptr;
    DeviceNotificationClient* notificationClient_ = nullptr;
};

} // namespace
} // namespace audiodup

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"COM initialization failed.", L"Audio Duplicate", MB_OK | MB_ICONERROR);
        return 1;
    }

    audiodup::MainWindow window(instance);
    if (!window.Create()) {
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
