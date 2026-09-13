#include "cliplite/detection/process_classifier.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "cliplite/util/win_utf8.h"

namespace cliplite::detection {

namespace {

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

const std::vector<std::string>& system_exes() {
    static const std::vector<std::string> v = {
        "explorer.exe", "taskmgr.exe", "dwm.exe", "startmenuexperiencehost.exe",
        "searchhost.exe", "searchapp.exe", "shellexperiencehost.exe", "systemsettings.exe",
        "applicationframehost.exe", "textinputhost.exe", "sihost.exe", "ctfmon.exe",
        "lockapp.exe", "logonui.exe", "winlogon.exe",
    };
    return v;
}

const std::vector<std::string>& overlay_exes() {
    static const std::vector<std::string> v = {
        "gamebar.exe", "gamebarftserver.exe", "gamebarpresencewriter.exe",
        "xboxgamebarwidgets.exe", "nvcontainer.exe", "nvsphelper64.exe", "overwolf.exe",
    };
    return v;
}

const std::vector<std::string>& launcher_exes() {
    static const std::vector<std::string> v = {
        "steam.exe", "steamwebhelper.exe", "epicgameslauncher.exe", "eadesktop.exe",
        "ealauncher.exe", "battlenet.exe", "battle.net.exe", "galaxyclient.exe",
        "upc.exe", "ubisoftconnect.exe", "riotclientservices.exe", "itch.exe",
        "heroic.exe", "playnite.exe", "goggalaxy.exe", "eaapp.exe",
    };
    return v;
}

const std::vector<std::string>& known_games() {
    static const std::vector<std::string> v = {
        "minecraft.exe", "csgo.exe", "cs2.exe", "valorant.exe",
        "valorant-win64-shipping.exe", "fortniteclient-win64-shipping.exe", "r5apex.exe",
        "overwatch.exe", "rocketleague.exe", "dota2.exe", "ffxiv.exe", "ffxiv_dx11.exe",
        "eldenring.exe", "cyberpunk2077.exe", "reddeadredemption2.exe", "gtav.exe",
        "rainbowsix.exe", "rainbowsix_be.exe", "cod.exe", "modernwarfare.exe",
    };
    return v;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool title_has(const std::wstring& title, const char* needle) {
    return to_lower(cliplite::util::wide_to_utf8(title)).find(needle) != std::string::npos;
}

}  // namespace

ProcessClass classify_process(const std::string& exe_name_lower,
                              const std::string& exe_path_lower, const std::wstring& title,
                              bool fullscreen) {
    if (contains(system_exes(), exe_name_lower)) return ProcessClass::System;
    if (contains(overlay_exes(), exe_name_lower)) return ProcessClass::Overlay;
    if (contains(launcher_exes(), exe_name_lower)) return ProcessClass::Launcher;
    if (contains(known_games(), exe_name_lower)) return ProcessClass::Game;

    // Install-dir heuristics (games launched from a known platform library).
    if (exe_path_lower.find("steamapps\\common") != std::string::npos ||
        exe_path_lower.find("steamapps\\workshop") != std::string::npos ||
        exe_path_lower.find("\\epic games\\") != std::string::npos ||
        exe_path_lower.find("\\xboxgames\\") != std::string::npos ||
        exe_path_lower.find("\\gog games\\") != std::string::npos ||
        exe_path_lower.find("\\origin games\\") != std::string::npos) {
        return ProcessClass::Game;
    }

    // Title heuristics (e.g. Minecraft launched through javaw.exe).
    if (title_has(title, "minecraft")) return ProcessClass::Game;

    // Fullscreen/borderless non-system windows are usually games.
    if (fullscreen && !title.empty()) return ProcessClass::Game;

    return ProcessClass::Unknown;
}

}  // namespace cliplite::detection
