#include "cliplite/library/stem_mixdown.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "cliplite/audio/stem_track.h"
#include "cliplite/audio/wav_peaks.h"
#include "cliplite/log.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::library {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

constexpr uint32_t kStemRate = 48000;
constexpr uint16_t kStemChannels = 2;
constexpr uint32_t kAudioChunkFrames = 4800;  // ~100ms per AAC input sample

std::string read_all_narrow(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = f.tellg();
    if (n <= 0 || n > 1 << 20) return {};  // sidecars are tiny; refuse absurd ones
    std::string body(static_cast<size_t>(n), '\0');
    f.seekg(0);
    f.read(body.data(), n);
    return f ? body : std::string{};
}

// --- Minimal parser for our fixed sidecar schema (writer is deterministic).
// Finds "key":<value> scanning left to right; strings honor \" and \\.
bool find_key(const std::string& s, size_t from, const char* key, size_t& colon) {
    const std::string pat = std::string("\"") + key + "\":";
    colon = s.find(pat, from);
    if (colon == std::string::npos) return false;
    colon += pat.size();
    return true;
}
bool parse_u32(const std::string& s, size_t& pos, uint32_t& out) {
    size_t e = pos;
    while (e < s.size() && s[e] >= '0' && s[e] <= '9') ++e;
    if (e == pos) return false;
    out = static_cast<uint32_t>(std::wcstoul(
        cliplite::util::utf8_to_wide(s.substr(pos, e - pos)).c_str(), nullptr, 10));
    pos = e;
    return true;
}
bool parse_i64(const std::string& s, size_t& pos, int64_t& out) {
    bool neg = false;
    if (pos < s.size() && s[pos] == '-') {
        neg = true;
        ++pos;
    }
    uint32_t u = 0;
    if (!parse_u32(s, pos, u)) return false;
    out = neg ? -static_cast<int64_t>(u) : static_cast<int64_t>(u);
    return true;
}
bool parse_json_string(const std::string& s, size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\' && pos < s.size()) {
            out.push_back(s[pos++]);
            continue;
        }
        out.push_back(c);
    }
    return false;
}
}  // namespace

bool read_stem_sidecar(const std::wstring& clip_path, StemClipInfo* info) {
    if (!info) return false;
    *info = StemClipInfo{};
    const std::string body = read_all_narrow(clip_path + L".apps.json");
    if (body.empty() || body.front() != '{') return false;

    // apps[] entries: {"pid":N,"exe":"..",...,"stem":true|false}
    size_t pos = 0, colon = 0;
    if (!find_key(body, 0, "apps", colon)) return false;
    pos = body.find('[', colon);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && body[pos] != ']') {
        size_t k = 0;
        if (!find_key(body, pos, "pid", k)) break;
        uint32_t pid = 0;
        if (!parse_u32(body, k, pid)) break;
        if (!find_key(body, k, "exe", k)) break;
        std::string exe;
        if (!parse_json_string(body, k, exe)) break;
        bool stem = false;
        if (find_key(body, k, "stem", k)) {
            stem = body.compare(k, 4, "true") == 0;
            k += stem ? 4 : 5;
        }
        info->apps[pid] = exe;
        info->has_stem[pid] = stem;
        const size_t end = body.find('}', k);
        if (end == std::string::npos) break;
        pos = end + 1;
        if (pos < body.size() && body[pos] == ',') ++pos;
    }

    // Optional v2 keys.
    if (find_key(body, 0, "stems", colon)) {
        std::string dir;
        if (parse_json_string(body, colon, dir) && !dir.empty()) {
            const std::wstring wclip = clip_path;
            const auto slash = wclip.find_last_of(L"\\/");
            const std::wstring parent =
                (slash == std::wstring::npos) ? L"." : wclip.substr(0, slash);
            info->stem_dir =
                parent + L"\\" + cliplite::util::utf8_to_wide(dir);
        }
    }
    if (find_key(body, 0, "head_trim_ms", colon)) parse_i64(body, colon, info->head_trim_ms);
    if (find_key(body, 0, "tail_trim_ms", colon)) parse_i64(body, colon, info->tail_trim_ms);
    if (info->head_trim_ms < 0) info->head_trim_ms = 0;
    if (info->tail_trim_ms < 0) info->tail_trim_ms = 0;
    return true;
}

bool export_without_apps(const std::wstring& input, const std::wstring& output,
                         const std::set<uint32_t>& excluded_pids) {
    StemClipInfo info;
    const bool have_sidecar = read_stem_sidecar(input, &info);

    // Stream-mix the snapshot window (one k-slice in RAM at a time; stray
    // WAVs from pid reuse and stem:false entries are ignored inside).
    std::vector<float> mixed;
    bool have_stems = false;
    if (have_sidecar && !info.stem_dir.empty()) {
        // Presence probe: any loadable stem file at all?
        auto paths = audio::group_snapshot_paths(
            cliplite::util::wide_to_utf8(info.stem_dir));
        for (const auto& [k, m] : paths) {
            (void)k;
            for (const auto& [pid, p] : m) {
                (void)p;
                const auto known = info.has_stem.find(pid);
                if (known != info.has_stem.end() && known->second) {
                    have_stems = true;
                    break;
                }
            }
            if (have_stems) break;
        }
        if (have_stems) {
            mixed = audio::mix_snapshot_streaming(
                cliplite::util::wide_to_utf8(info.stem_dir), info.has_stem, excluded_pids,
                info.head_trim_ms, info.tail_trim_ms, kStemRate, kStemChannels);
        }
    }
    // have_stems==true but every included pid excluded (or trims ate all):
    // groups empty or mixed empty -> video-only output.
    const bool want_audio_mix = have_stems && !mixed.empty();
    // No stems at all -> fall back to reader audio passthrough (old clips).
    const bool want_audio_copy = !have_stems;

    const HRESULT startup = MFStartup(MF_VERSION);
    bool ok = false;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(input.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        CL_ERROR("Mixdown", "open input failed (hr=" + hr_hex(hr) + ")");
        if (SUCCEEDED(startup)) MFShutdown();
        return false;
    }

    // Discover first video (+ first audio for COPY fallback) stream indices.
    int src_video = -1, src_audio = -1;
    for (DWORD i = 0; i < 4; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(i, 0, &native))) break;
        GUID major{};
        native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Video && src_video < 0) src_video = static_cast<int>(i);
        if (major == MFMediaType_Audio && src_audio < 0) src_audio = static_cast<int>(i);
    }
    if (src_video < 0) {
        CL_ERROR("Mixdown", "no video stream");
        reader.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        CL_ERROR("Mixdown", "create sink failed (hr=" + hr_hex(hr) + ")");
        reader.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        return false;
    }

    // Video: compressed passthrough of the native type.
    DWORD out_video = 0;
    {
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        hr = reader->GetNativeMediaType(static_cast<DWORD>(src_video), 0, &native);
        if (FAILED(hr) ||
            FAILED(writer->AddStream(native.Get(), &out_video)) ||
            FAILED(writer->SetInputMediaType(out_video, native.Get(), nullptr))) {
            CL_ERROR("Mixdown", "video stream setup failed");
            reader.Reset();
            writer.Reset();
            if (SUCCEEDED(startup)) MFShutdown();
            return false;
        }
        reader->SetCurrentMediaType(static_cast<DWORD>(src_video), nullptr, native.Get());
    }

    // Audio output: AAC-encode of the mixed stems, or native passthrough copy.
    DWORD out_audio = 0;
    bool audio_encode = false, audio_copy = false;
    if (want_audio_mix) {
        Microsoft::WRL::ComPtr<IMFMediaType> aout, ain;
        hr = MFCreateMediaType(&aout);
        if (SUCCEEDED(hr)) {
            aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kStemRate);
            aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kStemChannels);
            aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192'000 / 8);
            hr = writer->AddStream(aout.Get(), &out_audio);
        }
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&ain);
        if (SUCCEEDED(hr)) {
            ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kStemRate);
            ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kStemChannels);
            ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kStemChannels * 4);
            ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           kStemRate * kStemChannels * 4);
            hr = writer->SetInputMediaType(out_audio, ain.Get(), nullptr);
        }
        if (SUCCEEDED(hr)) {
            audio_encode = true;
        } else {
            CL_WARN("Mixdown", "AAC setup failed, falling back to video-only");
        }
    } else if (want_audio_copy && src_audio >= 0) {
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(src_audio), 0, &native)) &&
            SUCCEEDED(writer->AddStream(native.Get(), &out_audio)) &&
            SUCCEEDED(writer->SetInputMediaType(out_audio, native.Get(), nullptr))) {
            reader->SetCurrentMediaType(static_cast<DWORD>(src_audio), nullptr, native.Get());
            audio_copy = true;
        } else {
            CL_WARN("Mixdown", "audio passthrough setup failed, video-only");
        }
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        CL_ERROR("Mixdown", "BeginWriting failed (hr=" + hr_hex(hr) + ")");
        reader.Reset();
        writer.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        DeleteFileW(output.c_str());
        return false;
    }

    // Stream video; feed mixed-audio chunks due before each video timestamp
    // (same interleave pattern as the edit pipeline's pump_audio_until).
    size_t audio_frame = 0;
    const size_t audio_frames = mixed.size() / kStemChannels;
    bool eos_video = false, eos_audio = !audio_copy;
    int safety = 0;
    auto pump_audio_until = [&](LONGLONG video_time_100ns) {
        while (audio_encode && audio_frame < audio_frames) {
            const size_t take =
                std::min<size_t>(kAudioChunkFrames, audio_frames - audio_frame);
            const LONGLONG t =
                static_cast<LONGLONG>(audio_frame) * 10'000'000LL / kStemRate;
            if (t > video_time_100ns) break;
            const uint32_t bytes = static_cast<uint32_t>(take) * kStemChannels * 4;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) break;
            BYTE* dst = nullptr;
            if (FAILED(buf->Lock(&dst, nullptr, nullptr))) break;
            std::memcpy(dst, mixed.data() + audio_frame * kStemChannels, bytes);
            buf->Unlock();
            buf->SetCurrentLength(bytes);
            Microsoft::WRL::ComPtr<IMFSample> sample;
            if (FAILED(MFCreateSample(&sample))) break;
            sample->AddBuffer(buf.Get());
            sample->SetSampleTime(t);
            sample->SetSampleDuration(static_cast<LONGLONG>(take) * 10'000'000LL /
                                      kStemRate);
            if (FAILED(writer->WriteSample(out_audio, sample.Get()))) break;
            audio_frame += take;
        }
    };

    LONGLONG shared_base = -1;
    while ((!eos_video || !eos_audio) && safety++ < 1'000'000) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
                                &stream_index, &flags, &ts, &sample);
        if (FAILED(hr)) break;
        const bool is_video = static_cast<int>(stream_index) == src_video;
        const bool is_audio = src_audio >= 0 && static_cast<int>(stream_index) == src_audio;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (is_video) eos_video = true;
            if (is_audio) eos_audio = true;
            continue;
        }
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
        if (!is_video && !(is_audio && audio_copy)) continue;
        if (shared_base < 0) shared_base = ts;
        const LONGLONG t = ts - shared_base;
        sample->SetSampleTime(t);
        writer->WriteSample(is_video ? out_video : out_audio, sample.Get());
        if (is_video) pump_audio_until(t);
    }
    if (audio_encode) pump_audio_until(INT64_MAX);

    hr = writer->Finalize();
    reader.Reset();
    writer.Reset();
    if (SUCCEEDED(startup)) MFShutdown();
    if (FAILED(hr)) {
        CL_ERROR("Mixdown", "Finalize failed (hr=" + hr_hex(hr) + ")");
        DeleteFileW(output.c_str());
        return false;
    }
    ok = true;
    CL_INFO("Mixdown", "rendered (" + std::to_string(audio_frame) + " stem audio frames)");
    return ok;
}

}  // namespace cliplite::library
