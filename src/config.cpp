#include "cliplite/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace cliplite {
namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Minimal section/key=value parser. No external dependency.
class IniFile {
public:
    bool load(const std::string& path) {
        sections_.clear();
        std::ifstream in(path);
        if (!in.is_open()) return false;
        std::string line;
        std::string current;
        while (std::getline(in, line)) {
            const std::string t = trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t.front() == '[' && t.back() == ']') {
                current = to_lower(trim(t.substr(1, t.size() - 2)));
                continue;
            }
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = to_lower(trim(t.substr(0, eq)));
            const std::string value = trim(t.substr(eq + 1));
            sections_[current][key] = value;
        }
        return true;
    }

    bool save(const std::string& path) const {
        std::ofstream out(path);
        if (!out.is_open()) return false;
        for (const auto& [section, kv] : sections_) {
            out << "[" << section << "]\n";
            for (const auto& [k, v] : kv) out << k << "=" << v << "\n";
            out << "\n";
        }
        return true;
    }

    bool has(const std::string& section, const std::string& key) const {
        const auto s = sections_.find(to_lower(section));
        if (s == sections_.end()) return false;
        return s->second.count(to_lower(key)) != 0;
    }

    std::string get(const std::string& section, const std::string& key,
                    const std::string& def = {}) const {
        const auto s = sections_.find(to_lower(section));
        if (s == sections_.end()) return def;
        const auto k = s->second.find(to_lower(key));
        return k == s->second.end() ? def : k->second;
    }

    int get_int(const std::string& section, const std::string& key, int def) const {
        const auto v = get(section, key);
        if (v.empty()) return def;
        try {
            return std::stoi(v);
        } catch (...) {
            return def;
        }
    }

    bool get_bool(const std::string& section, const std::string& key, bool def) const {
        const auto v = to_lower(get(section, key));
        if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
        if (v == "0" || v == "false" || v == "no" || v == "off") return false;
        return def;
    }

    void set(const std::string& section, const std::string& key, const std::string& value) {
        sections_[to_lower(section)][to_lower(key)] = value;
    }

    const std::map<std::string, std::map<std::string, std::string>>& sections() const {
        return sections_;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> sections_;
};

void load_game_section(const IniFile& ini, const std::string& section, GameSettings& gs) {
    gs.replay_duration_sec = ini.get_int(section, "replay_duration_sec", gs.replay_duration_sec);
    gs.resolution = ini.get_int(section, "resolution", gs.resolution);
    gs.fps = ini.get_int(section, "fps", gs.fps);
    gs.bitrate_mbps = ini.get_int(section, "bitrate_mbps", gs.bitrate_mbps);
    gs.codec = ini.get(section, "codec", gs.codec);
    gs.mic_enabled = ini.get_bool(section, "mic_enabled", gs.mic_enabled);
    gs.auto_capture = ini.get_bool(section, "auto_capture", gs.auto_capture);
}

void save_game_section(IniFile& ini, const std::string& section, const GameSettings& gs) {
    ini.set(section, "replay_duration_sec", std::to_string(gs.replay_duration_sec));
    ini.set(section, "resolution", std::to_string(gs.resolution));
    ini.set(section, "fps", std::to_string(gs.fps));
    ini.set(section, "bitrate_mbps", std::to_string(gs.bitrate_mbps));
    ini.set(section, "codec", gs.codec);
    ini.set(section, "mic_enabled", gs.mic_enabled ? "1" : "0");
    ini.set(section, "auto_capture", gs.auto_capture ? "1" : "0");
}

}  // namespace

Settings Settings::defaults() {
    return Settings{};
}

bool Settings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    recording.replay_duration_sec =
        ini.get_int("recording", "replay_duration_sec", recording.replay_duration_sec);
    recording.resolution = ini.get_int("recording", "resolution", recording.resolution);
    recording.fps = ini.get_int("recording", "fps", recording.fps);
    recording.bitrate_mbps = ini.get_int("recording", "bitrate_mbps", recording.bitrate_mbps);
    recording.codec = ini.get("recording", "codec", recording.codec);
    recording.encoder = ini.get("recording", "encoder", recording.encoder);
    recording.capture_mode = ini.get("recording", "capture_mode", recording.capture_mode);
    recording.display_index = ini.get_int("recording", "display_index", recording.display_index);
    recording.source_mode = ini.get("recording", "source_mode", recording.source_mode);
    if (recording.source_mode != "window") recording.source_mode = "display";
    recording.source_window_exe =
        ini.get("recording", "source_window_exe", recording.source_window_exe);
    recording.source_window_title =
        ini.get("recording", "source_window_title", recording.source_window_title);

    audio.desktop_enabled = ini.get_bool("audio", "desktop_enabled", audio.desktop_enabled);
    audio.mic_enabled = ini.get_bool("audio", "mic_enabled", audio.mic_enabled);
    audio.mic_device = ini.get("audio", "mic_device", audio.mic_device);
    audio.mic_volume = ini.get_int("audio", "mic_volume", audio.mic_volume);
    audio.desktop_volume = ini.get_int("audio", "desktop_volume", audio.desktop_volume);

    hotkeys.save_clip = ini.get("hotkeys", "save_clip", hotkeys.save_clip);
    hotkeys.open_library = ini.get("hotkeys", "open_library", hotkeys.open_library);

    storage.clip_folder = ini.get("storage", "clip_folder", storage.clip_folder);
    storage.buffer_folder = ini.get("storage", "buffer_folder", storage.buffer_folder);
    storage.max_clip_storage_mb =
        ini.get_int("storage", "max_clip_storage_mb", storage.max_clip_storage_mb);

    general.start_with_windows =
        ini.get_bool("general", "start_with_windows", general.start_with_windows);
    general.minimize_to_tray = ini.get_bool("general", "minimize_to_tray", general.minimize_to_tray);
    general.close_to_tray = ini.get_bool("general", "close_to_tray", general.close_to_tray);
    general.notifications = ini.get_bool("general", "notifications", general.notifications);
    general.auto_update = ini.get_bool("general", "auto_update", general.auto_update);
    general.auto_capture = ini.get_bool("general", "auto_capture", general.auto_capture);
    general.capture_desktop_idle =
        ini.get_bool("general", "capture_desktop_idle", general.capture_desktop_idle);
    general.game_focus_timeout_sec =
        ini.get_int("general", "game_focus_timeout_sec", general.game_focus_timeout_sec);
    general.quality_preset = ini.get("general", "quality_preset", general.quality_preset);

    games.clear();
    for (const auto& [section, kv] : ini.sections()) {
        (void)kv;
        if (section.rfind("game.", 0) == 0) {
            const std::string exe = section.substr(5);
            GameSettings gs;
            load_game_section(ini, section, gs);
            games[exe] = gs;
        }
    }
    return true;
}

bool Settings::save(const std::string& path) const {
    IniFile ini;
    ini.set("recording", "replay_duration_sec", std::to_string(recording.replay_duration_sec));
    ini.set("recording", "resolution", std::to_string(recording.resolution));
    ini.set("recording", "fps", std::to_string(recording.fps));
    ini.set("recording", "bitrate_mbps", std::to_string(recording.bitrate_mbps));
    ini.set("recording", "codec", recording.codec);
    ini.set("recording", "encoder", recording.encoder);
    ini.set("recording", "capture_mode", recording.capture_mode);
    ini.set("recording", "display_index", std::to_string(recording.display_index));
    ini.set("recording", "source_mode", recording.source_mode);
    ini.set("recording", "source_window_exe", recording.source_window_exe);
    ini.set("recording", "source_window_title", recording.source_window_title);

    ini.set("audio", "desktop_enabled", audio.desktop_enabled ? "1" : "0");
    ini.set("audio", "mic_enabled", audio.mic_enabled ? "1" : "0");
    ini.set("audio", "mic_device", audio.mic_device);
    ini.set("audio", "mic_volume", std::to_string(audio.mic_volume));
    ini.set("audio", "desktop_volume", std::to_string(audio.desktop_volume));

    ini.set("hotkeys", "save_clip", hotkeys.save_clip);
    ini.set("hotkeys", "open_library", hotkeys.open_library);

    ini.set("storage", "clip_folder", storage.clip_folder);
    ini.set("storage", "buffer_folder", storage.buffer_folder);
    ini.set("storage", "max_clip_storage_mb", std::to_string(storage.max_clip_storage_mb));

    ini.set("general", "start_with_windows", general.start_with_windows ? "1" : "0");
    ini.set("general", "minimize_to_tray", general.minimize_to_tray ? "1" : "0");
    ini.set("general", "close_to_tray", general.close_to_tray ? "1" : "0");
    ini.set("general", "notifications", general.notifications ? "1" : "0");
    ini.set("general", "auto_update", general.auto_update ? "1" : "0");
    ini.set("general", "auto_capture", general.auto_capture ? "1" : "0");
    ini.set("general", "capture_desktop_idle", general.capture_desktop_idle ? "1" : "0");
    ini.set("general", "game_focus_timeout_sec", std::to_string(general.game_focus_timeout_sec));
    ini.set("general", "quality_preset", general.quality_preset);

    for (const auto& [exe, gs] : games) save_game_section(ini, "game." + exe, gs);
    return ini.save(path);
}

}  // namespace cliplite
