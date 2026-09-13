#include "test_main.h"

#include "cliplite/hotkeys.h"

using namespace cliplite;

TEST(parse_plain_f8) {
    const auto hk = parse_hotkey("F8");
    CHECK(hk.has_value());
    CHECK_EQ(hk->vk, 0x77u);  // VK_F8
    CHECK_EQ(hk->modifiers, 0u);
}

TEST(parse_ctrl_shift_s) {
    const auto hk = parse_hotkey("Ctrl+Shift+S");
    CHECK(hk.has_value());
    CHECK_EQ(hk->vk, static_cast<uint32_t>('S'));
    CHECK(hk->modifiers == (Hotkey::ModCtrl | Hotkey::ModShift));
}

TEST(parse_rejects_invalid) {
    CHECK(!parse_hotkey("Foo+Bar").has_value());
    CHECK(!parse_hotkey("").has_value());
    CHECK(!parse_hotkey("Ctrl+").has_value());
    CHECK(!parse_hotkey("Ctrl+NotAKey").has_value());
}

TEST(hotkey_roundtrip_string) {
    const auto hk = parse_hotkey("Alt+F8");
    CHECK(hk.has_value());
    CHECK_EQ(to_string(*hk), std::string("Alt+F8"));
}

TEST(hotkey_conflict_detection) {
    const auto a = parse_hotkey("Ctrl+F8");
    const auto b = parse_hotkey("Ctrl+F8");
    const auto c = parse_hotkey("Alt+F8");
    CHECK(a.has_value() && b.has_value() && c.has_value());
    CHECK(*a == *b);
    CHECK(*a != *c);
}
