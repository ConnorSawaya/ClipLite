#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace cliplite::detection {

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exe_path;
    std::string exe_name;   // lowercase basename, e.g. "notepad.exe"
    std::wstring title;
    int width = 0;
    int height = 0;
    bool minimized = false;
    bool visible = false;
    bool fullscreen = false;  // covers an entire monitor (exclusive or borderless)
    bool valid = false;
};

WindowInfo query_window(HWND hwnd);
WindowInfo query_foreground();

// Top-level visible windows with titles, in z-order, for the source picker.
// Skips our own windows, minimized/hidden/untitled ones. Best-effort snapshot.
std::vector<WindowInfo> enum_visible_windows();

}  // namespace cliplite::detection
