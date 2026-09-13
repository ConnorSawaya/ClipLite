#pragma once

#include <string>

namespace cliplite::detection {

enum class ProcessClass {
    System,    // Explorer, Task Manager, DWM, Start menu, Settings, Search, ...
    Overlay,   // Game Bar / vendor overlay windows
    Launcher,  // Steam / Epic / Battle.net / GOG / ...
    Game,      // looks like a game
    Unknown,   // regular foreground application
};

// Classifies a foreground process by executable name, path, title, and whether
// its window is fullscreen. Pure logic (no process handles opened here).
ProcessClass classify_process(const std::string& exe_name_lower,
                              const std::string& exe_path_lower, const std::wstring& title,
                              bool fullscreen);

}  // namespace cliplite::detection
