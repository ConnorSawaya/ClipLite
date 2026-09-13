#include "cliplite/replay/clip_remux.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "cliplite/log.h"

namespace cliplite::replay {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}
}  // namespace

bool remux_mp4s(const std::vector<std::wstring>& inputs, const std::wstring& output,
                int64_t head_trim_ms, int64_t tail_trim_ms) {
    if (inputs.empty()) return false;
    if (head_trim_ms < 0) head_trim_ms = 0;
    if (tail_trim_ms < 0) tail_trim_ms = 0;
    const LONGLONG head_trim_hns = head_trim_ms * 10'000LL;
    const LONGLONG tail_trim_hns = tail_trim_ms * 10'000LL;

    const HRESULT startup = MFStartup(MF_VERSION);
    const bool mf_started = SUCCEEDED(startup);

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        CL_ERROR("Remux", "MFCreateSinkWriterFromURL failed (hr=" + hr_hex(hr) + ")");
        if (mf_started) MFShutdown();
        return false;
    }

    bool added_video = false;
    bool added_audio = false;
    DWORD vstream = 0;
    DWORD astream = 0;
    LONGLONG time_offset = 0;
    bool began_writing = false;

    for (const auto& input : inputs) {
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(input.c_str(), nullptr, &reader);
        if (FAILED(hr)) {
            CL_WARN("Remux", "open failed for segment (hr=" + hr_hex(hr) + ")");
            continue;
        }

        // Map source stream index -> kind, and switch to compressed passthrough.
        int src_video = -1;
        int src_audio = -1;
        for (DWORD i = 0; i < 2; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaType> native;
            hr = reader->GetNativeMediaType(i, 0, &native);
            if (FAILED(hr)) break;
            GUID major{};
            native->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major == MFMediaType_Video && src_video < 0) {
                src_video = static_cast<int>(i);
                if (!added_video) {
                    if (SUCCEEDED(writer->AddStream(native.Get(), &vstream))) {
                        writer->SetInputMediaType(vstream, native.Get(), nullptr);
                        added_video = true;
                    }
                }
            } else if (major == MFMediaType_Audio && src_audio < 0) {
                src_audio = static_cast<int>(i);
                if (!added_audio) {
                    if (SUCCEEDED(writer->AddStream(native.Get(), &astream))) {
                        writer->SetInputMediaType(astream, native.Get(), nullptr);
                        added_audio = true;
                    }
                }
            }
            // Request compressed samples from the source reader for this stream.
            reader->SetCurrentMediaType(i, nullptr, native.Get());
        }

        if (!began_writing) {
            hr = writer->BeginWriting();
            if (FAILED(hr)) {
                CL_ERROR("Remux", "BeginWriting failed (hr=" + hr_hex(hr) + ")");
                break;
            }
            began_writing = true;
        }

        const bool is_first = (&input == &inputs.front());
        const bool is_last = (&input == &inputs.back());
        const LONGLONG skip_hns = is_first ? head_trim_hns : 0;

        // Probe media duration so the tail trim can cut relative to the actual
        // timeline (wall-clock segment length may differ by stall/drift).
        LONGLONG file_dur_hns = 0;
        {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(reader->GetPresentationAttribute(
                    static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                    &var)) &&
                var.vt == VT_UI8) {
                file_dur_hns = static_cast<LONGLONG>(var.uhVal.QuadPart);
            }
            PropVariantClear(&var);
        }
        const LONGLONG tail_cut_hns =
            (is_last && tail_trim_hns > 0 && file_dur_hns > tail_trim_hns)
                ? (file_dur_hns - tail_trim_hns)
                : INT64_MAX;

        LONGLONG file_end = time_offset;
        bool wrote_any = false;
        while (true) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
                                    &stream_index, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (!sample) continue;
            if (flags & MF_SOURCE_READERF_STREAMTICK) continue;

            DWORD sink = 0;
            if (static_cast<int>(stream_index) == src_video) {
                sink = vstream;
            } else if (static_cast<int>(stream_index) == src_audio) {
                sink = astream;
            } else {
                continue;
            }

            // Head trim: drop samples entirely before the requested window.
            if (timestamp < skip_hns) continue;
            // Tail trim: stop once past the cutoff (timestamps are per-file).
            if (timestamp > tail_cut_hns) {
                // Don't break outright: the other track may still have samples
                // before the cutoff interleaved later. Skip this sample but
                // keep reading; the loop ends at EOS. To avoid unbounded reads
                // on pathological files, rely on EOS (MP4 readers always send
                // it) rather than breaking early.
                continue;
            }

            LONGLONG dur = 0;
            sample->GetSampleDuration(&dur);
            // Rebase so the first kept sample of the first file lands at 0:
            // subsequent files chain via time_offset.
            const LONGLONG t = timestamp - skip_hns + time_offset;
            sample->SetSampleTime(t);
            writer->WriteSample(sink, sample.Get());
            wrote_any = true;
            const LONGLONG sample_end = t + (dur > 0 ? dur : 0);
            if (sample_end > file_end) file_end = sample_end;
        }

        // Only advance the timeline if this file contributed samples. Empty or
        // fully-trimmed files previously added a +1 (100ns) drift per file.
        if (wrote_any) {
            time_offset = file_end;
        }
    }

    hr = writer->Finalize();
    writer.Reset();
    if (mf_started) MFShutdown();

    if (FAILED(hr)) {
        CL_ERROR("Remux", "Finalize failed (hr=" + hr_hex(hr) + ")");
        return false;
    }
    return true;
}

}  // namespace cliplite::replay
