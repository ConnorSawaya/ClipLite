#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cliplite {

// A global hotkey represented as Windows modifier flags plus a virtual key code.
struct Hotkey {
    uint32_t modifiers = 0;
    uint32_t vk = 0;

    // Match Windows MOD_* bit values.
    static constexpr uint32_t ModAlt = 0x0001;
    static constexpr uint32_t ModCtrl = 0x0002;
    static constexpr uint32_t ModShift = 0x0004;
    static constexpr uint32_t ModWin = 0x0008;

    bool operator==(const Hotkey& other) const {
        return modifiers == other.modifiers && vk == other.vk;
    }
    bool operator!=(const Hotkey& other) const { return !(*this == other); }
    bool valid() const { return vk != 0; }
};

// Parses strings like "F8", "Ctrl+Shift+S", "Alt+F8". Returns nullopt on failure.
std::optional<Hotkey> parse_hotkey(const std::string& text);

// Serializes back to canonical form ("Ctrl+Shift+S").
std::string to_string(const Hotkey& hk);

}  // namespace cliplite
