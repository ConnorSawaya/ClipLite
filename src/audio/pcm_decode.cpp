#include "cliplite/audio/pcm_decode.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cliplite/audio/stem_track.h"
#include "cliplite/log.h"

namespace cliplite::audio {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}
}  // namespace

std::vector<float> decode_audio_range(const std::wstring& path, int64_t start_ms,
                                      int64_t end_ms) {
    std::vector<float> stereo48;
    if (path.empty() || end_ms <= start_ms || start_ms < 0) return stereo48;
    // A corrupt duration could otherwise reserve gigabytes below.
    if (end_ms - start_ms > 2LL * 60 * 60 * 1000) return stereo48;
    // May run on threads without COM (render workers own MF but not COM):
    // init only when this thread isn't already in an apartment.
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_here = SUCCEEDED(co) && co != RPC_E_CHANGED_MODE;

    const HRESULT startup = MFStartup(MF_VERSION);
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        if (SUCCEEDED(startup)) MFShutdown();
        if (com_here) CoUninitialize();
        return stereo48;
    }

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), FALSE);
    // Float preferred, PCM16 fallback (same box-dependent gap as elsewhere).
    uint32_t bytes_per_sample = 0;
    uint32_t src_rate = 0;
    uint32_t src_ch = 0;
    for (const GUID& sub : {MFAudioFormat_Float, MFAudioFormat_PCM}) {
        Microsoft::WRL::ComPtr<IMFMediaType> mt;
        if (FAILED(MFCreateMediaType(&mt))) break;
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mt->SetGUID(MF_MT_SUBTYPE, sub);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                  nullptr, mt.Get()))) {
            bytes_per_sample = (sub == MFAudioFormat_Float) ? 4 : 2;
            Microsoft::WRL::ComPtr<IMFMediaType> actual;
            if (SUCCEEDED(reader->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual))) {
                actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                  reinterpret_cast<UINT32*>(&src_rate));
                actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                  reinterpret_cast<UINT32*>(&src_ch));
            }
            break;
        }
    }
    if (bytes_per_sample == 0 || src_rate == 0 || src_ch == 0) {
        reader.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        if (com_here) CoUninitialize();
        return stereo48;  // no decodable audio stream
    }

    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = start_ms * 10'000;
    reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);

    const int64_t end_hns = end_ms * 10'000;
    std::vector<float> native;
    native.reserve(static_cast<size_t>(end_ms - start_ms) * src_rate / 1000 * src_ch);
    for (int guard = 0; guard < 1'000'000; ++guard) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample || ts >= end_hns) break;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len)) || !data) continue;
        const size_t n = len / bytes_per_sample;
        const size_t was = native.size();
        native.resize(was + n);
        if (bytes_per_sample == 4) {
            std::memcpy(native.data() + was, data, n * 4);
        } else {
            const int16_t* v = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < n; ++i)
                native[was + i] = static_cast<float>(v[i]) / 32768.f;
        }
        buf->Unlock();
    }
    reader.Reset();
    if (SUCCEEDED(startup)) MFShutdown();
    if (com_here) CoUninitialize();
    if (native.empty()) return stereo48;

    // Channels -> stereo (mono duplicates, stereo passes through,
    // anything else averages to dual-mono; all bounded by the input peak).
    const size_t in_frames = native.size() / src_ch;
    std::vector<float> stereo;
    stereo.reserve(in_frames * 2);
    for (size_t f = 0; f < in_frames; ++f) {
        float l = 0.f, r = 0.f;
        if (src_ch == 1) {
            l = r = native[f];
        } else if (src_ch == 2) {
            l = native[f * 2];
            r = native[f * 2 + 1];
        } else {
            float acc = 0.f;
            for (uint32_t c = 0; c < src_ch; ++c) acc += native[f * src_ch + c];
            l = r = acc / src_ch;
        }
        stereo.push_back(l);
        stereo.push_back(r);
    }
    if (com_here) CoUninitialize();
    if (src_rate == 48000) return stereo;
    return resample_pcm_linear(stereo, 2, static_cast<double>(src_rate) / 48000.0);
}

}  // namespace cliplite::audio
