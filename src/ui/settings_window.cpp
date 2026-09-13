#include "cliplite/ui/settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cstdio>
#include <cstdlib>

#include "cliplite/log.h"

namespace cliplite::ui {

namespace {

constexpr UINT IDC_REPLAY = 300;
constexpr UINT IDC_FPS = 301;
constexpr UINT IDC_BITRATE = 302;
constexpr UINT IDC_HOTKEY = 303;
constexpr UINT IDC_DESKTOP = 304;
constexpr UINT IDC_MIC = 305;
constexpr UINT IDC_STARTUP = 306;
constexpr UINT IDC_NOTIFY = 307;
constexpr UINT IDC_SAVE = 308;
constexpr UINT IDC_CANCEL = 309;
constexpr UINT IDC_QUALITY = 310;

const wchar_t* kClass = L"ClipLiteSettingsWindow";
const COLORREF kBg = RGB(10, 10, 10);
const COLORREF kInput = RGB(13, 13, 13);
const COLORREF kLine = RGB(38, 38, 38);
const COLORREF kText = RGB(240, 240, 240);
const COLORREF kMuted = RGB(140, 140, 140);
const COLORREF kDisabledText = RGB(90, 90, 90);
const COLORREF kPrimary = RGB(245, 245, 245);
const COLORREF kPrimaryHot = RGB(255, 255, 255);
const COLORREF kPrimaryPressed = RGB(210, 210, 210);
const COLORREF kSecondary = RGB(22, 22, 22);
const COLORREF kSecondaryHot = RGB(32, 32, 32);
const COLORREF kSecondaryPressed = RGB(12, 12, 12);

int get_int(HWND edit) {
    wchar_t buf[32]{};
    GetWindowTextW(edit, buf, 32);
    return static_cast<int>(std::wcstol(buf, nullptr, 10));
}

void set_int(HWND edit, int v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", v);
    SetWindowTextW(edit, buf);
}

void set_text(HWND edit, const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    SetWindowTextW(edit, w.c_str());
}

std::string get_text(HWND edit) {
    wchar_t buf[128]{};
    GetWindowTextW(edit, buf, 128);
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

HWND make_label(HWND parent, HINSTANCE h, const wchar_t* text, int x, int y, int w) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20, parent,
                                 nullptr, h, nullptr);
    SendMessageW(label, WM_SETFONT, 0, FALSE);  // font applied by caller
    return label;
}

int preset_index(const std::string& preset) {
    if (preset == "low") return 0;
    if (preset == "high") return 2;
    if (preset == "custom") return 3;
    return 1;
}

}  // namespace

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (body_font_) DeleteObject(body_font_);
    if (section_font_) DeleteObject(section_font_);
    if (bg_brush_) DeleteObject(bg_brush_);
    if (edit_brush_) DeleteObject(edit_brush_);
    if (divider_brush_) DeleteObject(divider_brush_);
    if (primary_brush_) DeleteObject(primary_brush_);
    if (primary_hot_brush_) DeleteObject(primary_hot_brush_);
    if (primary_pressed_brush_) DeleteObject(primary_pressed_brush_);
    if (secondary_brush_) DeleteObject(secondary_brush_);
    if (secondary_hot_brush_) DeleteObject(secondary_hot_brush_);
    if (secondary_pressed_brush_) DeleteObject(secondary_pressed_brush_);
    if (disabled_brush_) DeleteObject(disabled_brush_);
    if (border_pen_) DeleteObject(border_pen_);
}

bool SettingsWindow::create(HINSTANCE hInstance, cliplite::Settings* settings,
                            const std::string& settings_path, HWND notify_hwnd, UINT notify_msg) {
    hinst_ = hInstance;
    settings_ = settings;
    settings_path_ = settings_path;
    notify_hwnd_ = notify_hwnd;
    notify_msg_ = notify_msg;

    bg_brush_ = CreateSolidBrush(kBg);
    edit_brush_ = CreateSolidBrush(kInput);
    divider_brush_ = CreateSolidBrush(kLine);
    primary_brush_ = CreateSolidBrush(kPrimary);
    primary_hot_brush_ = CreateSolidBrush(kPrimaryHot);
    primary_pressed_brush_ = CreateSolidBrush(kPrimaryPressed);
    secondary_brush_ = CreateSolidBrush(kSecondary);
    secondary_hot_brush_ = CreateSolidBrush(kSecondaryHot);
    secondary_pressed_brush_ = CreateSolidBrush(kSecondaryPressed);
    disabled_brush_ = CreateSolidBrush(RGB(24, 24, 24));
    border_pen_ = CreatePen(PS_SOLID, 1, kLine);
    body_font_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    section_font_ = CreateFontW(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = bg_brush_;
    wc.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kClass, L"Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 480, 525, nullptr, nullptr, hinst_,
                            this);
    if (!hwnd_) return false;

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &kBg, sizeof(kBg));

    create_controls(hwnd_);
    load_controls();
    return true;
}

void SettingsWindow::create_controls(HWND parent) {
    auto label = [&](const wchar_t* text, int x, int y, int w) {
        HWND c = make_label(parent, hinst_, text, x, y, w);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        return c;
    };
    auto edit = [&](UINT id, int x, int y, int w, bool number) {
        HWND c = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     (number ? ES_NUMBER : ES_AUTOHSCROLL),
                                 x, y, w, 24, parent, (HMENU)(INT_PTR)id, hinst_, nullptr);
        SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        return c;
    };
    auto check = [&](const wchar_t* text, UINT id, int x, int y) {
        HWND c = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, x, y, 220,
                                 20, parent, (HMENU)(INT_PTR)id, hinst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        return c;
    };

    constexpr int kLabelX = 20;
    constexpr int kFieldX = 210;
    constexpr int kFieldW = 150;

    label(L"Replay length (seconds)", kLabelX, 50, 180);
    edit(IDC_REPLAY, kFieldX, 48, kFieldW, true);
    label(L"FPS", kLabelX, 80, 180);
    edit(IDC_FPS, kFieldX, 78, kFieldW, true);
    label(L"Bitrate (Mbps)", kLabelX, 110, 180);
    edit(IDC_BITRATE, kFieldX, 108, kFieldW, true);
    label(L"Quality preset", kLabelX, 140, 180);
    combo_quality_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                                     kFieldX, 138, kFieldW + 40, 140, parent,
                                     (HMENU)(INT_PTR)IDC_QUALITY, hinst_, nullptr);
    SetWindowTheme(combo_quality_, L"DarkMode_Explorer", nullptr);
    SendMessageW(combo_quality_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
    for (const wchar_t* item : {L"Low", L"Balanced", L"High", L"Custom"}) {
        SendMessageW(combo_quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }

    check(L"Desktop audio", IDC_DESKTOP, kLabelX, 216);
    check(L"Microphone", IDC_MIC, kLabelX, 246);

    label(L"Save clip hotkey", kLabelX, 322, 180);
    edit(IDC_HOTKEY, kFieldX, 320, kFieldW, false);
    check(L"Start with Windows", IDC_STARTUP, kLabelX, 354);
    check(L"Notifications", IDC_NOTIFY, kLabelX, 384);

    btn_save_ = CreateWindowExW(0, L"BUTTON", L"Save",
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 20, 424, 110,
                                34, parent, (HMENU)(INT_PTR)IDC_SAVE, hinst_, nullptr);
    btn_cancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 140, 424, 110,
                                  34, parent, (HMENU)(INT_PTR)IDC_CANCEL, hinst_, nullptr);
    for (HWND b : {btn_save_, btn_cancel_}) {
        SetWindowSubclass(b, ButtonProc, 0, reinterpret_cast<DWORD_PTR>(this));
    }
}

void SettingsWindow::load_controls() {
    if (!hwnd_) return;
    set_int(edit_replay_, settings_->recording.replay_duration_sec);
    set_int(edit_fps_, settings_->recording.fps);
    set_int(edit_bitrate_, settings_->recording.bitrate_mbps);
    set_text(edit_hotkey_, settings_->hotkeys.save_clip);
    SendMessageW(chk_desktop_, BM_SETCHECK,
                 settings_->audio.desktop_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chk_mic_, BM_SETCHECK,
                 settings_->audio.mic_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chk_startup_, BM_SETCHECK,
                 settings_->general.start_with_windows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chk_notify_, BM_SETCHECK,
                 settings_->general.notifications ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(combo_quality_, CB_SETCURSEL, preset_index(settings_->general.quality_preset), 0);
}

void SettingsWindow::apply_preset(int index) {
    if (index == 0) {
        set_int(edit_replay_, 60);
        set_int(edit_fps_, 30);
        set_int(edit_bitrate_, 8);
    } else if (index == 1) {
        set_int(edit_replay_, 60);
        set_int(edit_fps_, 60);
        set_int(edit_bitrate_, 15);
    } else if (index == 2) {
        set_int(edit_replay_, 60);
        set_int(edit_fps_, 60);
        set_int(edit_bitrate_, 25);
    }
}

void SettingsWindow::save() {
    int replay = get_int(edit_replay_);
    int fps = get_int(edit_fps_);
    int bitrate = get_int(edit_bitrate_);
    if (replay < 5) replay = 60;
    if (fps < 1) fps = 60;
    if (fps > 240) fps = 240;
    if (bitrate < 1) bitrate = 15;
    if (bitrate > 200) bitrate = 200;
    settings_->recording.replay_duration_sec = replay;
    settings_->recording.fps = fps;
    settings_->recording.bitrate_mbps = bitrate;

    const std::string hotkey = get_text(edit_hotkey_);
    if (!hotkey.empty()) settings_->hotkeys.save_clip = hotkey;

    settings_->audio.desktop_enabled =
        SendMessageW(chk_desktop_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_->audio.mic_enabled = SendMessageW(chk_mic_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_->general.start_with_windows =
        SendMessageW(chk_startup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_->general.notifications =
        SendMessageW(chk_notify_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    const LRESULT sel = SendMessageW(combo_quality_, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel <= 3) {
        settings_->general.quality_preset = sel == 0   ? "low"
                                             : sel == 1 ? "balanced"
                                             : sel == 2 ? "high"
                                                        : "custom";
    }

    if (settings_->save(settings_path_)) {
        CL_INFO("Settings", "saved to " + settings_path_);
    } else {
        CL_ERROR("Settings", "failed to save settings");
    }
    if (notify_hwnd_) PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
    ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::show() {
    if (!hwnd_) return;
    load_controls();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::update_hover(HWND control, LPARAM) {
    const UINT id = static_cast<UINT>(GetDlgCtrlID(control));
    if (hot_id_ == id) return;
    if (hot_id_) {
        if (HWND old = GetDlgItem(hwnd_, static_cast<int>(hot_id_)))
            InvalidateRect(old, nullptr, TRUE);
    }
    hot_id_ = id;
    if (HWND now = GetDlgItem(hwnd_, static_cast<int>(id))) InvalidateRect(now, nullptr, TRUE);
}

void SettingsWindow::clear_hot() {
    if (!hot_id_) return;
    if (HWND old = GetDlgItem(hwnd_, static_cast<int>(hot_id_)))
        InvalidateRect(old, nullptr, TRUE);
    hot_id_ = 0;
}

void SettingsWindow::draw_button(const DRAWITEMSTRUCT& item) {
    RECT rc = item.rcItem;
    const bool primary = item.CtlID == IDC_SAVE;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool hot = hot_id_ == item.CtlID && !disabled && !pressed;

    HBRUSH fill = primary ? (pressed ? primary_pressed_brush_
                                     : hot ? primary_hot_brush_ : primary_brush_)
                          : (pressed ? secondary_pressed_brush_
                                     : hot ? secondary_hot_brush_ : secondary_brush_);
    if (disabled) fill = disabled_brush_;

    HGDIOBJ old_brush = SelectObject(item.hDC, fill);
    HGDIOBJ old_pen = SelectObject(item.hDC, primary ? GetStockObject(NULL_PEN) : border_pen_);
    RoundRect(item.hDC, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
    SelectObject(item.hDC, old_pen);
    SelectObject(item.hDC, old_brush);

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? kDisabledText : (primary ? kBg : kText));
    DrawTextW(item.hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void SettingsWindow::on_paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc = ps.rcPaint;
    FillRect(dc, &rc, bg_brush_);

    struct Section {
        const wchar_t* title;
        int y_text;
        int y_line;
    };
    const Section sections[] = {
        {L"RECORDING", 18, 38},
        {L"AUDIO", 186, 206},
        {L"GENERAL", 292, 312},
    };
    HGDIOBJ old = SelectObject(dc, section_font_);
    SetBkMode(dc, TRANSPARENT);
    for (const Section& s : sections) {
        SetTextColor(dc, kMuted);
        TextOutW(dc, 20, s.y_text, s.title, static_cast<int>(wcslen(s.title)));
        RECT line{20, s.y_line, rc.right - 20, s.y_line + 1};
        FillRect(dc, &line, divider_brush_);
    }
    SelectObject(dc, old);
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK SettingsWindow::ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SettingsWindow*>(ref);
    if (self) {
        switch (msg) {
            case WM_MOUSEMOVE: {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                self->update_hover(hwnd, lp);
                break;
            }
            case WM_MOUSELEAVE:
                self->clear_hot();
                break;
            default:
                break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            self->on_paint();
            return 0;
        case WM_COMMAND:
            switch (HIWORD(wp)) {
                case CBN_SELCHANGE:
                    if (LOWORD(wp) == IDC_QUALITY) {
                        const LRESULT sel = SendMessageW(self->combo_quality_, CB_GETCURSEL, 0, 0);
                        self->apply_preset(static_cast<int>(sel));
                    }
                    break;
                case BN_CLICKED:
                    if (LOWORD(wp) == IDC_SAVE) self->save();
                    if (LOWORD(wp) == IDC_CANCEL) ShowWindow(hwnd, SW_HIDE);
                    break;
                default:
                    break;
            }
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wp), kText);
            SetBkColor(reinterpret_cast<HDC>(wp), kInput);
            return reinterpret_cast<LRESULT>(self->edit_brush_);
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wp), kText);
            SetBkColor(reinterpret_cast<HDC>(wp), kBg);
            return reinterpret_cast<LRESULT>(self->bg_brush_);
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis && dis->CtlType == ODT_BUTTON) {
                self->draw_button(*dis);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace cliplite::ui
