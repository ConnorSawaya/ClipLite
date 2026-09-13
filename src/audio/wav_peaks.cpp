#include "cliplite/audio/wav_peaks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cliplite::audio {

namespace {
bool read_wav_header(const std::string& path, uint16_t& fmt, uint16_t& ch, uint32_t& rate,
                     uint32_t& bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff total = f.tellg();
    if (total < 44) return false;
    f.seekg(0);
    char h[44] = {0};
    f.read(h, 44);
    if (!f || std::string(h, h + 4) != "RIFF" || std::string(h + 8, h + 12) != "WAVE" ||
        std::string(h + 12, h + 16) != "fmt " || std::string(h + 36, h + 40) != "data") {
        return false;
    }
    uint16_t bits = 0;
    std::memcpy(&fmt, h + 20, 2);
    std::memcpy(&ch, h + 22, 2);
    std::memcpy(&rate, h + 24, 4);
    std::memcpy(&bits, h + 34, 2);
    std::memcpy(&bytes, h + 40, 4);
    if ((fmt != 3 && fmt != 1) || bits == 0 || ch == 0) return false;
    const uint32_t bps = bits / 8;
    if (bps != 2 && bps != 4) return false;
    if (bytes == 0 || bytes > static_cast<uint32_t>(total - 44)) return false;
    return true;
}
}  // namespace

WavInfo wav_info(const std::string& utf8_path) {
    WavInfo info;
    if (utf8_path.empty()) return info;
    uint16_t fmt = 0, ch = 0;
    uint32_t rate = 0, bytes = 0;
    if (!read_wav_header(utf8_path, fmt, ch, rate, bytes)) return info;
    const uint32_t bps = (fmt == 3) ? 4 : 2;
    info.ok = true;
    info.frames = bytes / (ch * bps);
    info.rate = rate;
    info.channels = ch;
    return info;
}

std::map<uint32_t, std::map<uint32_t, std::string>> group_snapshot_paths(
    const std::string& utf8_dir) {
    std::map<uint32_t, std::map<uint32_t, std::string>> groups;
    if (utf8_dir.empty()) return groups;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(utf8_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
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
        groups[k][pid] = entry.path().string();
    }
    return groups;
}

std::vector<float> concat_pid_peaks(const std::map<uint32_t, std::string>& k_files,
                                    uint32_t buckets) {
    std::vector<float> out;
    if (k_files.empty() || buckets == 0) return out;
    struct Item {
        std::string path;
        uint32_t frames = 0;
        uint32_t share = 0;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    for (const auto& [k, path] : k_files) {
        (void)k;
        const WavInfo info = wav_info(path);
        if (!info.ok || info.frames == 0) continue;
        items.push_back({path, info.frames, 0});
        total += info.frames;
    }
    if (items.empty() || total == 0) return out;
    uint32_t assigned = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].share = (i + 1 == items.size())
                             ? buckets - assigned
                             : static_cast<uint32_t>(static_cast<uint64_t>(items[i].frames) *
                                                     buckets / total);
        assigned += items[i].share;
    }
    for (const auto& it : items) {
        if (it.share == 0) continue;
        auto peaks = compute_wav_peaks(it.path, it.share);
        if (peaks.size() < it.share) peaks.resize(it.share, 0.f);
        out.insert(out.end(), peaks.begin(), peaks.begin() + it.share);
    }
    if (out.size() < buckets) out.resize(buckets, 0.f);
    return out;
}

std::vector<float> compute_wav_peaks(const std::string& utf8_path, uint32_t buckets) {
    std::vector<float> peaks;
    if (utf8_path.empty() || buckets == 0) return peaks;

    std::ifstream f(utf8_path, std::ios::binary | std::ios::ate);
    if (!f) return peaks;
    const std::streamoff total = f.tellg();
    if (total < 44) return peaks;
    f.seekg(0);
    char h[44] = {0};
    f.read(h, 44);
    if (!f || std::string(h, h + 4) != "RIFF" || std::string(h + 8, h + 12) != "WAVE" ||
        std::string(h + 12, h + 16) != "fmt " || std::string(h + 36, h + 40) != "data") {
        return peaks;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t bytes = 0;
    std::memcpy(&fmt, h + 20, 2);
    std::memcpy(&ch, h + 22, 2);
    std::memcpy(&bits, h + 34, 2);
    std::memcpy(&bytes, h + 40, 4);
    if ((fmt != 3 && fmt != 1) || bits == 0 || ch == 0) return peaks;
    const uint32_t bytes_per_sample = bits / 8;
    if (bytes_per_sample != 2 && bytes_per_sample != 4) return peaks;
    if (bytes == 0 || bytes > static_cast<uint32_t>(total - 44)) return peaks;
    const uint64_t frames = bytes / (ch * bytes_per_sample);
    if (frames == 0) return peaks;

    peaks.assign(buckets, 0.f);
    // Stream in small blocks; each frame lands in exactly one bucket.
    constexpr uint32_t kBlockFrames = 4096;
    std::vector<char> block(static_cast<size_t>(kBlockFrames) * ch * bytes_per_sample);
    uint64_t done = 0;
    const bool is_float = (fmt == 3);
    while (done < frames && f) {
        const uint32_t want =
            static_cast<uint32_t>(std::min<uint64_t>(kBlockFrames, frames - done));
        f.read(block.data(), static_cast<std::streamsize>(want) * ch * bytes_per_sample);
        const std::streamsize got = f.gcount();
        const uint32_t got_frames = static_cast<uint32_t>(got / (ch * bytes_per_sample));
        for (uint32_t i = 0; i < got_frames; ++i) {
            float peak = 0.f;
            for (uint16_t c = 0; c < ch; ++c) {
                float s = 0.f;
                if (is_float) {
                    float v = 0.f;
                    std::memcpy(&v, block.data() + (static_cast<size_t>(i) * ch + c) * 4, 4);
                    s = std::fabs(v);
                } else {
                    int16_t v = 0;
                    std::memcpy(&v, block.data() + (static_cast<size_t>(i) * ch + c) * 2, 2);
                    s = std::fabs(static_cast<float>(v) / 32768.f);
                }
                if (s > peak) peak = s;
            }
            const uint32_t b =
                static_cast<uint32_t>((done + i) * buckets / frames);
            if (b < buckets && peak > peaks[b]) peaks[b] = peak;
        }
        done += got_frames;
        if (got_frames < want) break;  // short read / EOF
    }
    return peaks;
}

}  // namespace cliplite::audio
