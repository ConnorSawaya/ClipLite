#include "cliplite/detection/window_info.h"

#include <algorithm>
#include <cctype>

#include "cliplite/util/win_utf8.h"

namespace cliplite::detection {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

WindowInfo query_window(HWND hwnd) {
    WindowInfo info;
    if (!hwnd || !IsWindow(hwnd)) return info;
    info.hwnd = hwnd;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.pid = pid;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH]{};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
            info.exe_path = path;
        }
        CloseHandle(proc);
    }

    if (!info.exe_path.empty()) {
        const auto slash = info.exe_path.find_last_of(L"\\/");
        std::wstring base = (slash == std::wstring::npos) ? info.exe_path
                                                          : info.exe_path.substr(slash + 1);
        info.exe_name = to_lower(cliplite::util::wide_to_utf8(base));
    }

    const int title_len = GetWindowTextLengthW(hwnd);
    if (title_len > 0) {
        std::wstring title(static_cast<size_t>(title_len) + 1, L'\0');
        const int got = GetWindowTextW(hwnd, title.data(), title_len + 1);
        title.resize(static_cast<size_t>(got));
        info.title = title;
    }

    info.minimized = (IsIconic(hwnd) != FALSE);
    info.visible = (IsWindowVisible(hwnd) != FALSE);

    RECT rc{};
    if (GetWindowRect(hwnd, &rc)) {
        info.width = static_cast<int>(rc.right - rc.left);
        info.height = static_cast<int>(rc.bottom - rc.top);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT& m = mi.rcMonitor;
            info.fullscreen = (rc.left <= m.left && rc.top <= m.top && rc.right >= m.right &&
                               rc.bottom >= m.bottom);
        }
    }

    info.valid = true;
    return info;
}

WindowInfo query_foreground() {
    return query_window(GetForegroundWindow());
}

namespace {
BOOL CALLBACK collect_visible(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lp);
    if (out->size() >= 64) return FALSE;  // bound the picker, z-order kept
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    WindowInfo info = query_window(hwnd);
    if (!info.valid || info.title.empty() || info.exe_name.empty()) return TRUE;
    if (info.pid == GetCurrentProcessId()) return TRUE;  // never capture ourselves
    out->push_back(std::move(info));
    return TRUE;
}
}  // namespace

std::vector<WindowInfo> enum_visible_windows() {
    std::vector<WindowInfo> out;
    EnumWindows(collect_visible, reinterpret_cast<LPARAM>(&out));
    return out;
}

}  // namespace cliplite::detection
