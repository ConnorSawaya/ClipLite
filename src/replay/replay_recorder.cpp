#include "cliplite/replay/replay_recorder.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "cliplite/audio/wasapi_loopback.h"
#include "cliplite/audio/wasapi_mic.h"
#include "cliplite/audio/session_monitor.h"
#include "cliplite/audio/process_loopback.h"
#include "cliplite/audio/stem_track.h"
#include "cliplite/audio/stem_writer.h"
#include "cliplite/capture/dxgi_capture.h"
#include "cliplite/encoder/media_foundation.h"
#include "cliplite/graphics/frame_converter.h"
#include "cliplite/log.h"
#include "cliplite/replay/clip_remux.h"
#include "cliplite/replay/recovery.h"
#include "cliplite/replay/ring_buffer.h"
#include "cliplite/util/string.h"

namespace cliplite::replay {

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

}  // namespace

ReplayRecorder::ReplayRecorder() = default;

ReplayRecorder::~ReplayRecorder() {
    stop();
}

bool ReplayRecorder::start(const RecorderConfig& cfg) {
    if (running_) return false;
    cfg_ = cfg;

    // Pin Media Foundation for the whole pipeline lifetime. Segment rotation
    // creates/destroys encoders every few seconds; without the pin each cycle
    // would run MFStartup/MFShutdown, which leaks kernel handles.
    mf_pinned_ = encoder::mf_hold_reference();

    cleanup_buffer(cfg_.buffer_dir);

    capture_ = std::make_unique<capture::DxgiDesktopDuplication>();
    if (!capture_->start(cfg_.display_index)) {
        CL_ERROR("Recorder", "desktop capture failed to start");
        return false;
    }
    cap_width_ = capture_->width();
    cap_height_ = capture_->height();

    // Guard against div-by-zero in timestamp math (fps/sample_rate divisors).
    if (cfg_.fps == 0) {
        CL_WARN("Recorder", "fps=0 in config, defaulting to 30");
        cfg_.fps = 30;
    }
    if (cfg_.sample_rate == 0) {
        CL_WARN("Recorder", "sample_rate=0 in config, defaulting to 48000");
        cfg_.sample_rate = 48000;
    }
    if (cfg_.channels == 0) {
        CL_WARN("Recorder", "channels=0 in config, defaulting to 2");
        cfg_.channels = 2;
    }

    audio_.reset();
    mic_.reset();
    if (cfg_.desktop_audio) {
        audio_ = std::make_unique<audio::WasapiLoopback>();
        audio::AudioFormat afmt;
        if (!audio_->start(&afmt)) {
            CL_WARN("Recorder", "loopback audio failed to start, continuing video-only");
            audio_.reset();
        } else {
            // Match the encoder to the device's actual negotiated format.
            cfg_.sample_rate = afmt.sample_rate;
            cfg_.channels = afmt.channels;
        }
    }
    if (cfg_.mic_enabled) {
        mic_ = std::make_unique<audio::WasapiMic>();
        if (!mic_->start(cfg_.mic_device)) {
            CL_WARN("Recorder", "mic failed to start, continuing without it");
            mic_.reset();
        } else if (!audio_) {
            // Mic-only: adopt the mic's negotiated format. Previously the
            // default 48kHz was kept, stamping 16kHz mic frames at 48kHz
            // (3x short audio, wrong clip length).
            cfg_.sample_rate = mic_->sample_rate();
            cfg_.channels = mic_->channels();
            CL_INFO("Recorder", "mic-only rate=" + std::to_string(cfg_.sample_rate) +
                                    " ch=" + std::to_string(cfg_.channels));
        } else {
            // Both active with mismatched rates: mixing without resampling
            // stretches time. Prefer correct length over a stretched mix.
            if (mic_->sample_rate() != cfg_.sample_rate ||
                mic_->channels() == 0) {
                CL_WARN("Recorder", "mic rate mismatch (mic=" +
                                        std::to_string(mic_->sample_rate()) + " vs loopback=" +
                                        std::to_string(cfg_.sample_rate) +
                                        "), ignoring mic to preserve A/V length");
                mic_.reset();
            }
        }
    }

    nv12_.assign(static_cast<size_t>(cap_width_) * cap_height_ * 3 / 2, 0);
    // Ceiling division avoids systematic short for odd sample rates.
    const uint32_t chunk = (cfg_.sample_rate + 9) / 10;
    audio_buf_.assign(static_cast<size_t>(chunk) * cfg_.channels, 0.0f);
    // Size the mic scratch for the mic's own channel count (it may exceed
    // the encoder's when devices differ); the mixing loop downmixes.
    {
        size_t mic_need = static_cast<size_t>(chunk) * cfg_.channels;
        if (mic_ && mic_->channels() > cfg_.channels)
            mic_need = static_cast<size_t>(chunk) * mic_->channels();
        mic_buf_.assign(mic_need, 0.0f);
    }

    ring_ = std::make_unique<RingBuffer>(cfg_.replay_ms);

    // App-audio sidecar foundation: discover audible apps ~2x/sec in tick().
    // Gated on having an audio source so video-only recording stays silent.
    // Stem capture implies tracking (it needs the same polls).
    heard_apps_.clear();
    last_app_poll_ms_ = 0;
    mix_limiter_ = audio::MixLimiter{};
    app_monitor_.reset();
    if ((cfg_.track_app_audio || cfg_.stem_capture) && (audio_ || mic_)) {
        app_monitor_ = std::make_unique<audio::SessionMonitor>();
    }
    stem_caps_.clear();
    stem_failed_.clear();
    stem_buf_.clear();
    if (cfg_.stem_capture && (audio_ || mic_)) {
        stem_buf_.assign(static_cast<size_t>((48000 + 9) / 10) * 2, 0.f);
    }

    start_ms_ = GetTickCount64();
    segment_start_ms_ = 0;
    now_ms_ = 0;
    video_frames_ = 0;
    audio_frames_ = 0;
    next_segment_id_ = 0;

    if (!start_segment()) {
        CL_ERROR("Recorder", "initial segment failed");
        stop();
        return false;
    }

    running_ = true;
    CL_INFO("Recorder", "recording started (" + std::to_string(cap_width_) + "x" +
                            std::to_string(cap_height_) + ")");
    return true;
}

void ReplayRecorder::stop() {
    if (running_) finalize_segment();
    running_ = false;
    encoder_.reset();
    audio_.reset();
    capture_.reset();
    ring_.reset();
    app_monitor_.reset();
    heard_apps_.clear();
    stems_close_writers();
    stem_caps_.clear();
    stem_failed_.clear();
    stem_buf_.clear();
    mic_writer_.reset();
    mic_stem_scratch_.clear();
    if (mf_pinned_) {
        encoder::mf_drop_reference();
        mf_pinned_ = false;
    }
    for (auto& [path, end] : files_) {
        (void)end;
        delete_stem_siblings(path);
        DeleteFileW(path.c_str());
    }
    files_.clear();
    // Current (unfinalized-empty or just-rotated) segment's stems, if any.
    if (!current_segment_path_.empty()) delete_stem_siblings(current_segment_path_);
    current_segment_path_.clear();
}

bool ReplayRecorder::start_segment() {
    encoder_ = std::make_unique<encoder::MediaFoundationEncoder>();

    // Per-segment timestamps start at 0 so segments can be remuxed by rebasing.
    video_frames_ = 0;
    audio_frames_ = 0;
    segment_has_samples_ = false;

    current_segment_id_ = next_segment_id_++;
    current_segment_path_ = cfg_.buffer_dir + L"\\segment_" +
                            std::to_wstring(current_segment_id_) + L".tmp.mp4";

    encoder::EncoderConfig ec;
    ec.width = cap_width_;
    ec.height = cap_height_;
    ec.fps_num = cfg_.fps;
    ec.fps_den = 1;
    ec.video_bitrate_bps = cfg_.video_bitrate_bps;
    ec.audio_enabled = (audio_ != nullptr) || (mic_ != nullptr);
    ec.sample_rate = cfg_.sample_rate;
    ec.channels = cfg_.channels;

    if (!encoder_->start(current_segment_path_, ec)) {
        CL_ERROR("Recorder", "encoder start failed");
        return false;
    }
    stems_open_writers();
    return true;
}

bool ReplayRecorder::finalize_segment(bool keep_in_ring) {
    if (!encoder_) return true;
    // Stem WAV headers must be finalized for whichever segment is closing, in
    // every path below (kept or dropped).
    stems_close_writers();
    // An empty segment (no samples) cannot be finalized; discard it.
    if (!segment_has_samples_) {
        encoder_->abort();
        encoder_.reset();
        delete_stem_siblings(current_segment_path_);
        DeleteFileW(current_segment_path_.c_str());
        return true;
    }
    const bool ok = encoder_->finish();
    encoder_.reset();
    CL_DEBUG("Recorder", "segment finalized v=" + std::to_string(video_frames_) +
                             " a=" + std::to_string(audio_frames_) + " ok=" + (ok ? "1" : "0"));
    if (!keep_in_ring) {
        // Outage segment: not part of the replay timeline, drop the file.
        delete_stem_siblings(current_segment_path_);
        DeleteFileW(current_segment_path_.c_str());
        return ok;
    }
    if (ok && now_ms_ > segment_start_ms_) {
        const int64_t start_ms = static_cast<int64_t>(segment_start_ms_);
        const int64_t end_ms = static_cast<int64_t>(now_ms_);
        ring_->add_segment(start_ms, end_ms, wide_to_utf8(current_segment_path_));
        files_.push_back({current_segment_path_, end_ms});
        buffered_ms_ = ring_->total_duration_ms();
    }
    // Delete segment files fully evicted from the ring.
    const int64_t span_start = ring_->span_start_ms();
    while (!files_.empty() && files_.front().second <= span_start) {
        delete_stem_siblings(files_.front().first);
        DeleteFileW(files_.front().first.c_str());
        files_.pop_front();
    }
    return ok;
}

bool ReplayRecorder::tick() {
    if (!running_) return false;
    now_ms_ = GetTickCount64() - start_ms_;

    // Device-loss recovery: if the desktop capture lost access (screen locked,
    // session switch, monitor change), recreate the capture every ~2s until it
    // succeeds, then resume encoding into a fresh segment.
    if (capture_->lost()) capture_lost_ = true;
    if (capture_lost_) {
        const uint64_t t = GetTickCount64();
        if (t - last_recover_ms_ >= 2000) {
            last_recover_ms_ = t;
            // Discard: this segment covers (part of) the outage, so it must
            // not enter the replay timeline.
            finalize_segment(false);
            capture_->stop();
            if (capture_->start(cfg_.display_index)) {
                capture_lost_ = false;
                segment_start_ms_ = now_ms_;
                CL_INFO("Recorder", "capture recovered");
                if (!start_segment()) return false;
            } else {
                CL_WARN("Recorder", "capture still unavailable, retrying");
            }
        }
        return true;
    }

    // FPS throttle: tick() runs every ~8ms but frames are stamped at 1/fps.
    // Without pacing, a 60Hz desktop at fps=30 pushes 2x too many frames
    // stamped at half rate (2x long clip). Drop early frames to hold length.
    const int64_t wall_elapsed_ms = static_cast<int64_t>(now_ms_) -
                                    static_cast<int64_t>(segment_start_ms_);
    const int64_t expected_media_ms =
        cfg_.fps > 0 ? (video_frames_ * 1000LL / static_cast<int64_t>(cfg_.fps)) : 0;
    const bool video_due = (expected_media_ms <= wall_elapsed_ms + 2);

    capture::CapturedFrame frame;
    const bool have_frame = capture_->acquire_frame(&frame, 50);
    if (have_frame) {
        if (video_due && cfg_.fps > 0) {
            cliplite::graphics::bgra_to_nv12(frame.bgra, frame.stride, frame.width,
                                              frame.height, nv12_.data());
            const int64_t t = video_frames_ * 10'000'000LL / cfg_.fps;
            encoder_->push_video_frame(nv12_.data(), frame.width, t);
            ++video_frames_;
            segment_has_samples_ = true;
        }
        // else: tick ran faster than fps; drop this desktop frame so media
        // time does not run ahead of wall time (prevents long clips).
        capture_->release_frame();
    }

    // Gap-fill: a static screen starves acquire_frame, stalling the video
    // timeline behind wall clock (wrong clip lengths + A/V drift). Duplicate
    // the last converted frame to hold media time to wall time. Bounded per
    // tick so long stalls converge smoothly instead of bursting.
    if (cfg_.fps > 0 && video_frames_ > 0) {
        int budget = 8;
        while (budget-- > 0 &&
               video_frames_ * 1000LL / static_cast<int64_t>(cfg_.fps) <=
                   wall_elapsed_ms + 2) {
            const int64_t t = video_frames_ * 10'000'000LL / cfg_.fps;
            if (!encoder_->push_video_frame(nv12_.data(), cap_width_, t)) break;
            ++video_frames_;
            segment_has_samples_ = true;
        }
    }

    if (audio_ || mic_) {
        const uint32_t chunk = (cfg_.sample_rate + 9) / 10;
        const size_t ch = cfg_.channels;
        uint32_t got = 0;
        if (audio_) got = audio_->read(audio_buf_.data(), chunk);
        // Zero the unwritten tail: the mic overlay below mixes onto silence
        // there. Without this the tail holds last tick's samples (garble).
        if (audio_ && got < chunk) {
            std::memset(audio_buf_.data() + static_cast<size_t>(got) * ch, 0,
                        static_cast<size_t>(chunk - got) * ch * sizeof(float));
        }
        if (!audio_ && mic_) {
            std::memset(audio_buf_.data(), 0,
                        static_cast<size_t>(chunk) * ch * sizeof(float));
        }
        uint32_t mic_mixed = 0;
        if (mic_) {
            const uint32_t mgot_raw = mic_->read(mic_buf_.data(), chunk);
            uint16_t mch = mic_->channels();
            if (mch == 0) mch = 1;
            // Overlay onto the SAME time base (not appended after loopback):
            // both devices cover the same wall interval, so frame f of mic
            // mixes with frame f of loopback. Appending doubled the timeline.
            const uint32_t frames = std::min<uint32_t>(mgot_raw, chunk);
            // Downmix/upmix mic channels into the encoder's channel count.
            for (uint32_t f = 0; f < frames; ++f) {
                float acc = 0.f;
                for (uint16_t c = 0; c < mch; ++c)
                    acc += mic_buf_[static_cast<size_t>(f) * mch + c];
                acc /= mch;
                for (size_t c = 0; c < ch; ++c) {
                    // No per-sample clamp here: the peak limiter below fits
                    // hot sums transparently (flat-topping reads as garble).
                    audio_buf_[static_cast<size_t>(f) * ch + c] += acc;
                }
            }
            mic_mixed = frames;
            // Microphone layer: track it as an app-like row and mirror it
            // into its own stem so editor remixes keep the voice. 1ch
            // duplicates, 2ch copies (48k stereo guaranteed at open).
            if (frames > 0) {
                HeardApp& rec = heard_apps_[kMicStemPid];
                if (rec.polls == 0) {
                    rec.exe = "Microphone";
                    rec.first_ms = static_cast<int64_t>(now_ms_);
                }
                rec.last_ms = static_cast<int64_t>(now_ms_);
                ++rec.polls;
            }
            if (mic_writer_ && mic_writer_->is_open() && frames > 0) {
                mic_stem_scratch_.resize(static_cast<size_t>(frames) * 2);
                for (uint32_t f = 0; f < frames; ++f) {
                    const float l = mic_buf_[static_cast<size_t>(f) * mch];
                    const float r =
                        (mch == 1) ? l : mic_buf_[static_cast<size_t>(f) * mch + 1];
                    mic_stem_scratch_[static_cast<size_t>(f) * 2] = l;
                    mic_stem_scratch_[static_cast<size_t>(f) * 2 + 1] = r;
                }
                if (!mic_writer_->append(mic_stem_scratch_.data(), frames)) {
                    CL_WARN("Recorder", "mic stem: wav append failed");
                    mic_writer_->close();
                }
            }
        }
        // Mixed length is the overlap max, not the sum: both sources cover
        // the same wall interval. Summing ran the audio clock ~2x hot.
        uint32_t total = 0;
        if (audio_ && mic_) {
            total = got > mic_mixed ? got : mic_mixed;
        } else if (audio_) {
            total = got;
        } else if (mic_) {
            total = mic_mixed;
        }
        if (total > chunk) total = chunk;
        if (total > 0 && cfg_.sample_rate > 0) {
            audio::limit_mix_block(audio_buf_.data(), total,
                                   static_cast<uint16_t>(ch), mix_limiter_);
            const int64_t t = audio_frames_ * 10'000'000LL / cfg_.sample_rate;
            encoder_->push_audio_frames(audio_buf_.data(), total, t);
            audio_frames_ += total;
            // Audio-only samples still make the segment finalizable; otherwise
            // a broken video path would cause endless encoder create/destroy
            // cycles while audio keeps flowing.
            segment_has_samples_ = true;
        }
        // Gap-fill: device underruns stall the audio timeline behind wall
        // clock (the video gap-fill above keeps video paced; audio must match
        // or A/V drift + wrong lengths return). Top up with silence, bounded
        // per tick. mic_buf_ doubles as zero scratch (rewritten by mic reads).
        //
        // The deficit gets a full tick of grace (~100ms), AND filling only
        // happens when this tick captured nothing at all: frames still sitting
        // in the device buffer (drained next tick, e.g. after a slow tick
        // capped the drain at `chunk`) look identical to lost frames, and
        // stapling silence next to soon-arriving real audio chops the track
        // up (garbled) and inflates it past wall clock. A dry tick with a
        // real deficit is true loss, safe to silence.
        if (cfg_.sample_rate > 0 && total == 0) {
            const int64_t expected_ms =
                audio_frames_ * 1000LL / static_cast<int64_t>(cfg_.sample_rate);
            const int64_t deficit_ms = wall_elapsed_ms - expected_ms - 100;
            uint32_t fill = 0;
            if (deficit_ms > 0) {
                uint64_t f = static_cast<uint64_t>(deficit_ms) *
                             static_cast<uint64_t>(cfg_.sample_rate) / 1000;
                const uint64_t cap = static_cast<uint64_t>(chunk) * 2;
                if (f > cap) f = cap;
                fill = static_cast<uint32_t>(f);
            }
            uint32_t remaining = fill;
            while (remaining > 0) {
                const uint32_t n = std::min(remaining, chunk);
                std::memset(mic_buf_.data(), 0, static_cast<size_t>(n) * ch * sizeof(float));
                const int64_t t = audio_frames_ * 10'000'000LL / cfg_.sample_rate;
                if (!encoder_->push_audio_frames(mic_buf_.data(), n, t)) break;
                audio_frames_ += n;
                segment_has_samples_ = true;
                remaining -= n;
            }
        }
    }

    // Track audible apps for the per-clip sidecar. 2s cadence keeps the COM
    // enumeration cost trivial; same thread as save_clip, no locking needed.
    if (app_monitor_ && now_ms_ - last_app_poll_ms_ >= 2000) {
        last_app_poll_ms_ = now_ms_;
        const int64_t rel_ms = static_cast<int64_t>(now_ms_);
        const std::vector<audio::AudibleApp> audible = app_monitor_->poll_audible();
        for (const auto& app : audible) {
            HeardApp& rec = heard_apps_[app.pid];
            if (rec.polls == 0) {
                rec.exe = app.exe;
                rec.first_ms = rel_ms;
            }
            rec.last_ms = rel_ms;
            ++rec.polls;
        }
        stems_on_poll(audible);
    }

    stems_drain();

    if (now_ms_ - segment_start_ms_ >= cfg_.segment_ms) {
        if (!finalize_segment()) return false;
        segment_start_ms_ = now_ms_;
        if (!start_segment()) return false;
    }
    return true;
}

void ReplayRecorder::set_game_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(name_mutex_);
    game_name_ = name;
}

std::wstring ReplayRecorder::make_clip_path() const {
    std::string prefix;
    {
        std::lock_guard<std::mutex> lock(name_mutex_);
        prefix = game_name_;
    }
    const std::string base =
        prefix.empty() ? "ClipLite" : cliplite::util::sanitize_filename(prefix);

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%s_%04d-%02d-%02d_%02d-%02d-%02d.mp4",
                  utf8_to_wide(base).c_str(), tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return cfg_.clip_dir + L"\\" + buf;
}

std::wstring ReplayRecorder::save_clip() {
    if (!running_) return {};
    if (!finalize_segment()) return {};

    const int64_t end_ms = static_cast<int64_t>(now_ms_);
    const int64_t start_ms = end_ms - static_cast<int64_t>(cfg_.replay_ms);
    const auto segs = ring_->collect(start_ms, end_ms);

    std::vector<std::wstring> files;
    for (const auto& s : segs) files.push_back(utf8_to_wide(s.path));
    if (files.empty()) {
        segment_start_ms_ = now_ms_;
        start_segment();
        return {};
    }

    // Compute head/tail trims so the remuxed clip matches [start_ms, end_ms)
    // instead of whole files (previously up to +segment_ms too long).
    int64_t head_trim_ms = 0;
    int64_t tail_trim_ms = 0;
    {
        std::unordered_map<std::string, Segment> orig_by_path;
        for (const auto& o : ring_->segments()) orig_by_path[o.path] = o;
        const auto it_first = orig_by_path.find(segs.front().path);
        if (it_first != orig_by_path.end()) {
            // Trim against the FILE's true start (orig_start_ms), not the
            // pruned window start: prune() slides start_ms forward while the
            // head bytes stay on disk, and trimming against the slid value
            // silently keeps up to a full segment of extra footage.
            int64_t file_start = it_first->second.orig_start_ms;
            if (file_start > segs.front().start_ms)
                file_start = it_first->second.start_ms;  // inconsistent: be conservative
            head_trim_ms = segs.front().start_ms - file_start;
            if (head_trim_ms < 0) head_trim_ms = 0;
        }
        const auto it_last = orig_by_path.find(segs.back().path);
        if (it_last != orig_by_path.end()) {
            tail_trim_ms = it_last->second.end_ms - segs.back().end_ms;
            if (tail_trim_ms < 0) tail_trim_ms = 0;
        }
    }

    {
        std::string dbg = "save window: segs=" + std::to_string(segs.size()) +
                          " head_trim=" + std::to_string(head_trim_ms) +
                          " tail_trim=" + std::to_string(tail_trim_ms) + " [";
        for (const auto& s : segs)
            dbg += "(" + std::to_string(s.start_ms) + "-" + std::to_string(s.end_ms) + ")";
        dbg += "]";
        CL_INFO("Recorder", dbg);
    }

    const std::wstring out = make_clip_path();
    if (!remux_mp4s(files, out, head_trim_ms, tail_trim_ms)) {
        CL_ERROR("Recorder", "clip remux failed");
        DeleteFileW(out.c_str());
        segment_start_ms_ = now_ms_;
        start_segment();
        return {};
    }

    CL_INFO("Recorder", "clip saved: " + wide_to_utf8(out));
    StemSnapshot snap = snapshot_stems(segs, out);
    snap.head_trim_ms = head_trim_ms;
    snap.tail_trim_ms = tail_trim_ms;
    write_app_sidecar(out, snap);
    segment_start_ms_ = now_ms_;
    start_segment();
    return out;
}

namespace {
void write_json_string(std::ofstream& f, const std::string& s) {
    f << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') f << '\\';
        f << c;
    }
    f << '"';
}
}  // namespace

// Copies the window's stem WAV siblings into "<clip>.stems/<k>.app_<pid>.wav"
// (k = chronological position in the window) so the mixdown render reads
// stable files instead of the evicting buffer dir. Best-effort per file:
// a missing/silent stem just marks has_data=false for that pid.
ReplayRecorder::StemSnapshot ReplayRecorder::snapshot_stems(
    const std::vector<Segment>& segs, const std::wstring& clip_path) {
    StemSnapshot snap;
    if (!cfg_.stem_capture || segs.empty()) return snap;
    if (clip_path.size() < 4 ||
        clip_path.compare(clip_path.size() - 4, 4, L".mp4") != 0) {
        return snap;
    }
    const std::wstring dir = clip_path.substr(0, clip_path.size() - 4) + L".stems";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        CL_WARN("Recorder", "stem snapshot: mkdir failed");
        return snap;
    }
    snap.dir_name = std::filesystem::path(dir).filename().wstring();
    for (size_t k = 0; k < segs.size(); ++k) {
        const std::wstring seg_w = utf8_to_wide(segs[k].path);
        const auto slash = seg_w.find_last_of(L"\\/");
        const std::wstring seg_dir =
            (slash == std::wstring::npos) ? L"." : seg_w.substr(0, slash);
        std::wstring stem_base = (slash == std::wstring::npos) ? seg_w : seg_w.substr(slash + 1);
        const std::wstring suf = L".tmp.mp4";
        if (stem_base.size() <= suf.size() ||
            stem_base.compare(stem_base.size() - suf.size(), suf.size(), suf) != 0) {
            continue;
        }
        stem_base.replace(stem_base.size() - suf.size(), suf.size(), L".app_");
        for (const auto& entry : std::filesystem::directory_iterator(seg_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(stem_base, 0) != 0 || name.size() < 4 ||
                name.compare(name.size() - 4, 4, L".wav") != 0) {
                continue;
            }
            // name = segment_<id>.app_<pid>.wav -> pid between ".app_" and ".wav".
            const auto pid_pos = name.find(L".app_");
            const uint32_t pid = static_cast<uint32_t>(std::wcstoul(
                name.c_str() + pid_pos + 5, nullptr, 10));
            const std::wstring dst =
                dir + L"\\" + std::to_wstring(k) + L".app_" + std::to_wstring(pid) + L".wav";
            std::filesystem::copy_file(entry.path(), dst,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                CL_WARN("Recorder", "stem snapshot: copy failed");
                continue;
            }
            uintmax_t bytes = std::filesystem::file_size(dst, ec);
            if (!ec && bytes > 44) snap.has_data[pid] = true;
            if (snap.has_data.find(pid) == snap.has_data.end()) snap.has_data[pid] = false;
        }
    }
    CL_INFO("Recorder", "stem snapshot: " + std::to_string(snap.has_data.size()) +
                            " app(s) -> " + wide_to_utf8(snap.dir_name));
    return snap;
}

// Writes "<clip>.apps.json" listing every pid heard during this session.
// The post-clip picker shows these with all selected by default; unchecking
// an app re-renders without its stem. Best-effort: never fails the save.
void ReplayRecorder::write_app_sidecar(const std::wstring& clip_path,
                                       const StemSnapshot& snap) {
    const std::wstring sidecar = clip_path + L".apps.json";
    std::ofstream f(sidecar, std::ios::binary | std::ios::trunc);
    if (!f) {
        CL_WARN("Recorder", "app sidecar: open failed");
        return;
    }
    f << "{\"apps\":[";
    bool first = true;
    for (const auto& [pid, rec] : heard_apps_) {
        if (!first) f << ',';
        first = false;
        f << "{\"pid\":" << pid << ",\"exe\":";
        write_json_string(f, rec.exe);
        f << ",\"first_ms\":" << rec.first_ms << ",\"last_ms\":" << rec.last_ms;
        const auto it = snap.has_data.find(pid);
        f << ",\"stem\":" << ((it != snap.has_data.end() && it->second) ? "true" : "false")
          << "}";
    }
    f << "]";
    if (!snap.dir_name.empty()) {
        f << ",\"stems\":";
        write_json_string(f, wide_to_utf8(snap.dir_name));
        f << ",\"head_trim_ms\":" << snap.head_trim_ms
          << ",\"tail_trim_ms\":" << snap.tail_trim_ms << ",\"replay_ms\":"
          << static_cast<uint32_t>(cfg_.replay_ms);
    }
    f << "}";
    f.close();
    if (!f) {
        CL_WARN("Recorder", "app sidecar: write failed");
        DeleteFileW(sidecar.c_str());
        return;
    }
    CL_INFO("Recorder", "app sidecar: " + std::to_string(heard_apps_.size()) + " app(s)");
}

std::wstring ReplayRecorder::stem_path_for(uint32_t pid) const {
    // segment_<id>.app_<pid>.wav lives next to segment_<id>.tmp.mp4 so ring
    // eviction, outage drops, and stop() can delete siblings by name.
    return cfg_.buffer_dir + L"\\segment_" + std::to_wstring(current_segment_id_) +
           L".app_" + std::to_wstring(pid) + L".wav";
}

bool ReplayRecorder::stems_open_writer_for(StemCap& sc, uint32_t pid) {
    sc.writer = std::make_unique<audio::StemSegmentWriter>();
    if (current_segment_id_ < 0) return false;
    if (!sc.writer->open(wide_to_utf8(stem_path_for(pid)))) {
        CL_WARN("Recorder", "stem: wav open failed for pid=" + std::to_string(pid));
        sc.writer.reset();
        return false;
    }
    return true;
}

void ReplayRecorder::stems_open_writers() {
    if (!cfg_.stem_capture) return;
    std::vector<uint32_t> dead;
    for (auto& [pid, sc] : stem_caps_) {
        // Rotation: finalize whatever the previous segment collected first.
        if (sc.writer) sc.writer->close();
        if (!stems_open_writer_for(sc, pid)) dead.push_back(pid);
    }
    for (uint32_t pid : dead) {
        stem_caps_.erase(pid);
        stem_failed_.insert(pid);
    }
    // Microphone stem: 48kHz stereo float like app stems (snapshot contract).
    // Odd formats are skipped with a warning instead of corrupting the mix.
    if (mic_) {
        mic_writer_.reset();
        const uint32_t rate = mic_->sample_rate();
        const uint16_t mch = mic_->channels();
        if (rate != 48000 || (mch != 1 && mch != 2)) {
            CL_WARN("Recorder", "mic stem skipped (rate=" + std::to_string(rate) +
                                    " ch=" + std::to_string(mch) +
                                    "); need 48kHz mono/stereo");
        } else {
            mic_writer_ = std::make_unique<audio::StemSegmentWriter>();
            if (!mic_writer_->open(wide_to_utf8(stem_path_for(kMicStemPid)))) {
                CL_WARN("Recorder", "mic stem: wav open failed");
                mic_writer_.reset();
            }
        }
    }
}

void ReplayRecorder::stems_close_writers() {
    for (auto& [pid, sc] : stem_caps_) {
        if (sc.writer) sc.writer->close();
    }
    if (mic_writer_) mic_writer_->close();
}

void ReplayRecorder::stems_on_poll(const std::vector<audio::AudibleApp>& audible) {
    if (!cfg_.stem_capture) return;
    const uint32_t max_apps = cfg_.max_stem_apps < 1 ? 1 : cfg_.max_stem_apps;
    std::set<uint32_t> present;
    for (const auto& app : audible) {
        if (app.pid == 0) continue;  // system-sounds session: no capturable pid
        present.insert(app.pid);
        auto it = stem_caps_.find(app.pid);
        if (it != stem_caps_.end()) {
            it->second.absent_polls = 0;
            continue;
        }
        if (stem_failed_.count(app.pid)) continue;
        if (stem_caps_.size() >= max_apps) {
            CL_DEBUG("Recorder", "stem: at cap, ignoring pid=" + std::to_string(app.pid));
            continue;
        }
        auto cap = std::make_unique<audio::ProcessLoopbackCapture>();
        if (!cap->start(app.pid)) {
            // Dead/protected pid: don't retry-spam for the rest of the session.
            stem_failed_.insert(app.pid);
            continue;
        }
        StemCap sc;
        sc.cap = std::move(cap);
        sc.exe = app.exe;
        if (!stems_open_writer_for(sc, app.pid)) {
            stem_failed_.insert(app.pid);
            continue;
        }
        stem_caps_.emplace(app.pid, std::move(sc));
        CL_INFO("Recorder", "stem: capturing " + app.exe +
                                " (pid=" + std::to_string(app.pid) + ")");
    }
    // Retire apps unheard for 3 consecutive polls (~6s): pid exited or went
    // silent at the session level. Respawn is allowed if heard again.
    for (auto it = stem_caps_.begin(); it != stem_caps_.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        if (++it->second.absent_polls >= 3) {
            if (it->second.writer) it->second.writer->close();
            CL_INFO("Recorder", "stem: retiring pid=" + std::to_string(it->first));
            it = stem_caps_.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplayRecorder::stems_drain() {
    if (!cfg_.stem_capture || stem_caps_.empty() || stem_buf_.empty()) return;
    constexpr uint32_t kStemChunk = (48000 + 9) / 10;  // ~100ms at stem rate
    std::vector<uint32_t> dead;
    for (auto& [pid, sc] : stem_caps_) {
        if (!sc.cap || !sc.writer) continue;
        const uint32_t got = sc.cap->read(stem_buf_.data(), kStemChunk);
        if (got == 0) continue;
        if (!sc.writer->append(stem_buf_.data(), got)) {
            CL_WARN("Recorder", "stem: wav append failed for pid=" + std::to_string(pid));
            dead.push_back(pid);
        }
    }
    for (uint32_t pid : dead) {
        auto it = stem_caps_.find(pid);
        if (it != stem_caps_.end()) {
            if (it->second.writer) it->second.writer->close();
            stem_caps_.erase(it);
            stem_failed_.insert(pid);
        }
    }
}

void ReplayRecorder::delete_stem_siblings(const std::wstring& segment_path) {
    // segment_<id>.tmp.mp4 -> delete segment_<id>.app_*.wav in the same dir.
    const auto slash = segment_path.find_last_of(L"\\/");
    const std::wstring dir =
        (slash == std::wstring::npos) ? L"." : segment_path.substr(0, slash);
    const std::wstring file =
        (slash == std::wstring::npos) ? segment_path : segment_path.substr(slash + 1);
    const std::wstring seg_tok = L"segment_";
    const std::wstring seg_suf = L".tmp.mp4";
    if (file.rfind(seg_tok, 0) != 0 || file.size() <= seg_tok.size() + seg_suf.size() ||
        file.compare(file.size() - seg_suf.size(), seg_suf.size(), seg_suf) != 0) {
        return;
    }
    const std::wstring id =
        file.substr(seg_tok.size(), file.size() - seg_tok.size() - seg_suf.size());
    const std::wstring prefix = seg_tok + id + L".app_";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) == 0 && name.size() >= 4 &&
            name.compare(name.size() - 4, 4, L".wav") == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

}  // namespace cliplite::replay
