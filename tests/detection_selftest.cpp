// Detection self-test: classifier (pure logic), game naming from real version
// resources, and a live foreground-window query.

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>

#include "cliplite/detection/capture_selector.h"
#include "cliplite/detection/game_detector.h"
#include "cliplite/detection/game_namer.h"
#include "cliplite/detection/window_info.h"
#include "cliplite/detection/process_classifier.h"
#include "cliplite/log.h"
#include "cliplite/util/win_utf8.h"

using cliplite::detection::ProcessClass;

namespace {
int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_eq(ProcessClass got, ProcessClass want, const char* what) {
    check(got == want, what);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower((unsigned char)c));
    return s;
}
}  // namespace

int main() {
    cliplite::log::init("cliplite_detection_test.log");

    std::printf("classifier\n");
    check_eq(cliplite::detection::classify_process("explorer.exe", "", L"", false),
             ProcessClass::System, "explorer.exe -> System");
    check_eq(cliplite::detection::classify_process("taskmgr.exe", "", L"", false),
             ProcessClass::System, "taskmgr.exe -> System");
    check_eq(cliplite::detection::classify_process("steam.exe", "", L"", false),
             ProcessClass::Launcher, "steam.exe -> Launcher");
    check_eq(cliplite::detection::classify_process("epicgameslauncher.exe", "", L"", false),
             ProcessClass::Launcher, "epicgameslauncher.exe -> Launcher");
    check_eq(cliplite::detection::classify_process("gamebar.exe", "", L"", false),
             ProcessClass::Overlay, "gamebar.exe -> Overlay");
    check_eq(cliplite::detection::classify_process("minecraft.exe", "", L"", false),
             ProcessClass::Game, "minecraft.exe -> Game");
    check_eq(cliplite::detection::classify_process("valorant-win64-shipping.exe", "", L"", false),
             ProcessClass::Game, "valorant-win64-shipping.exe -> Game");
    check_eq(cliplite::detection::classify_process("chrome.exe", "", L"Google Chrome", false),
             ProcessClass::Unknown, "chrome.exe -> Unknown");
    check_eq(cliplite::detection::classify_process("javaw.exe", "", L"Minecraft 1.21", false),
             ProcessClass::Game, "javaw.exe + 'Minecraft' title -> Game");
    check_eq(cliplite::detection::classify_process(
                 "foo.exe", "c:\\steam\\steamapps\\common\\foo\\foo.exe", L"Foo", false),
             ProcessClass::Game, "steamapps\\common path -> Game");
    check_eq(cliplite::detection::classify_process("foo.exe", "", L"Some App", true),
             ProcessClass::Game, "fullscreen unknown -> Game");

    std::printf("game naming\n");
    {
        const std::string name = cliplite::detection::name_application(
            L"C:\\Windows\\System32\\notepad.exe", L"Untitled - Notepad", "notepad.exe");
        std::printf("  notepad name = '%s'\n", name.c_str());
        check(lower(name).find("notepad") != std::string::npos, "notepad.exe -> 'Notepad'");
    }
    {
        const std::string name = cliplite::detection::name_application(
            L"C:\\nope\\javaw.exe", L"Minecraft 1.21.4 - Launcher", "javaw.exe");
        std::printf("  javaw name = '%s'\n", name.c_str());
        check(name.find("Minecraft") != std::string::npos, "javaw + title -> Minecraft");
    }

    std::printf("capture selector (game focus)\n");
    {
        using cliplite::detection::CaptureSelector;
        auto make_app = [](bool game, const std::string& name) {
            cliplite::detection::DetectedApp a;
            a.is_game = game;
            a.display_name = name;
            a.win.exe_name = name;
            a.cls = game ? ProcessClass::Game : ProcessClass::Unknown;
            return a;
        };

        CaptureSelector sel(30000);
        auto s = sel.update(make_app(true, "Minecraft"), 0);
        check(s.active && s.game_name == "Minecraft", "game detected -> active");

        s = sel.update(make_app(false, "Discord"), 10000);  // 10s alt-tab
        check(s.active && s.game_name == "Minecraft", "alt-tab within timeout keeps game");

        s = sel.update(make_app(false, "Discord"), 40000);  // >30s away
        check(!s.active, "game dropped after timeout");

        s = sel.update(make_app(true, "Minecraft"), 41000);
        check(s.active && s.game_name == "Minecraft", "game returns -> active again");

        s = sel.update(make_app(true, "Valorant"), 42000);
        check(s.active && s.game_name == "Valorant", "different game switches immediately");
    }

    std::printf("live foreground\n");
    {
        cliplite::detection::GameDetector det;
        const auto app = det.detect_foreground();
        std::printf("  fg: exe='%s' title='%s' class=%d game=%d fullscreen=%d %dx%d\n",
                    app.win.exe_name.c_str(),
                    cliplite::util::wide_to_utf8(app.win.title).c_str(),
                    static_cast<int>(app.cls), app.is_game ? 1 : 0, app.win.fullscreen ? 1 : 0,
                    app.win.width, app.win.height);
        check(app.win.valid, "foreground window queried (valid)");
        // A hung foreground window (e.g. "Not Responding" game) refuses
        // process queries: skip instead of failing on user state.
        if (app.win.hwnd && IsHungAppWindow(app.win.hwnd)) {
            std::printf("  foreground hung, skipping exe check\n");
        } else {
            check(!app.win.exe_name.empty(), "foreground exe name non-empty");
        }
        check(app.win.pid != 0, "foreground pid non-zero");
    }

    std::printf("visible windows (source picker)\n");
    {
        const auto wins = cliplite::detection::enum_visible_windows();
        std::printf("  %zu window(s)\n", wins.size());
        const DWORD self = GetCurrentProcessId();
        for (const auto& w : wins) {
            check(w.valid, "entry valid");
            check(w.visible && !w.minimized, "entry visible + restored");
            check(!w.title.empty() && !w.exe_name.empty(), "entry titled + named");
            check(w.pid != 0 && w.pid != self, "entry foreign pid");
        }
    }

    std::printf(g_failures == 0 ? "DETECTION SELFTEST PASS\n" : "DETECTION SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
