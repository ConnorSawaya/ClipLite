#include "cliplite/detection/capture_selector.h"

namespace cliplite::detection {

CaptureSelector::CaptureSelector(int focus_timeout_ms) : focus_timeout_ms_(focus_timeout_ms) {}

CaptureSelection CaptureSelector::update(const DetectedApp& app, uint64_t now_ms) {
    if (app.is_game) {
        sel_.game_name = app.display_name;
        sel_.exe_name = app.win.exe_name;
        sel_.active = true;
        last_game_ms_ = now_ms;
        return sel_;
    }

    if (sel_.active &&
        (now_ms - last_game_ms_) <= static_cast<uint64_t>(focus_timeout_ms_)) {
        return sel_;  // keep the active game through a brief alt-tab
    }

    sel_.active = false;
    sel_.game_name.clear();
    sel_.exe_name.clear();
    return sel_;
}

}  // namespace cliplite::detection
