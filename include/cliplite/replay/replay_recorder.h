#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cliplite/audio/mix_limiter.h"

namespace cliplite::capture {
class DxgiDesktopDuplication;
}
namespace cliplite::audio {
class WasapiLoopback;
class WasapiMic;
class SessionMonitor;
class ProcessLoopbackCapture;
class StemSegmentWriter;
struct AudibleApp;
}
namespace cliplite::encoder {
class MediaFoundationEncoder;
}
namespace cliplite::replay {
class RingBuffer;
struct Segment;
}

namespace cliplite::replay {

// Reserved pseudo-pid for the microphone stem layer. Real processes never
// reach this value, so the mic appears in sidecars/editor rows like any app
// and mute/solo/volume apply to it through the same pid-keyed mix path.
constexpr uint32_t kMicStemPid = 0xFFFFFFFEu;

struct RecorderConfig {
    uint32_t fps = 30;
    uint32_t video_bitrate_bps = 15'000'000;
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    std::wstring buffer_dir;
    std::wstring clip_dir;
    uint32_t segment_ms = 2000;   // rotate the encoder every 2 s
    uint32_t replay_ms = 60000;   // retain the last 60 s
    uint32_t display_index = 0;   // which monitor to duplicate
    std::string source_mode = "display";  // display | window (WGC backend next)
    std::string source_window_exe;        // window-mode target (resolved at start)
    std::string source_window_title;
    bool desktop_audio = true;    // WASAPI loopback of the output device
    bool mic_enabled = false;     // mix a microphone in as well
    std::wstring mic_device;      // endpoint id; empty = system default
    bool track_app_audio = true;  // poll audible apps for the per-clip sidecar
    bool stem_capture = true;     // record per-app WAV stems (picker input)
    uint32_t max_stem_apps = 4;   // CPU/IO bound on parallel app captures
};

// Ties capture (DXGI desktop) + audio (WASAPI loopback) + encoding (MF H.264/AAC)
// into a rolling set of disk-backed MP4 segments tracked by a RingBuffer. Pressing
// save_clip() remuxes the previous replay_ms into a single MP4 clip.
class ReplayRecorder {
public:
    ReplayRecorder();
    ~ReplayRecorder();
    ReplayRecorder(const ReplayRecorder&) = delete;
    ReplayRecorder& operator=(const ReplayRecorder&) = delete;

    bool start(const RecorderConfig& cfg);
    void stop();
    bool running() const { return running_; }

    // Captures one video frame + drains available audio into the encoder,
    // rotating segments as needed. Returns false on a fatal error.
    bool tick();

    // Saves the previous replay_ms as a clip. Returns the output path, or an
    // empty string on failure. Recording continues into a fresh segment.
    std::wstring save_clip();

    uint32_t width() const { return cap_width_; }
    uint32_t height() const { return cap_height_; }
    int64_t buffered_ms() const { return buffered_ms_; }

    // Sets the name used as the clip filename prefix (e.g. "Minecraft"). Empty
    // falls back to "ClipLite". Thread-safe; called from the UI thread.
    void set_game_name(const std::string& name);

private:
    bool start_segment();
    // Finishes the current segment. When keep_in_ring is false the segment is
    // finalized/discarded without entering the replay buffer (used for segments
    // recorded during a capture outage).
    bool finalize_segment(bool keep_in_ring = true);
    std::wstring make_clip_path() const;
    // Snapshot of the window's stem WAVs copied beside the clip (survives ring
    // eviction; the mixdown render reads these, never the buffer dir).
    struct StemSnapshot {
        std::wstring dir_name;  // basename of "<clip>.stems", empty when skipped
        std::map<uint32_t, bool> has_data;  // pid -> any copied wav > header-only
        int64_t head_trim_ms = 0;  // same window trims as the video remux
        int64_t tail_trim_ms = 0;
    };
    StemSnapshot snapshot_stems(const std::vector<Segment>& segs,
                                const std::wstring& clip_path);
    // Best-effort "<clip>.apps.json" sidecar for the post-clip app picker.
    void write_app_sidecar(const std::wstring& clip_path, const StemSnapshot& snap);

    RecorderConfig cfg_;
    bool running_ = false;

    std::unique_ptr<capture::DxgiDesktopDuplication> capture_;
    std::unique_ptr<audio::WasapiLoopback> audio_;
    std::unique_ptr<audio::WasapiMic> mic_;
    std::unique_ptr<encoder::MediaFoundationEncoder> encoder_;
    std::unique_ptr<RingBuffer> ring_;

    // Audible-app tracking for the per-clip sidecar (post-clip picker lists
    // these; all selected by default). Polled from tick(), same thread.
    std::unique_ptr<audio::SessionMonitor> app_monitor_;
    struct HeardApp {
        std::string exe;
        int64_t first_ms = 0;  // session-relative, from now_ms_
        int64_t last_ms = 0;
        uint32_t polls = 0;
    };
    std::map<uint32_t, HeardApp> heard_apps_;  // keyed by pid, deterministic order
    uint64_t last_app_poll_ms_ = 0;

    // Live per-app stem capture (parallel to the loopback mix; the encoder
    // input is unchanged — stems are future picker/mixdown input).
    struct StemCap {
        std::unique_ptr<audio::ProcessLoopbackCapture> cap;
        std::unique_ptr<audio::StemSegmentWriter> writer;
        std::string exe;
        int absent_polls = 0;
    };
    std::map<uint32_t, StemCap> stem_caps_;
    std::set<uint32_t> stem_failed_;  // pids that refused capture this session
    std::vector<float> stem_buf_;     // 48kHz stereo drain scratch
    // Microphone layer: recorded like an app stem under kMicStemPid so editor
    // remixes (mute/solo/volume/speed) keep the voice instead of dropping it.
    std::unique_ptr<audio::StemSegmentWriter> mic_writer_;
    std::vector<float> mic_stem_scratch_;
    audio::MixLimiter mix_limiter_;   // transparent peak limiting, no flat-tops
    int64_t current_segment_id_ = -1;
    void stems_on_poll(const std::vector<audio::AudibleApp>& audible);
    void stems_drain();
    void stems_close_writers();
    void stems_open_writers();
    bool stems_open_writer_for(StemCap& sc, uint32_t pid);
    std::wstring stem_path_for(uint32_t pid) const;
    // Deletes segment_<id>.app_*.wav siblings of a segment file.
    void delete_stem_siblings(const std::wstring& segment_path);

    std::vector<uint8_t> nv12_;
    std::vector<float> audio_buf_;
    std::vector<float> mic_buf_;
    uint32_t cap_width_ = 0;
    uint32_t cap_height_ = 0;

    uint64_t start_ms_ = 0;
    uint64_t segment_start_ms_ = 0;
    uint64_t now_ms_ = 0;
    int64_t buffered_ms_ = 0;
    int64_t video_frames_ = 0;
    int64_t audio_frames_ = 0;
    bool segment_has_samples_ = false;
    int64_t next_segment_id_ = 0;
    std::wstring current_segment_path_;

    mutable std::mutex name_mutex_;
    std::string game_name_;

    bool capture_lost_ = false;
    uint64_t last_recover_ms_ = 0;
    bool mf_pinned_ = false;  // process-wide MF reference held while recording

    // Parallel track of segment files for deletion when evicted from the ring.
    std::deque<std::pair<std::wstring, int64_t>> files_;
};

}  // namespace cliplite::replay
