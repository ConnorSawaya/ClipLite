#pragma once

#include <cstdint>
#include <string>

#include "cliplite/detection/game_detector.h"

namespace cliplite::detection {

struct CaptureSelection {
    std::string game_name;  // active game display name (empty if none)
    std::string exe_name;   // active game executable (lowercase)
    bool active = false;
};

// Game Focus mode: keeps the current game selected through brief alt-tabs so
// clips stay named/captured as the game rather than flipping to Discord/Chrome.
// Switches immediately when another game appears; drops the game only after it
// stays inactive longer than focus_timeout_ms.
class CaptureSelector {
public:
    explicit CaptureSelector(int focus_timeout_ms = 30000);

    CaptureSelection update(const DetectedApp& app, uint64_t now_ms);
    CaptureSelection selection() const { return sel_; }

private:
    int focus_timeout_ms_;
    CaptureSelection sel_;
    uint64_t last_game_ms_ = 0;
};

}  // namespace cliplite::detection
