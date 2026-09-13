#pragma once

#include <windows.h>

#include <string>

#include "cliplite/config.h"

namespace cliplite::ui {

// Black & white settings window: three grouped sections (RECORDING / AUDIO /
// GENERAL) with painted headers + dividers, dark inputs, and flat rounded
// owner-draw buttons. Persists to the INI file and posts a notification message
// so the app can re-apply the hotkey / startup state immediately.
class SettingsWindow {
public:
    SettingsWindow() = default;
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    bool create(HINSTANCE hInstance, cliplite::Settings* settings,
                const std::string& settings_path, HWND notify_hwnd, UINT notify_msg);
    void show();

private:
    void create_controls(HWND parent);
    void load_controls();
    void save();
    void apply_preset(int index);
    void on_paint();
    void draw_button(const DRAWITEMSTRUCT& item);
    void update_hover(HWND parent, LPARAM lp);
    void clear_hot();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND hwnd_ = nullptr;
    HWND edit_replay_ = nullptr;
    HWND edit_fps_ = nullptr;
    HWND edit_bitrate_ = nullptr;
    HWND edit_hotkey_ = nullptr;
    HWND combo_quality_ = nullptr;
    HWND chk_desktop_ = nullptr;
    HWND chk_mic_ = nullptr;
    HWND chk_startup_ = nullptr;
    HWND chk_notify_ = nullptr;
    HWND btn_save_ = nullptr;
    HWND btn_cancel_ = nullptr;

    HFONT body_font_ = nullptr;
    HFONT section_font_ = nullptr;
    HBRUSH bg_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HBRUSH divider_brush_ = nullptr;
    HBRUSH primary_brush_ = nullptr;
    HBRUSH primary_hot_brush_ = nullptr;
    HBRUSH primary_pressed_brush_ = nullptr;
    HBRUSH secondary_brush_ = nullptr;
    HBRUSH secondary_hot_brush_ = nullptr;
    HBRUSH secondary_pressed_brush_ = nullptr;
    HBRUSH disabled_brush_ = nullptr;
    HPEN border_pen_ = nullptr;

    UINT hot_id_ = 0;

    cliplite::Settings* settings_ = nullptr;
    std::string settings_path_;
    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
    HINSTANCE hinst_ = nullptr;
};

}  // namespace cliplite::ui
