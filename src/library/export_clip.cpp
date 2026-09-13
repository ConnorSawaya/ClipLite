#include "cliplite/library/export_clip.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "cliplite/log.h"

namespace cliplite::library {

namespace {

std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

// Passthrough copy of [start_hns, end_hns] into a fresh MP4. Requires MF to
// already be started on this thread.
bool passthrough_range(const std::wstring& input, const std::wstring& output,
                       int64_t start_hns, int64_t end_hns) {
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(input.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        CL_ERROR("Trim", "open input failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        CL_ERROR("Trim", "create sink writer failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // Switch every stream to compressed passthrough and mirror it in the writer.
    int src_video = -1;
    int src_audio = -1;
    DWORD out_video = 0;
    DWORD out_audio = 0;

    for (DWORD i = 0; i < 2; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(i, 0, &native))) break;
        GUID major{};
        native->GetGUID(MF_MT_MAJOR_TYPE, &major);

        if (major == MFMediaType_Video && src_video < 0) {
            src_video = static_cast<int>(i);
            DWORD sid = 0;
            if (FAILED(writer->AddStream(native.Get(), &sid)) ||
                FAILED(writer->SetInputMediaType(sid, native.Get(), nullptr))) {
                CL_ERROR("Trim", "add video stream failed");
                return false;
            }
            out_video = sid;
        } else if (major == MFMediaType_Audio && src_audio < 0) {
            src_audio = static_cast<int>(i);
            DWORD sid = 0;
            if (FAILED(writer->AddStream(native.Get(), &sid)) ||
                FAILED(writer->SetInputMediaType(sid, native.Get(), nullptr))) {
                CL_ERROR("Trim", "add audio stream failed");
                return false;
            }
            out_audio = sid;
        }
        reader->SetCurrentMediaType(i, nullptr, native.Get());
    }

    if (src_video < 0) {
        CL_ERROR("Trim", "no writable video stream");
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        CL_ERROR("Trim", "BeginWriting failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // Seek near the requested start.
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = start_hns;
    hr = reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(hr)) {
        CL_WARN("Trim", "SetCurrentPosition failed (hr=" + hr_hex(hr) + "); reading from 0");
    }

    LONGLONG shared_base = -1;
    bool video_done = false;
    bool audio_done = (src_audio < 0);
    int safety = 0;
    // Run until BOTH tracks are done (EOS or past end_hns) so a trailing
    // audio tail is not truncated when video ends first, and vice versa.
    // Range is [start_hns, end_hns] inclusive to match the header contract.
    while ((!video_done || !audio_done) && safety++ < 1'000'000) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream_index,
                                &flags, &ts, &sample);
        if (FAILED(hr)) break;

        const bool is_video = static_cast<int>(stream_index) == src_video;
        const bool is_audio =
            src_audio >= 0 && static_cast<int>(stream_index) == src_audio;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (is_video) video_done = true;
            if (is_audio) audio_done = true;
            // Unmapped stream EOS: ignore.
            continue;
        }
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
        if (!is_video && !is_audio) continue;
        // Per-track end handling: a video sample past end must not truncate
        // a lagging audio tail (and vice versa).
        if (ts > end_hns) {
            if (is_video) video_done = true;
            if (is_audio) audio_done = true;
            continue;
        }
        // Drop seek pre-roll on AUDIO only (AAC frames decode independently).
        // VIDEO pre-roll must be kept: SetCurrentPosition snaps to the earlier
        // keyframe and a passthrough cut starting on a delta frame is
        // undecodable (thumbnail/player fail). Keeping the keyframe pre-roll
        // leaves up to one GOP of extra head (test file: 200..700ms -> ~733ms)
        // but stays decodable; exact-length cuts use the re-encode path
        // (video_edit.cpp) instead. Matches the 300..800ms test tolerance.
        if (is_audio && ts < start_hns) continue;

        // One shared time base keeps audio/video offsets intact.
        if (shared_base < 0) shared_base = ts;
        sample->SetSampleTime(ts - shared_base);
        writer->WriteSample(is_video ? out_video : out_audio, sample.Get());
    }

    // Start beyond EOF (or fully outside range) yields no samples: fail so the
    // caller does not believe an empty trim succeeded.
    if (shared_base < 0) {
        writer->Finalize();
        return false;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        CL_ERROR("Trim", "Finalize failed (hr=" + hr_hex(hr) + ")");
        return false;
    }
    return true;
}

}  // namespace

bool export_trimmed(const std::wstring& input, const std::wstring& output, int64_t start_ms,
                    int64_t end_ms) {
    if (end_ms <= start_ms) return false;

    const HRESULT startup = MFStartup(MF_VERSION);
    const bool ok = passthrough_range(input, output, start_ms * 10'000, end_ms * 10'000);
    if (SUCCEEDED(startup)) MFShutdown();

    if (!ok) DeleteFileW(output.c_str());  // never leave a broken file behind
    return ok;
}

}  // namespace cliplite::library
