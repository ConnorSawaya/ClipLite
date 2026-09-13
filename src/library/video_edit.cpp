#include "cliplite/library/video_edit.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "cliplite/encoder/media_foundation.h"
#include "cliplite/graphics/frame_converter.h"
#include "cliplite/log.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::library {

namespace {

std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

inline int even_down(int v) { return v & ~1; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int64_t clampi64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Box blur on the 8-bit Y plane (separable, region-limited).
void box_blur_y(uint8_t* plane, int stride, int rx, int ry, int rw, int rh, int radius) {
    if (rw <= 0 || rh <= 0 || radius <= 0) return;
    std::vector<uint8_t> tmp(static_cast<size_t>(rw) * rh);
    // Horizontal
    for (int row = 0; row < rh; ++row) {
        const uint8_t* src = plane + static_cast<size_t>(ry + row) * stride + rx;
        uint8_t* dst = tmp.data() + static_cast<size_t>(row) * rw;
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) sum += src[clampi(i, 0, rw - 1)];
        for (int x = 0; x < rw; ++x) {
            dst[x] = static_cast<uint8_t>(sum / (radius * 2 + 1));
            const int out = clampi(x - radius, 0, rw - 1);
            const int in = clampi(x + radius + 1, 0, rw - 1);
            sum += src[in] - src[out];
        }
    }
    // Vertical
    for (int x = 0; x < rw; ++x) {
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) sum += tmp[clampi(i, 0, rh - 1) * rw + x];
        for (int y = 0; y < rh; ++y) {
            plane[static_cast<size_t>(ry + y) * stride + rx + x] =
                static_cast<uint8_t>(sum / (radius * 2 + 1));
            const int out = clampi(y - radius, 0, rh - 1);
            const int in = clampi(y + radius + 1, 0, rh - 1);
            sum += tmp[in * rw + x] - tmp[out * rw + x];
        }
    }
}

// Box blur on interleaved UV. Columns are byte offsets into the interleaved
// plane; rows are chroma-row offsets. The whole region is deinterleated once,
// each plane blurred as a full image, then interleaved back.
void box_blur_uv(uint8_t* plane, int stride, int cw_bytes, int ch_rows, const EditBlur& b,
                 int radius) {
    int bx = even_down(clampi(static_cast<int>(b.x * cw_bytes), 0, cw_bytes - 2));
    int by = clampi(static_cast<int>(b.y * ch_rows), 0, ch_rows - 1);
    int bw = std::min(even_down(clampi(static_cast<int>(b.w * cw_bytes), 2, cw_bytes)),
                      cw_bytes - bx);
    int bh = std::min(clampi(static_cast<int>(b.h * ch_rows), 1, ch_rows), ch_rows - by);
    if (bw < 2 || bh <= 0) return;
    const int w2 = bw / 2;

    std::vector<uint8_t> u(static_cast<size_t>(w2) * bh);
    std::vector<uint8_t> v(static_cast<size_t>(w2) * bh);
    for (int row = 0; row < bh; ++row) {
        const uint8_t* src = plane + static_cast<size_t>(by + row) * stride + bx;
        uint8_t* ud = u.data() + static_cast<size_t>(row) * w2;
        uint8_t* vd = v.data() + static_cast<size_t>(row) * w2;
        for (int x = 0; x < w2; ++x) {
            ud[x] = src[2 * x];
            vd[x] = src[2 * x + 1];
        }
    }
    const int r2 = std::max(1, radius / 2);
    box_blur_y(u.data(), w2, 0, 0, w2, bh, r2);
    box_blur_y(v.data(), w2, 0, 0, w2, bh, r2);
    for (int row = 0; row < bh; ++row) {
        uint8_t* back = plane + static_cast<size_t>(by + row) * stride + bx;
        const uint8_t* ud = u.data() + static_cast<size_t>(row) * w2;
        const uint8_t* vd = v.data() + static_cast<size_t>(row) * w2;
        for (int x = 0; x < w2; ++x) {
            back[2 * x] = ud[x];
            back[2 * x + 1] = vd[x];
        }
    }
}

}  // namespace

bool edit_video(const std::wstring& input, const std::wstring& output, const EditOptions& opts,
                const std::function<bool(int)>& progress) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_here = SUCCEEDED(co);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    bool ok = false;

    const HRESULT mf = MFStartup(MF_VERSION);
    HRESULT hr = MFCreateSourceReaderFromURL(input.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        CL_ERROR("Edit", "open input failed (hr=" + hr_hex(hr) + ")");
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    }

    UINT32 sw = 0;
    UINT32 sh = 0;
    UINT32 fps_num = 30;
    UINT32 fps_den = 1;
    Microsoft::WRL::ComPtr<IMFMediaType> native;
    reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
    if (native) {
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &sw, &sh);
        MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);
    }
    if (fps_den == 0 || fps_num == 0) {
        fps_num = 30;
        fps_den = 1;
    }
    // Audio-only/corrupt files have no frame size: fail here, not later as a
    // huge allocation or negative crop coordinates.
    if (sw == 0 || sh == 0) {
        CL_ERROR("Edit", "source has no video stream");
        reader.Reset();
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    }

    // Resolve time range.
    int64_t dur_ms = 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8) {
        dur_ms = static_cast<int64_t>(var.uhVal.QuadPart / 10000);
    }
    PropVariantClear(&var);
    const int64_t dur_hi = dur_ms > 0 ? dur_ms : 0;
    int64_t start_ms = clampi64(opts.start_ms, 0, dur_hi);
    int64_t end_ms = opts.end_ms > 0 ? std::min<int64_t>(opts.end_ms, dur_ms) : dur_ms;
    // Fail loudly on empty/inverted ranges like export_trimmed does instead of
    // silently exporting the whole file (which produced wildly wrong lengths).
    if (end_ms <= start_ms) {
        CL_ERROR("Edit", "invalid range: start=" + std::to_string(opts.start_ms) +
                              " end=" + std::to_string(opts.end_ms) +
                              " dur=" + std::to_string(dur_ms));
        reader.Reset();
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    }
    const int64_t start_hns = start_ms * 10'000;
    const int64_t end_hns = end_ms * 10'000;
    const int64_t span_hns = std::max<int64_t>(1, end_hns - start_hns);

    // Resolve crop.
    int cx = even_down(clampi(opts.crop_x, 0, static_cast<int>(sw) - 2));
    int cy = even_down(clampi(opts.crop_y, 0, static_cast<int>(sh) - 2));
    int cw = opts.crop_w > 0 ? even_down(std::min(opts.crop_w, static_cast<int>(sw) - cx))
                             : static_cast<int>(sw);
    int ch = opts.crop_h > 0 ? even_down(std::min(opts.crop_h, static_cast<int>(sh) - cy))
                             : static_cast<int>(sh);
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;

    // Switch the reader to decoded NV12 at source size.
    Microsoft::WRL::ComPtr<IMFMediaType> nv12;
    if (FAILED(MFCreateMediaType(&nv12))) {
        CL_ERROR("Edit", "NV12 type alloc failed");
        reader.Reset();
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    }
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, sw, sh);
    MFSetAttributeRatio(nv12.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
    if (FAILED(hr)) {
        CL_ERROR("Edit", "NV12 setup failed (hr=" + hr_hex(hr) + ")");
        reader.Reset();
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    }

    // Actual decoded stride (decoders may pad rows; never assume tight).
    INT32 stride_signed = 0;
    Microsoft::WRL::ComPtr<IMFMediaType> cur_type;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur_type);
    if (cur_type) cur_type->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride_signed));
    const size_t src_stride =
        stride_signed > 0 ? static_cast<size_t>(stride_signed) : static_cast<size_t>(sw);
    // Seek to start.
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = start_hns;
    reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);

    // Unified render: a caller-supplied stem mix replaces the compressed
    // audio passthrough (same AAC settings the recorder uses).
    const bool use_mix = opts.mix_pcm != nullptr && !opts.mix_pcm->empty();
    const size_t mix_frames_total =
        use_mix ? opts.mix_pcm->size() / 2 : 0;  // 48kHz stereo contract
    size_t mix_frame = 0;

    cliplite::encoder::EncoderConfig cfg;
    cfg.width = static_cast<uint32_t>(cw);
    cfg.height = static_cast<uint32_t>(ch);
    cfg.fps_num = fps_num;
    cfg.fps_den = fps_den;
    cfg.audio_enabled = use_mix;  // else audio arrives via passthrough
    const float px_ratio =
        (static_cast<float>(cw) * ch) / (static_cast<float>(sw) * sh + 1.0f);
    cfg.video_bitrate_bps = std::max(4'000'000u,
        static_cast<uint32_t>(14'000'000 * px_ratio));
    cliplite::encoder::MediaFoundationEncoder enc;
    cliplite::encoder::AudioPassthrough ap;
    bool enc_ok = false;
    if (use_mix) {
        enc_ok = enc.start(output, cfg);
    } else {
        ap.input = input;
        ap.start_hns = start_hns;
        enc_ok = enc.start(output, cfg, ap);
    }
    if (!enc_ok) {
        CL_ERROR("Edit", "encoder start failed");
        reader.Reset();
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        DeleteFileW(output.c_str());
        return false;
    }
    std::vector<uint8_t> scratch(static_cast<size_t>(cw) * ch * 3 / 2);
    const int blur_radius = std::max(6, std::min(cw, ch) / 40);

    int frames = 0;
    while (true) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts,
                                &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
        if (ts >= end_hns) break;
        if (ts < start_hns) continue;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) break;
        BYTE* data = nullptr;
        DWORD max_len = 0;
        DWORD cur_len = 0;
        HRESULT lock_hr = buf->Lock(&data, nullptr, &cur_len);
        if (FAILED(lock_hr) || !data) {
            if (SUCCEEDED(lock_hr)) buf->Unlock();
            break;
        }
        if (cur_len < static_cast<DWORD>(cw) * ch * 3 / 2) {
            buf->Unlock();
            break;
        }
        // Crop-copy into tight NV12.
        uint8_t* y_dst = scratch.data();
        uint8_t* uv_dst = scratch.data() + static_cast<size_t>(cw) * ch;
        for (int row = 0; row < ch; ++row) {
            std::memcpy(y_dst + static_cast<size_t>(row) * cw,
                        data + static_cast<size_t>(cy + row) * src_stride + cx, cw);
        }
        for (int row = 0; row < ch / 2; ++row) {
            std::memcpy(uv_dst + static_cast<size_t>(row) * cw,
                        data + static_cast<size_t>(src_stride) * sh +
                            static_cast<size_t>(cy / 2 + row) * src_stride + cx,
                        cw);
        }
        buf->Unlock();
        for (const EditBlur& b : opts.blurs) {
            const int bx = clampi(static_cast<int>(b.x * cw), 0, cw - 2) & ~1;
            const int by = clampi(static_cast<int>(b.y * ch), 0, ch - 2) & ~1;
            const int bw = std::min(even_down(clampi(static_cast<int>(b.w * cw), 4, cw)), cw - bx);
            const int bh = std::min(even_down(clampi(static_cast<int>(b.h * ch), 4, ch)), ch - by);
            if (bw <= 0 || bh <= 0) continue;
            box_blur_y(y_dst, cw, bx, by, bw, bh, blur_radius);
            box_blur_uv(uv_dst, cw, cw, ch / 2, b, blur_radius);
        }
        const int64_t out_ts = ts - start_hns;
        if (use_mix) {
            // Feed mixed-stem chunks due before this video timestamp.
            while (mix_frame < mix_frames_total) {
                const int64_t t =
                    static_cast<int64_t>(mix_frame) * 10'000'000LL / 48000;
                if (t > out_ts) break;
                const size_t take =
                    std::min<size_t>(4800, mix_frames_total - mix_frame);
                if (!enc.push_audio_frames(opts.mix_pcm->data() + mix_frame * 2,
                                           static_cast<uint32_t>(take), t)) {
                    break;
                }
                mix_frame += take;
            }
        } else {
            enc.pump_audio_until(out_ts);
        }
        if (!enc.push_video_frame(scratch.data(), cw, out_ts)) {
            CL_WARN("Edit", "frame push failed at " + std::to_string(frames));
        }
        ++frames;

        if ((frames % 30) == 0 && progress) {
            if (!progress(static_cast<int>(std::min(99LL, (ts - start_hns) * 100 / span_hns)))) {
                CL_INFO("Edit", "cancelled by caller");
                enc.abort();
                reader.Reset();
                if (SUCCEEDED(mf)) MFShutdown();
                if (com_here) CoUninitialize();
                DeleteFileW(output.c_str());
                return false;
            }
        }
    }
    if (use_mix) {
        // Drain any mixed tail past the last video timestamp.
        while (mix_frame < mix_frames_total) {
            const int64_t t = static_cast<int64_t>(mix_frame) * 10'000'000LL / 48000;
            const size_t take = std::min<size_t>(4800, mix_frames_total - mix_frame);
            if (!enc.push_audio_frames(opts.mix_pcm->data() + mix_frame * 2,
                                       static_cast<uint32_t>(take), t)) {
                break;
            }
            mix_frame += take;
        }
    }
    ok = enc.finish();
    reader.Reset();
    if (SUCCEEDED(mf)) MFShutdown();
    if (com_here) CoUninitialize();

    if (!ok) DeleteFileW(output.c_str());
    else CL_INFO("Edit", "rendered " + std::to_string(frames) + " frames -> " +
                             cliplite::util::wide_to_utf8(output));
    if (progress) progress(ok ? 100 : 0);
    return ok;
}

bool edit_project_video(const std::vector<ProjectSegment>& segments, const std::wstring& output,
                        const EditOptions& opts,
                        const std::function<bool(int)>& progress) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_here = SUCCEEDED(co);
    const HRESULT mf = MFStartup(MF_VERSION);
    bool ok = false;
    cliplite::encoder::MediaFoundationEncoder enc;
    auto fail = [&](const std::string& what) {
        CL_ERROR("EditProject", what);
        // Abort first: without it the encoder destructor would Finalize()
        // and resurrect the just-deleted partial file.
        enc.abort();
        DeleteFileW(output.c_str());
        if (SUCCEEDED(mf)) MFShutdown();
        if (com_here) CoUninitialize();
        return false;
    };

    if (segments.empty()) return fail("no segments");
    for (const auto& s : segments) {
        if (s.input.empty() || s.end_ms <= s.start_ms) return fail("empty/inverted segment");
        if (!(s.speed >= 0.25 && s.speed <= 4.0)) return fail("segment speed out of range");
    }

    // Probe every input (dimensions/fps come from segment 0's file;
    // durations are per file so imported clips clamp to their own length).
    struct SegMedia {
        UINT32 sw = 0, sh = 0;
        int64_t dur_ms = 0;
        int64_t end_ms = 0;  // clamped segment end
    };
    UINT32 fps_num = 30, fps_den = 1;
    std::vector<SegMedia> media;
    media.reserve(segments.size());
    for (size_t si = 0; si < segments.size(); ++si) {
        SegMedia m;
        Microsoft::WRL::ComPtr<IMFSourceReader> probe;
        HRESULT hr = MFCreateSourceReaderFromURL(segments[si].input.c_str(), nullptr, &probe);
        if (FAILED(hr)) return fail("open input failed");
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        probe->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
        if (native) {
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &m.sw, &m.sh);
            if (si == 0)
                MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);
            // Phone clips stored landscape + rotation flag decode sideways;
            // swap so all downstream math sees display orientation.
            UINT32 rot = 0;
            if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_ROTATION, &rot)) &&
                (rot == 90 || rot == 270)) {
                const UINT32 t = m.sw;
                m.sw = m.sh;
                m.sh = t;
            }
        }
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(probe->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8) {
            m.dur_ms = static_cast<int64_t>(var.uhVal.QuadPart / 10000);
        }
        PropVariantClear(&var);
        probe.Reset();
        if (m.sw == 0 || m.sh == 0) return fail("segment source has no video");
        const int64_t e = segments[si].end_ms > 0
                              ? std::min<int64_t>(segments[si].end_ms, m.dur_ms)
                              : m.dur_ms;
        if (e <= segments[si].start_ms || segments[si].start_ms < 0)
            return fail("segment outside source");
        m.end_ms = e;
        media.push_back(m);
    }
    if (fps_den == 0 || fps_num == 0) {
        fps_num = 30;
        fps_den = 1;
    }
    const UINT32 sw = media[0].sw, sh = media[0].sh;
    // Output durations are speed-adjusted: twice as fast = half the time.
    int64_t total_ms = 0;
    for (size_t si = 0; si < segments.size(); ++si) {
        total_ms += static_cast<int64_t>(std::llround(
            static_cast<double>(media[si].end_ms - segments[si].start_ms) /
            segments[si].speed));
    }
    if (total_ms <= 0) return fail("project has no duration");
    const int64_t total_hns = total_ms * 10'000;
    // Video fade windows on the output timeline (clamped to the duration).
    const int64_t fade_in_hns =
        std::min<int64_t>(std::max<int64_t>(0, opts.fade_in_ms) * 10'000, total_hns);
    const int64_t fade_out_hns =
        std::min<int64_t>(std::max<int64_t>(0, opts.fade_out_ms) * 10'000, total_hns);

    // Resolve crop (global for the project, like edit_video).
    int cx = even_down(clampi(opts.crop_x, 0, static_cast<int>(sw) - 2));
    int cy = even_down(clampi(opts.crop_y, 0, static_cast<int>(sh) - 2));
    int cw = opts.crop_w > 0 ? even_down(std::min(opts.crop_w, static_cast<int>(sw) - cx))
                             : static_cast<int>(sw);
    int ch = opts.crop_h > 0 ? even_down(std::min(opts.crop_h, static_cast<int>(sh) - cy))
                             : static_cast<int>(sh);
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;

    // Aspect canvas fit: letterbox the whole source window into the aspect
    // box (black bars) instead of center-cropping it away (fill). Box height
    // follows the window; absurd upscales (>4x) fall back to fill.
    int boxw = 0, boxh = 0;
    if (opts.canvas_ar_w > 0 && opts.canvas_ar_h > 0) {
        const int bh = ch & ~1;
        const int bw =
            even_down(static_cast<int>(static_cast<int64_t>(bh) * opts.canvas_ar_w /
                                      opts.canvas_ar_h));
        if (bw >= 16 && bh >= 16 && bw <= cw * 4 && bh <= ch * 4) {
            boxw = bw;
            boxh = bh;
        } else {
            CL_WARN("EditProject", "canvas fit out of range, using fill");
        }
    }
    const bool use_fit = boxw > 0;

    const bool use_mix = opts.mix_pcm != nullptr && !opts.mix_pcm->empty();
    const size_t mix_frames_total = use_mix ? opts.mix_pcm->size() / 2 : 0;
    size_t mix_frame = 0;

    // Fit canvas: working frame becomes the aspect box (blur fractions and
    // downstream stages all operate in canvas space from here on).
    int frame_w = cw, frame_h = ch;
    std::vector<uint8_t> canvas_buf, fit_tmp;
    if (use_fit) {
        const double s = std::min(static_cast<double>(boxw) / cw,
                                  static_cast<double>(boxh) / ch);
        const int fit_dw = even_down(std::max(2, static_cast<int>(cw * s)));
        const int fit_dh = even_down(std::max(2, static_cast<int>(ch * s)));
        canvas_buf.resize(static_cast<size_t>(boxw) * boxh * 3 / 2);
        fit_tmp.resize(static_cast<size_t>(fit_dw) * fit_dh * 3 / 2);
        frame_w = boxw;
        frame_h = boxh;
    }

    // Export targets: downscale output, output frame rate, quality tier.
    // ow/oh default to the working frame size; a target height below it
    // scales down preserving aspect (even dims throughout for NV12/H.264).
    int ow = frame_w, oh = frame_h;
    if (opts.out_height > 0 && frame_h > opts.out_height) {
        oh = opts.out_height & ~1;
        ow = even_down(static_cast<int>(static_cast<int64_t>(frame_w) * oh / frame_h));
        if (oh < 16 || ow < 16) {
            ow = frame_w;
            oh = frame_h;
        }
    }
    uint32_t out_fps_num = fps_num, out_fps_den = fps_den;
    if (opts.out_fps == 30 || opts.out_fps == 60) {
        out_fps_num = static_cast<uint32_t>(opts.out_fps);
        out_fps_den = 1;
    }
    constexpr float kQualityMult[3] = {0.5f, 1.0f, 1.6f};
    const float qmult = kQualityMult[opts.quality < 0 || opts.quality > 2 ? 1 : opts.quality];

    // Prerender text overlays once (output-size dependent). Bad texts are
    // skipped with a warning; they never fail the render.
    struct LiveText {
        TextBitmap bmp;
        int x0 = 0, y0 = 0;
        int64_t start_hns = 0, end_hns = 0;
    };
    std::vector<LiveText> live_texts;
    for (const auto& t : opts.texts) {
        if (t.text.empty() || t.end_ms <= t.start_ms) continue;
        const int cap_px = static_cast<int>(t.size_frac * oh);
        if (cap_px < 8) continue;
        TextBitmap bmp;
        if (!prerender_text_bitmap(t, cap_px, bmp) || bmp.w <= 0) continue;
        int x0 = 0, y0 = 0;
        text_top_left(t.x, t.y, t.align, ow, oh, bmp.w, bmp.h, x0, y0);
        LiveText lt;
        lt.bmp = std::move(bmp);
        lt.x0 = x0;
        lt.y0 = y0;
        lt.start_hns = t.start_ms * 10'000;
        lt.end_hns = t.end_ms * 10'000;
        live_texts.push_back(std::move(lt));
    }

    cliplite::encoder::EncoderConfig cfg;
    cfg.width = static_cast<uint32_t>(ow);
    cfg.height = static_cast<uint32_t>(oh);
    cfg.fps_num = out_fps_num;
    cfg.fps_den = out_fps_den;
    cfg.audio_enabled = use_mix;
    const float px_ratio =
        (static_cast<float>(ow) * oh) / (static_cast<float>(sw) * sh + 1.0f);
    // Bitrate scales with pixels AND frame rate (area-only starved 60fps).
    const float fps_factor =
        std::min(2.0f, std::max(1.0f, static_cast<float>(out_fps_num) /
                                           static_cast<float>(out_fps_den) / 30.f));
    cfg.video_bitrate_bps = static_cast<uint32_t>(
        std::max(4'000'000u, static_cast<uint32_t>(14'000'000 * px_ratio)) * qmult *
        fps_factor);
    bool enc_ok = false;
    if (use_mix) {
        enc_ok = enc.start(output, cfg);
    } else if (segments.size() == 1 && !opts.force_video_only) {
        cliplite::encoder::AudioPassthrough ap;
        ap.input = segments[0].input;
        ap.start_hns = segments[0].start_ms * 10'000;
        enc_ok = enc.start(output, cfg, ap);
    } else {
        // No stems (old clips) + several segments: video-only rather than a
        // silent wrong-length file; the caller toasts honestly.
        enc_ok = enc.start(output, cfg);
        CL_WARN("EditProject", "no mix for multi-segment project, video-only");
    }
    if (!enc_ok) return fail("encoder start failed");

    std::vector<uint8_t> scratch(static_cast<size_t>(cw) * ch * 3 / 2);
    const size_t out_size = static_cast<size_t>(ow) * oh * 3 / 2;
    // Scale workspace (only when downscaling).
    std::vector<uint8_t> scaled_buf;
    if (ow != frame_w || oh != frame_h) scaled_buf.resize(out_size);
    // Post workspace shared by text blend + fade: both must copy first,
    // because slow speeds re-emit frames and in-place edits would compound.
    std::vector<uint8_t> fx_buf;
    if (!live_texts.empty() || fade_in_hns > 0 || fade_out_hns > 0) fx_buf.resize(out_size);
    int64_t out_base_hns = 0;
    int frames = 0;

    // Output pacing follows the OUTPUT rate (source fps unless overridden).
    const int64_t frame_interval_hns = 10'000'000LL *
                                       static_cast<int64_t>(out_fps_den) /
                                       static_cast<int64_t>(out_fps_num);

    for (size_t si = 0; si < segments.size(); ++si) {
        const auto& seg = segments[si];
        const SegMedia& sm = media[si];
        const int swi = static_cast<int>(sm.sw), shi = static_cast<int>(sm.sh);
        const int64_t start_hns = seg.start_ms * 10'000;
        const int64_t end_hns = sm.end_ms * 10'000;
        const int64_t seg_out_hns = static_cast<int64_t>(std::llround(
                                        static_cast<double>(end_hns - start_hns) / seg.speed));
        // Output-paced emission: faster speeds drop source frames, slower
        // speeds duplicate (nearest-neighbor). last_emit starts one interval
        // back so the first source frame always emits at out_base.
        int64_t last_emit_hns = out_base_hns - frame_interval_hns;

        Microsoft::WRL::ComPtr<IMFMediaType> nv12;
        if (FAILED(MFCreateMediaType(&nv12))) return fail("NV12 type alloc failed");
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, sm.sw, sm.sh);
        MFSetAttributeRatio(nv12.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        HRESULT hr = MFCreateSourceReaderFromURL(seg.input.c_str(), nullptr, &reader);
        if (FAILED(hr)) return fail("open segment input failed");
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                         nv12.Get());
        if (FAILED(hr)) return fail("NV12 setup failed");
        INT32 stride_signed = 0;
        Microsoft::WRL::ComPtr<IMFMediaType> cur_type;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur_type);
        if (cur_type)
            cur_type->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                reinterpret_cast<UINT32*>(&stride_signed));
        const size_t src_stride =
            stride_signed > 0 ? static_cast<size_t>(stride_signed) : static_cast<size_t>(swi);
        // Clamp the project crop window into THIS file (imported clips may
        // differ in resolution); the export scaler normalizes to ow/oh after.
        const int scx = even_down(clampi(cx, 0, swi - 2));
        const int scy = even_down(clampi(cy, 0, shi - 2));
        const int scw = even_down(std::min(cw, swi - scx));
        const int sch = even_down(std::min(ch, shi - scy));
        if (scw < 16 || sch < 16) return fail("segment source smaller than crop");
        // Per-segment fit box + buffer growth (later segments may be larger).
        // A degenerate box falls back to fill for that segment, never fails.
        int seg_boxw = 0, seg_boxh = 0, seg_fdw = 0, seg_fdh = 0, seg_fox = 0, seg_foy = 0;
        const bool seg_fit =
            use_fit &&
            (sch & ~1) >= 16 &&
            even_down(static_cast<int>(static_cast<int64_t>(sch & ~1) * opts.canvas_ar_w /
                                       opts.canvas_ar_h)) >= 16;
        if (seg_fit) {
            const int bh = sch & ~1;
            const int bw = even_down(
                static_cast<int>(static_cast<int64_t>(bh) * opts.canvas_ar_w /
                                 opts.canvas_ar_h));
            seg_boxw = bw;
            seg_boxh = bh;
            const double s = std::min(static_cast<double>(bw) / scw,
                                      static_cast<double>(bh) / sch);
            seg_fdw = even_down(std::max(2, static_cast<int>(scw * s)));
            seg_fdh = even_down(std::max(2, static_cast<int>(sch * s)));
            seg_fox = ((bw - seg_fdw) / 2) & ~1;
            seg_foy = ((bh - seg_fdh) / 2) & ~1;
            if (canvas_buf.size() < static_cast<size_t>(bw) * bh * 3 / 2)
                canvas_buf.resize(static_cast<size_t>(bw) * bh * 3 / 2);
            if (fit_tmp.size() < static_cast<size_t>(seg_fdw) * seg_fdh * 3 / 2)
                fit_tmp.resize(static_cast<size_t>(seg_fdw) * seg_fdh * 3 / 2);
        }
        if (scratch.size() < static_cast<size_t>(scw) * sch * 3 / 2)
            scratch.resize(static_cast<size_t>(scw) * sch * 3 / 2);
        const int seg_blur_radius =
            std::max(6, std::min(seg_fit ? seg_boxw : scw, seg_fit ? seg_boxh : sch) / 40);
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = start_hns;
        reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);

        while (true) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                    &ts, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
            if (ts >= end_hns) break;
            if (ts < start_hns) continue;

            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) break;
            BYTE* data = nullptr;
            DWORD max_len = 0;
            DWORD cur_len = 0;
            HRESULT lock_hr = buf->Lock(&data, nullptr, &cur_len);
            if (FAILED(lock_hr) || !data) {
                if (SUCCEEDED(lock_hr)) buf->Unlock();
                break;
            }
            // Fail closed on truncated samples: the crop copy below reads a
            // full scw*sch frame plus chroma.
            if (cur_len < static_cast<DWORD>(scw) * sch * 3 / 2) {
                buf->Unlock();
                break;
            }
            uint8_t* y_dst = scratch.data();
            uint8_t* uv_dst = scratch.data() + static_cast<size_t>(scw) * sch;
            for (int row = 0; row < sch; ++row) {
                std::memcpy(y_dst + static_cast<size_t>(row) * scw,
                            data + static_cast<size_t>(scy + row) * src_stride + scx, scw);
            }
            for (int row = 0; row < sch / 2; ++row) {
                std::memcpy(uv_dst + static_cast<size_t>(row) * scw,
                            data + static_cast<size_t>(src_stride) * shi +
                                static_cast<size_t>(scy / 2 + row) * src_stride + scx,
                            scw);
            }
            buf->Unlock();
            // Fit canvas: scale the crop window into the aspect box with
            // black bars. Downstream (blur/scale/text/fade/push) all work in
            // canvas space from here on.
            uint8_t* work_y = y_dst;
            uint8_t* work_uv = uv_dst;
            int work_w = scw, work_h = sch;
            if (seg_fit) {
                cliplite::graphics::nv12_scale(scratch.data(), static_cast<uint32_t>(scw),
                                               static_cast<uint32_t>(sch), fit_tmp.data(),
                                               static_cast<uint32_t>(seg_fdw),
                                               static_cast<uint32_t>(seg_fdh));
                uint8_t* cy_p = canvas_buf.data();
                uint8_t* cuv_p =
                    canvas_buf.data() + static_cast<size_t>(seg_boxw) * seg_boxh;
                std::memset(cy_p, 16, static_cast<size_t>(seg_boxw) * seg_boxh);
                std::memset(cuv_p, 128, static_cast<size_t>(seg_boxw) * seg_boxh / 2);
                const uint8_t* fy_p = fit_tmp.data();
                const uint8_t* fuv_p =
                    fit_tmp.data() + static_cast<size_t>(seg_fdw) * seg_fdh;
                for (int row = 0; row < seg_fdh; ++row) {
                    std::memcpy(
                        cy_p + static_cast<size_t>(seg_foy + row) * seg_boxw + seg_fox,
                        fy_p + static_cast<size_t>(row) * seg_fdw, seg_fdw);
                }
                for (int row = 0; row < seg_fdh / 2; ++row) {
                    std::memcpy(cuv_p +
                                    static_cast<size_t>(seg_foy / 2 + row) * seg_boxw +
                                    seg_fox,
                                fuv_p + static_cast<size_t>(row) * seg_fdw, seg_fdw);
                }
                work_y = cy_p;
                work_uv = cuv_p;
                work_w = seg_boxw;
                work_h = seg_boxh;
            }
            for (const EditBlur& b : opts.blurs) {
                const int bx = clampi(static_cast<int>(b.x * work_w), 0, work_w - 2) & ~1;
                const int by = clampi(static_cast<int>(b.y * work_h), 0, work_h - 2) & ~1;
                const int bw = std::min(even_down(clampi(static_cast<int>(b.w * work_w), 4,
                                                         work_w)),
                                        work_w - bx);
                const int bh = std::min(even_down(clampi(static_cast<int>(b.h * work_h), 4,
                                                         work_h)),
                                        work_h - by);
                if (bw <= 0 || bh <= 0) continue;
                box_blur_y(work_y, work_w, bx, by, bw, bh, seg_blur_radius);
                box_blur_uv(work_uv, work_w, work_w, work_h / 2, b, seg_blur_radius);
            }
            // Output-paced emission: speed > 1 drops source frames, speed < 1
            // duplicates (nearest-neighbor). Audio mix already covers the
            // output timeline, so both stay synchronized by construction.
            const int64_t want_ts =
                out_base_hns + static_cast<int64_t>((ts - start_hns) / seg.speed);
            // Cap duplicates per source frame: a pathological timestamp jump
            // (e.g. hours on VFR footage) must not look like a hang or bloat
            // the output. 240 covers 0.25x slow-mo several times over.
            int dup_budget = 240;
            while (last_emit_hns + frame_interval_hns <= want_ts && dup_budget-- > 0) {
                last_emit_hns += frame_interval_hns;
                // Export scale (working frame -> output size). Decided per
                // emission, not per project: imported segments may differ in
                // resolution from segment 0, and pushing a smaller frame at
                // the output stride would read past its buffer.
                const uint8_t* frame_out = work_y;
                uint32_t frame_stride = static_cast<uint32_t>(work_w);
                if (work_w != ow || work_h != oh) {
                    if (scaled_buf.size() < out_size) scaled_buf.resize(out_size);
                    cliplite::graphics::nv12_scale(work_y,
                                                   static_cast<uint32_t>(work_w),
                                                   static_cast<uint32_t>(work_h),
                                                   scaled_buf.data(),
                                                   static_cast<uint32_t>(ow),
                                                   static_cast<uint32_t>(oh));
                    frame_out = scaled_buf.data();
                    frame_stride = static_cast<uint32_t>(ow);
                }
                // Text overlays live on the output timeline, under fades.
                bool fx_live = false;
                auto fx_acquire = [&]() -> uint8_t* {
                    if (!fx_live && !fx_buf.empty()) {
                        fx_buf.assign(frame_out, frame_out + out_size);
                        fx_live = true;
                        return fx_buf.data();
                    }
                    return fx_live ? fx_buf.data() : nullptr;
                };
                for (const auto& lt : live_texts) {
                    if (last_emit_hns < lt.start_hns || last_emit_hns >= lt.end_hns) continue;
                    if (uint8_t* fx = fx_acquire()) {
                        blend_text_nv12(fx, fx + static_cast<size_t>(ow) * oh, ow, oh,
                                        lt.bmp, lt.x0, lt.y0);
                    }
                }
                if (fx_live) frame_out = fx_buf.data();
                // Fade to/from black on the output timeline (limited-range
                // NV12: Y black = 16, UV neutral = 128). Same copy rule.
                if ((fade_in_hns > 0 && last_emit_hns < fade_in_hns) ||
                    (fade_out_hns > 0 && last_emit_hns > total_hns - fade_out_hns)) {
                    float vf = 1.f;
                    if (fade_in_hns > 0 && last_emit_hns < fade_in_hns)
                        vf = static_cast<float>(last_emit_hns) /
                             static_cast<float>(fade_in_hns);
                    if (fade_out_hns > 0 && last_emit_hns > total_hns - fade_out_hns) {
                        const float vo =
                            static_cast<float>(total_hns - last_emit_hns) /
                            static_cast<float>(fade_out_hns);
                        if (vo < vf) vf = vo;
                    }
                    if (vf < 0.f) vf = 0.f;
                    if (vf < 0.999f) {
                        if (uint8_t* fx = fx_acquire()) {
                            const size_t on = static_cast<size_t>(ow) * oh;
                            uint8_t* y = fx;
                            uint8_t* uv = fx + on;
                            for (size_t i = 0; i < on; ++i)
                                y[i] = static_cast<uint8_t>(16 + (y[i] - 16) * vf);
                            const size_t uvn = on / 2;
                            for (size_t i = 0; i < uvn; ++i)
                                uv[i] = static_cast<uint8_t>(128 + (uv[i] - 128) * vf);
                        }
                        if (fx_live) frame_out = fx_buf.data();
                    }
                }
                if (use_mix) {
                    while (mix_frame < mix_frames_total) {
                        const int64_t t =
                            static_cast<int64_t>(mix_frame) * 10'000'000LL / 48000;
                        if (t > last_emit_hns) break;
                        const size_t take =
                            std::min<size_t>(4800, mix_frames_total - mix_frame);
                        if (!enc.push_audio_frames(opts.mix_pcm->data() + mix_frame * 2,
                                                   static_cast<uint32_t>(take), t)) {
                            break;
                        }
                        mix_frame += take;
                    }
                } else if (segments.size() == 1) {
                    enc.pump_audio_until(last_emit_hns);
                }
                if (!enc.push_video_frame(frame_out, frame_stride, last_emit_hns)) {
                    CL_WARN("EditProject", "frame push failed at " + std::to_string(frames));
                }
                ++frames;

                if ((frames % 30) == 0 && progress) {
                    if (!progress(static_cast<int>(std::min<int64_t>(
                            99, last_emit_hns * 100 / std::max<int64_t>(1, total_hns))))) {
                        CL_INFO("EditProject", "cancelled by caller");
                        enc.abort();
                        reader.Reset();
                        if (SUCCEEDED(mf)) MFShutdown();
                        if (com_here) CoUninitialize();
                        DeleteFileW(output.c_str());
                        return false;
                    }
                }
            }
        }
        out_base_hns += seg_out_hns;
        reader.Reset();
    }
    if (use_mix) {
        while (mix_frame < mix_frames_total) {
            const int64_t t = static_cast<int64_t>(mix_frame) * 10'000'000LL / 48000;
            const size_t take = std::min<size_t>(4800, mix_frames_total - mix_frame);
            if (!enc.push_audio_frames(opts.mix_pcm->data() + mix_frame * 2,
                                       static_cast<uint32_t>(take), t)) {
                break;
            }
            mix_frame += take;
        }
    }
    ok = enc.finish();
    if (SUCCEEDED(mf)) MFShutdown();
    if (com_here) CoUninitialize();

    if (!ok) DeleteFileW(output.c_str());
    else CL_INFO("EditProject", "rendered " + std::to_string(frames) + " frames, " +
                                    std::to_string(total_ms) + "ms -> " +
                                    cliplite::util::wide_to_utf8(output));
    if (progress) progress(ok ? 100 : 0);
    return ok;
}

}  // namespace cliplite::library
