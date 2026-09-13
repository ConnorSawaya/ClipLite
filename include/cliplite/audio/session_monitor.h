#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::audio {

// One audible render-session group, keyed by process.
struct AudibleApp {
    uint32_t pid = 0;  // 0 = system sounds session
    std::string exe;   // basename, e.g. "spotify.exe"; "System Sounds" for pid 0
    float peak = 0.f;  // meter 0..1 observed at poll time
};

// Polls the default render endpoint's audio sessions and returns the apps with
// audible activity (session Active, or meter peak above threshold). Used to
// discover which apps produced sound during a recording so the post-clip
// picker can offer per-app removal. COM must be initialized on the calling
// thread. Never throws; returns an empty vector on silence or when no render
// device exists. One entry per pid (max peak across that pid's sessions).
class SessionMonitor {
public:
    SessionMonitor() = default;

    std::vector<AudibleApp> poll_audible(float peak_threshold = 0.001f);
};

}  // namespace cliplite::audio
