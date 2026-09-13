#include "test_main.h"

#include "cliplite/util/string.h"

using namespace cliplite::util;

TEST(sanitize_replaces_illegal_chars) {
    CHECK_EQ(sanitize_filename("mine:craft*2026?"), "mine_craft_2026_");
}

TEST(sanitize_trims_trailing_dots_and_spaces) {
    CHECK_EQ(sanitize_filename("clip... "), "clip");
}

TEST(sanitize_empty_input) {
    CHECK_EQ(sanitize_filename(""), "clip");
}

TEST(sanitize_preserves_valid_name) {
    CHECK_EQ(sanitize_filename("Minecraft_2026-08-21_21-42-18"), "Minecraft_2026-08-21_21-42-18");
}

TEST(exe_base_name_strips_path_and_extension) {
    CHECK_EQ(exe_base_name("C:\\Games\\VALORANT\\VALORANT-Win64-Shipping.exe"),
             "VALORANT-Win64-Shipping");
}

TEST(clean_game_name_strips_suffix) {
    CHECK_EQ(clean_game_name("Minecraft 1.21.4 - Launcher", "javaw.exe"), "Minecraft");
}

TEST(clean_game_name_strips_dirty_marker_and_version) {
    CHECK_EQ(clean_game_name("Minecraft* 1.21.11 - Multiplayer (3rd-party Server)", "javaw.exe"),
             "Minecraft");
}

TEST(clean_game_name_preserves_single_digit_titles) {
    CHECK_EQ(clean_game_name("Portal 2", "portal2.exe"), "Portal 2");
}

TEST(clean_game_name_falls_back_to_exe) {
    CHECK_EQ(clean_game_name("", "VALORANT-Win64-Shipping.exe"), "VALORANT-Win64-Shipping");
}
