#pragma once

#include <windows.h>

#include <string>

namespace cliplite::app {

constexpr int kAppIconId = 101;  // IDI_APPICON in assets/resource.h

// RAII wrapper around the Windows notification-area (tray) icon.
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Adds the icon. Returns false if Shell_NotifyIcon fails.
    bool add(HWND hwnd, UINT callback_message, const std::wstring& tip);
    void remove();

    // Shows a small balloon notification next to the icon.
    bool notify(const std::wstring& title, const std::wstring& body);

    // Updates the hover tooltip (e.g. recording state). No-op when not added.
    bool set_tip(const std::wstring& tip);

    bool added() const { return added_; }

private:
    HWND hwnd_ = nullptr;
    UINT callback_ = 0;
    bool added_ = false;
};

}  // namespace cliplite::app
