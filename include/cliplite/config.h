#pragma once

#include <map>
#include <string>

namespace cliplite {

struct RecordingSettings {
    int replay_duration_sec = 60;
    int resolution = 1080;               // 0 = source; else vertical pixels (720/1080/1440/2160)
    int fps = 60;
    int bitrate_mbps = 15;
    std::string codec = "h264";          // h264 | hevc | av1
    std::string encoder = "auto";        // auto | nvenc | amf | qsv | mf | software
    std::string capture_mode = "game_focus";  // game_focus | follow_foreground
    int display_index = 0;               // which monitor to capture (full-screen mode)
    std::string source_mode = "display";  // display | window (source picker)
    std::string source_window_exe;        // exe to capture in window mode
    std::string source_window_title;      // title hint to disambiguate same-exe windows
};

struct AudioSettings {
    bool desktop_enabled = true;
    bool mic_enabled = false;
    std::string mic_device;
    int mic_volume = 80;                 // 0..100
    int desktop_volume = 100;            // 0..100
};

struct HotkeySettings {
    std::string save_clip = "F8";
    std::string open_library = "Ctrl+Shift+L";
};

struct StorageSettings {
    std::string clip_folder;             // empty = default %USERPROFILE%\Videos\ClipLite
    std::string buffer_folder;           // empty = default %LOCALAPPDATA%\ClipLite\Buffer
    int max_clip_storage_mb = 0;         // 0 = unlimited
};

struct GeneralSettings {
    bool start_with_windows = false;
    bool minimize_to_tray = true;
    bool close_to_tray = true;
    bool notifications = true;
    bool auto_update = false;
    bool auto_capture = true;
    bool capture_desktop_idle = false;   // keep recording when no game runs (RAM cost)
    int game_focus_timeout_sec = 30;
    std::string quality_preset = "balanced";  // low | balanced | high | custom
};

struct GameSettings {
    int replay_duration_sec = 60;
    int resolution = 1080;
    int fps = 60;
    int bitrate_mbps = 15;
    std::string codec = "h264";
    bool mic_enabled = false;
    bool auto_capture = true;
};

struct Settings {
    RecordingSettings recording;
    AudioSettings audio;
    HotkeySettings hotkeys;
    StorageSettings storage;
    GeneralSettings general;
    std::map<std::string, GameSettings> games;  // key = lowercase executable name

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    static Settings defaults();
};

}  // namespace cliplite
