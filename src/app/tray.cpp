#include "cliplite/app/tray.h"

#include <shellapi.h>

namespace cliplite::app {

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::add(HWND hwnd, UINT callback_message, const std::wstring& tip) {
    if (added_) remove();
    hwnd_ = hwnd;
    callback_ = callback_message;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = callback_message;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId));
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    added_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

bool TrayIcon::set_tip(const std::wstring& tip) {
    if (!added_) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

bool TrayIcon::notify(const std::wstring& title, const std::wstring& body) {
    if (!added_) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 5000;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

}  // namespace cliplite::app
