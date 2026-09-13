#include "cliplite/hotkeys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace cliplite {
namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

uint32_t key_to_vk(const std::string& name) {
    if (name.size() >= 2 && name[0] == 'F') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return 0x70 + static_cast<uint32_t>(n - 1);  // VK_F1 = 0x70
        return 0;
    }
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') return static_cast<uint32_t>(c);
        if (c >= '0' && c <= '9') return static_cast<uint32_t>(c);
    }
    static const std::map<std::string, uint32_t> special = {
        {"SPACE", 0x20},      {"ENTER", 0x0D},     {"ESC", 0x1B},
        {"ESCAPE", 0x1B},     {"TAB", 0x09},       {"BACKSPACE", 0x08},
        {"HOME", 0x24},       {"END", 0x23},       {"PGUP", 0x21},
        {"PGDN", 0x22},       {"LEFT", 0x25},      {"RIGHT", 0x27},
        {"UP", 0x26},         {"DOWN", 0x28},      {"INSERT", 0x2D},
        {"DELETE", 0x2E},     {"PRINTSCREEN", 0x2C},
    };
    const auto it = special.find(name);
    return it == special.end() ? 0 : it->second;
}

std::string vk_to_key_name(uint32_t vk) {
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);  // F1..F24
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    static const std::map<uint32_t, std::string> special = {
        {0x20, "Space"},    {0x0D, "Enter"},     {0x1B, "Esc"},
        {0x09, "Tab"},      {0x08, "Backspace"}, {0x24, "Home"},
        {0x23, "End"},      {0x21, "PgUp"},      {0x22, "PgDn"},
        {0x25, "Left"},     {0x27, "Right"},     {0x26, "Up"},
        {0x28, "Down"},     {0x2D, "Insert"},    {0x2E, "Delete"},
        {0x2C, "PrintScreen"},
    };
    const auto it = special.find(vk);
    return it == special.end() ? std::string() : it->second;
}

}  // namespace

std::optional<Hotkey> parse_hotkey(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == '+') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    if (parts.empty()) return std::nullopt;

    Hotkey hk;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string m = upper(trim(parts[i]));
        if (m == "CTRL" || m == "CONTROL") hk.modifiers |= Hotkey::ModCtrl;
        else if (m == "ALT") hk.modifiers |= Hotkey::ModAlt;
        else if (m == "SHIFT") hk.modifiers |= Hotkey::ModShift;
        else if (m == "WIN" || m == "WINDOWS") hk.modifiers |= Hotkey::ModWin;
        else return std::nullopt;
    }

    const std::string key = upper(trim(parts.back()));
    hk.vk = key_to_vk(key);
    if (hk.vk == 0) return std::nullopt;
    return hk;
}

std::string to_string(const Hotkey& hk) {
    std::string out;
    if (hk.modifiers & Hotkey::ModCtrl) out += "Ctrl+";
    if (hk.modifiers & Hotkey::ModAlt) out += "Alt+";
    if (hk.modifiers & Hotkey::ModShift) out += "Shift+";
    if (hk.modifiers & Hotkey::ModWin) out += "Win+";
    out += vk_to_key_name(hk.vk);
    return out;
}

}  // namespace cliplite
