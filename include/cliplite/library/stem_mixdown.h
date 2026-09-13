#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace cliplite::library {

// Parsed "<clip>.apps.json" (our fixed schema, v1 + v2).
struct StemClipInfo {
    std::wstring stem_dir;                  // absolute "<clip>.stems" dir (may not exist)
    std::map<uint32_t, std::string> apps;   // pid -> exe for ALL heard apps
    std::map<uint32_t, bool> has_stem;      // pid -> stem:true (separate track exists)
    int64_t head_trim_ms = 0;               // window trims, shared with video remux
    int64_t tail_trim_ms = 0;
};

// Parses the sidecar next to clip_path. Returns false when missing or
// unparsable (caller falls back to a plain A/V passthrough copy).
bool read_stem_sidecar(const std::wstring& clip_path, StemClipInfo* info);

// Renders output = input video (compressed passthrough) + AAC mix of the
// snapshot stems minus excluded_pids. Empty exclusion = full mix (all apps,
// the post-picker default). Behavior by stem availability:
//   usable stems + some included -> mixed AAC audio track;
//   usable stems + all excluded (or mix trims to empty) -> video-only;
//   no sidecar/stems at all -> plain A/V passthrough copy (old clips work).
// Returns false on failure (no partial output is left behind).
bool export_without_apps(const std::wstring& input, const std::wstring& output,
                         const std::set<uint32_t>& excluded_pids);

}  // namespace cliplite::library
