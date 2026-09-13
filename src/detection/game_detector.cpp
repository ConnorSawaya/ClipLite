#include "cliplite/detection/game_detector.h"

#include "cliplite/detection/game_namer.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::detection {

DetectedApp GameDetector::detect_foreground() const {
    DetectedApp app;
    app.win = query_foreground();
    if (!app.win.valid) return app;

    app.cls = classify_process(app.win.exe_name, cliplite::util::wide_to_utf8(app.win.exe_path),
                               app.win.title, app.win.fullscreen);
    app.display_name =
        name_application(app.win.exe_path, app.win.title, app.win.exe_name);
    app.is_game = (app.cls == ProcessClass::Game);
    return app;
}

std::string GameDetector::display_name_for(const WindowInfo& win) {
    return name_application(win.exe_path, win.title, win.exe_name);
}

}  // namespace cliplite::detection
