#include "cliplite/detection/game_namer.h"

#include <windows.h>

#include <vector>

#include "cliplite/util/string.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::detection {

namespace {

std::wstring version_string(const std::wstring& path, const wchar_t* key) {
    if (path.empty()) return {};
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return {};
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    }* translate = nullptr;
    UINT cbTranslate = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translate), &cbTranslate) ||
        cbTranslate < sizeof(LANGANDCODEPAGE) || !translate) {
        return {};
    }

    wchar_t sub[128]{};
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", translate[0].wLanguage,
               translate[0].wCodePage, key);
    wchar_t* value = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), sub, reinterpret_cast<void**>(&value), &len) || !value) {
        return {};
    }
    return std::wstring(value);
}

}  // namespace

std::string name_application(const std::wstring& exe_path, const std::wstring& window_title,
                             const std::string& exe_name) {
    // Generic runtimes carry misleading version descriptions ("Java(TM) Platform SE
    // binary"); derive the name from the window title instead.
    const bool generic_runtime = (exe_name == "javaw.exe" || exe_name == "java.exe" ||
                                  exe_name == "python.exe" || exe_name == "pythonw.exe");

    if (!generic_runtime) {
        const std::wstring desc = version_string(exe_path, L"FileDescription");
        if (!desc.empty()) return cliplite::util::wide_to_utf8(desc);

        const std::wstring product = version_string(exe_path, L"ProductName");
        if (!product.empty()) return cliplite::util::wide_to_utf8(product);
    }

    return cliplite::util::clean_game_name(cliplite::util::wide_to_utf8(window_title), exe_name);
}

}  // namespace cliplite::detection
