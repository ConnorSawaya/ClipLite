#pragma once

#include <string>

#include "cliplite/detection/process_classifier.h"
#include "cliplite/detection/window_info.h"

namespace cliplite::detection {

struct DetectedApp {
    WindowInfo win;
    ProcessClass cls = ProcessClass::Unknown;
    std::string display_name;
    bool is_game = false;
};

// Observes the foreground window and produces a classified, human-named result.
class GameDetector {
public:
    DetectedApp detect_foreground() const;

    // Human-readable name for an arbitrary window (used for tray status).
    static std::string display_name_for(const WindowInfo& win);
};

}  // namespace cliplite::detection
