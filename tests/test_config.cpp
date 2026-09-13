#include "test_main.h"

#include "cliplite/config.h"

#include <filesystem>
#include <fstream>

using namespace cliplite;

static std::string temp_path() {
    return (std::filesystem::temp_directory_path() / "cliplite_test_config.ini").string();
}

TEST(config_roundtrip_defaults) {
    const Settings s = Settings::defaults();
    const std::string p = temp_path();
    CHECK(s.save(p));
    Settings loaded;
    CHECK(loaded.load(p));
    CHECK_EQ(loaded.recording.replay_duration_sec, s.recording.replay_duration_sec);
    CHECK_EQ(loaded.recording.fps, s.recording.fps);
    CHECK_EQ(loaded.recording.codec, s.recording.codec);
    CHECK_EQ(loaded.hotkeys.save_clip, s.hotkeys.save_clip);
    CHECK_EQ(loaded.general.close_to_tray, s.general.close_to_tray);
    CHECK_EQ(loaded.audio.mic_volume, s.audio.mic_volume);
    CHECK_EQ(loaded.recording.display_index, s.recording.display_index);
    CHECK_EQ(loaded.recording.source_mode, s.recording.source_mode);
    CHECK_EQ(loaded.recording.source_window_exe, s.recording.source_window_exe);
    CHECK_EQ(loaded.recording.source_window_title, s.recording.source_window_title);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(config_source_roundtrip) {
    Settings s = Settings::defaults();
    s.recording.source_mode = "window";
    s.recording.source_window_exe = "spotify.exe";
    s.recording.source_window_title = "Spotify Premium";
    s.recording.display_index = 2;
    const std::string p = temp_path();
    CHECK(s.save(p));
    Settings loaded;
    CHECK(loaded.load(p));
    CHECK_EQ(loaded.recording.source_mode, "window");
    CHECK_EQ(loaded.recording.source_window_exe, "spotify.exe");
    CHECK_EQ(loaded.recording.source_window_title, "Spotify Premium");
    CHECK_EQ(loaded.recording.display_index, 2);
    // Unknown/garbage modes sanitize back to display.
    {
        std::ofstream out(p);
        out << "[recording]\nsource_mode=bogus\n";
    }
    Settings s2;
    CHECK(s2.load(p));
    CHECK_EQ(s2.recording.source_mode, "display");
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(config_load_missing_file_returns_false) {
    Settings s;
    CHECK(!s.load("Z:/nonexistent/cliplite_missing.ini"));
}

TEST(config_persists_game_settings) {
    Settings s = Settings::defaults();
    GameSettings gs;
    gs.replay_duration_sec = 30;
    gs.bitrate_mbps = 20;
    gs.mic_enabled = false;
    s.games["minecraft.exe"] = gs;
    const std::string p = temp_path();
    CHECK(s.save(p));
    Settings loaded;
    CHECK(loaded.load(p));
    CHECK(loaded.games.count("minecraft.exe") == 1);
    CHECK_EQ(loaded.games["minecraft.exe"].replay_duration_sec, 30);
    CHECK_EQ(loaded.games["minecraft.exe"].bitrate_mbps, 20);
    CHECK_EQ(loaded.games["minecraft.exe"].mic_enabled, false);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(config_bool_parsing_variants) {
    const std::string p = temp_path();
    {
        std::ofstream out(p);
        out << "[general]\nclose_to_tray=off\nnotifications=true\n";
    }
    Settings s;
    CHECK(s.load(p));
    CHECK_EQ(s.general.close_to_tray, false);
    CHECK_EQ(s.general.notifications, true);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}
