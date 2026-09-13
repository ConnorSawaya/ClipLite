#include "cliplite/app/startup.h"

#include <windows.h>

#include <string>

namespace cliplite::app {

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kValueName = L"ClipLite";
}  // namespace

bool set_startup_enabled(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enabled) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring value = L"\"" + std::wstring(path) + L"\"";
        ok = (RegSetValueExW(key, kValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
              ERROR_SUCCESS);
    } else {
        const LONG r = RegDeleteValueW(key, kValueName);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

bool startup_enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = 0;
    const LONG r = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, &size);
    RegCloseKey(key);
    return (r == ERROR_SUCCESS);
}

}  // namespace cliplite::app
