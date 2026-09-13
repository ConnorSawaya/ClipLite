#include "cliplite/audio/stem_track.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "cliplite/audio/wav_peaks.h"

namespace cliplite::audio {

namespace {
std::vector<float> load_stem_wav(const std::filesystem::path& path, uint32_t sample_rate,
                                 uint16_t channels) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff n = f.tellg();
    if (n < 44) return {};
    f.seekg(0);
    char h[44] = {0};
    f.read(h, 44);
    if (!f || std::string(h, h + 4) != "RIFF" || std::string(h + 8, h + 12) != "WAVE" ||
        std::string(h + 12, h + 16) != "fmt " || std::string(h + 36, h + 40) != "data") {
        return {};
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, bytes = 0;
    std::memcpy(&fmt, h + 20, 2);
    std::memcpy(&ch, h + 22, 2);
    std::memcpy(&rate, h + 24, 4);
    std::memcpy(&bits, h + 34, 2);
    std::memcpy(&bytes, h + 40, 4);
    if ((fmt != 3 && fmt != 1) || rate != sample_rate || ch != channels) return {};
    const uint32_t bps = (fmt == 3) ? 4 : 2;
    if (bits != bps * 8) return {};
    if (bytes == 0 || bytes > static_cast<uint32_t>(n - 44)) return {};
    if (bytes % (channels * bps) != 0) return {};
    std::vector<float> pcm(bytes / bps);
    if (fmt == 3) {
        f.read(reinterpret_cast<char*>(pcm.data()), bytes);
        if (!f) return {};
    } else {
        std::vector<char> raw(bytes);
        f.read(raw.data(), bytes);
        if (!f) return {};
        const int16_t* v = reinterpret_cast<const int16_t*>(raw.data());
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(v[i]) / 32768.f;
    }
    return pcm;
}
}  // namespace

std::map<uint32_t, StemGroup> load_stem_snapshot(const std::string& utf8_dir) {
    std::map<uint32_t, StemGroup> groups;
    if (utf8_dir.empty()) return groups;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(utf8_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        // "<k>.app_<pid>.wav"
        const auto dot = name.find('.');
        const auto app = name.find(".app_");
        if (dot == std::string::npos || app == std::string::npos) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
        uint32_t k = 0, pid = 0;
        try {
            k = static_cast<uint32_t>(std::stoul(name.substr(0, dot)));
            pid = static_cast<uint32_t>(
                std::stoul(name.substr(app + 5, name.size() - app - 5 - 4)));
        } catch (...) {
            continue;
        }
        auto pcm = load_stem_wav(entry.path(), 48000, 2);
        if (pcm.empty()) continue;
        groups[k].k = k;
        groups[k].stems[pid] = std::move(pcm);
    }
    return groups;
}

std::vector<float> mix_stem_window(const std::map<uint32_t, StemGroup>& groups,
                                   const std::set<uint32_t>& excluded_pids,
                                   int64_t head_trim_ms, int64_t tail_trim_ms,
                                   uint32_t sample_rate, uint16_t channels) {
    std::vector<float> mixed;
    if (sample_rate == 0 || channels == 0) return mixed;
    if (head_trim_ms < 0) head_trim_ms = 0;
    if (tail_trim_ms < 0) tail_trim_ms = 0;
    for (const auto& [k, group] : groups) {
        (void)k;
        size_t frames = 0;
        for (const auto& [pid, pcm] : group.stems) {
            if (excluded_pids.count(pid)) continue;
            frames = std::max(frames, pcm.size() / channels);
        }
        if (frames == 0) continue;
        const size_t base = mixed.size();
        mixed.resize(base + frames * channels, 0.f);
        for (const auto& [pid, pcm] : group.stems) {
            if (excluded_pids.count(pid)) continue;
            for (size_t i = 0; i < pcm.size(); ++i) mixed[base + i] += pcm[i];
        }
    }
    // Peak-normalize instead of flat-top clipping: preserves waveform shape
    // (transparent for quiet mixes, far less garble for hot game+music sums).
    float peak = 0.f;
    for (float s : mixed) {
        const float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    if (peak > 1.f) {
        for (float& s : mixed) s /= peak;
    }
    const size_t head =
        static_cast<size_t>(head_trim_ms) * sample_rate / 1000 * channels;
    const size_t tail =
        static_cast<size_t>(tail_trim_ms) * sample_rate / 1000 * channels;
    if (head < mixed.size()) mixed.erase(mixed.begin(), mixed.begin() + head);
    else mixed.clear();
    if (tail < mixed.size()) mixed.resize(mixed.size() - tail);
    else mixed.clear();
    return mixed;
}

std::vector<float> resample_pcm_linear(const std::vector<float>& pcm, uint16_t channels,
                                       double speed) {
    std::vector<float> out;
    if (pcm.empty() || channels == 0 || !(speed > 0.0)) return out;
    if (speed == 1.0) return pcm;
    const size_t in_frames = pcm.size() / channels;
    if (in_frames == 0) return out;
    if (in_frames == 1) {
        const size_t out_frames = static_cast<size_t>(1.0 / speed);
        out.assign(out_frames ? out_frames * channels : channels, 0.f);
        for (size_t i = 0; i < out.size(); i += channels) {
            for (uint16_t c = 0; c < channels; ++c) out[i + c] = pcm[c];
        }
        return out;
    }
    const size_t out_frames = static_cast<size_t>(
        static_cast<double>(in_frames) / speed);
    if (out_frames == 0) return out;
    out.resize(out_frames * channels);
    for (size_t o = 0; o < out_frames; ++o) {
        const double pos = static_cast<double>(o) * speed;
        size_t i0 = static_cast<size_t>(pos);
        if (i0 >= in_frames - 1) i0 = in_frames - 2;
        const double frac = pos - static_cast<double>(i0);
        for (uint16_t c = 0; c < channels; ++c) {
            const float a = pcm[i0 * channels + c];
            const float b = pcm[(i0 + 1) * channels + c];
            out[o * channels + c] = static_cast<float>(a + (b - a) * frac);
        }
    }
    return out;
}

void apply_fade_inout(std::vector<float>& interleaved, uint16_t channels,
                      uint32_t fade_in_frames, uint32_t fade_out_frames) {
    if (interleaved.empty() || channels == 0) return;
    const size_t frames = interleaved.size() / channels;
    for (uint32_t f = 0; f < fade_in_frames && f < frames; ++f) {
        const float g = static_cast<float>(f + 1) / static_cast<float>(fade_in_frames + 1);
        for (uint16_t c = 0; c < channels; ++c) interleaved[f * channels + c] *= g;
    }
    for (uint32_t f = 0; f < fade_out_frames && f < frames; ++f) {
        const size_t idx = frames - 1 - f;
        const float g = static_cast<float>(f + 1) / static_cast<float>(fade_out_frames + 1);
        for (uint16_t c = 0; c < channels; ++c) interleaved[idx * channels + c] *= g;
    }
}

std::vector<float> mix_snapshot_streaming(const std::string& utf8_dir,
                                          const std::map<uint32_t, bool>& has_stem,
                                          const std::set<uint32_t>& excluded_pids,
                                          int64_t head_trim_ms, int64_t tail_trim_ms,
                                          uint32_t sample_rate, uint16_t channels) {
    std::map<uint32_t, float> gains;
    for (uint32_t pid : excluded_pids) gains[pid] = 0.f;
    // Absent pids default to 1.0 inside the gains variant.
    return mix_snapshot_streaming_gains(utf8_dir, has_stem, gains, head_trim_ms,
                                        tail_trim_ms, sample_rate, channels);
}

std::vector<float> mix_snapshot_streaming_gains(
    const std::string& utf8_dir, const std::map<uint32_t, bool>& has_stem,
    const std::map<uint32_t, float>& gains, int64_t head_trim_ms, int64_t tail_trim_ms,
    uint32_t sample_rate, uint16_t channels) {
    std::vector<float> mixed;
    if (utf8_dir.empty() || sample_rate == 0 || channels == 0) return mixed;
    if (head_trim_ms < 0) head_trim_ms = 0;
    if (tail_trim_ms < 0) tail_trim_ms = 0;
    const auto paths = group_snapshot_paths(utf8_dir);
    for (const auto& [k, per_pid] : paths) {
        (void)k;
        // Load only this k-slice, mix it, then let it die with the iteration.
        std::map<uint32_t, std::vector<float>> stems;
        std::map<uint32_t, float> slice_gain;
        for (const auto& [pid, path] : per_pid) {
            float g = 1.f;
            const auto git = gains.find(pid);
            if (git != gains.end()) g = git->second;
            if (!(g > 0.f)) continue;  // removed (also covers NaN)
            if (g > 2.f) g = 2.f;
            if (!has_stem.empty()) {
                const auto known = has_stem.find(pid);
                if (known == has_stem.end() || !known->second) continue;
            }
            auto pcm = load_stem_wav(path, sample_rate, channels);
            if (!pcm.empty()) {
                stems[pid] = std::move(pcm);
                slice_gain[pid] = g;
            }
        }
        size_t frames = 0;
        for (const auto& [pid, pcm] : stems) {
            (void)pid;
            frames = std::max(frames, pcm.size() / channels);
        }
        if (frames == 0) continue;
        const size_t base = mixed.size();
        mixed.resize(base + frames * channels, 0.f);
        for (const auto& [pid, pcm] : stems) {
            const float g = slice_gain[pid];
            if (g == 1.f) {
                for (size_t i = 0; i < pcm.size(); ++i) mixed[base + i] += pcm[i];
            } else {
                for (size_t i = 0; i < pcm.size(); ++i) mixed[base + i] += pcm[i] * g;
            }
        }
    }
    float peak = 0.f;
    for (float s : mixed) {
        const float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    if (peak > 1.f) {
        for (float& s : mixed) s /= peak;
    }
    const size_t head =
        static_cast<size_t>(head_trim_ms) * sample_rate / 1000 * channels;
    const size_t tail =
        static_cast<size_t>(tail_trim_ms) * sample_rate / 1000 * channels;
    if (head < mixed.size()) mixed.erase(mixed.begin(), mixed.begin() + head);
    else mixed.clear();
    if (tail < mixed.size()) mixed.resize(mixed.size() - tail);
    else mixed.clear();
    return mixed;
}

}  // namespace cliplite::audio
