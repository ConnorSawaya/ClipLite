#include "cliplite/encoder/media_foundation.h"

#include <mfidl.h>
#include <mferror.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "cliplite/log.h"

namespace cliplite::encoder {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

// Process-wide MF refcount. MFStartup/MFShutdown pairs are expensive and leak
// handles when cycled at segment-rotation frequency; pinning MF for the
// recorder's lifetime avoids both.
std::mutex g_mf_mutex;
int g_mf_refs = 0;
}  // namespace

bool mf_hold_reference() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (g_mf_refs == 0) {
        const HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            CL_ERROR("Encoder", "MFStartup failed (hr=" + hr_hex(hr) + ")");
            return false;
        }
    }
    ++g_mf_refs;
    return true;
}

void mf_drop_reference() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (g_mf_refs > 0 && --g_mf_refs == 0) MFShutdown();
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    if (running_) finish();
}

bool MediaFoundationEncoder::start(const std::wstring& path, const EncoderConfig& cfg) {
    if (running_) return false;
    cfg_ = cfg;

    if (start_internal(path, /*use_hardware=*/true, nullptr)) {
        CL_INFO("Encoder", "using hardware H.264 encoder (or OS-selected transform)");
        return true;
    }
    CL_WARN("Encoder", "hardware encoder path failed, falling back to software H.264");

    // Reset partial state so the software attempt starts clean.
    writer_.Reset();
    pt_reader_.Reset();
    pt_active_ = false;
    pt_src_audio_idx_ = -1;
    cfg_ = cfg;
    video_stream_ = 0;
    audio_stream_ = 0;
    if (mf_initialized_) {
        mf_drop_reference();
        mf_initialized_ = false;
    }

    if (start_internal(path, /*use_hardware=*/false, nullptr)) {
        CL_INFO("Encoder", "using software H.264 encoder");
        return true;
    }
    CL_ERROR("Encoder", "software encoder path also failed");
    return false;
}

bool MediaFoundationEncoder::start(const std::wstring& path, const EncoderConfig& cfg,
                                   const AudioPassthrough& audio) {
    if (running_) return false;
    cfg_ = cfg;

    if (start_internal(path, /*use_hardware=*/true, &audio)) {
        CL_INFO("Encoder", "using hardware H.264 encoder + audio passthrough");
        return true;
    }
    CL_WARN("Encoder", "hardware encoder path failed, falling back to software H.264");

    writer_.Reset();
    pt_reader_.Reset();
    pt_active_ = false;
    pt_src_audio_idx_ = -1;
    cfg_ = cfg;
    video_stream_ = 0;
    audio_stream_ = 0;
    if (mf_initialized_) {
        mf_drop_reference();
        mf_initialized_ = false;
    }

    if (start_internal(path, /*use_hardware=*/false, &audio)) {
        CL_INFO("Encoder", "using software H.264 encoder + audio passthrough");
        return true;
    }
    CL_ERROR("Encoder", "software encoder path also failed");
    return false;
}

bool MediaFoundationEncoder::start_internal(const std::wstring& path, bool use_hardware,
                                            const AudioPassthrough* audio) {
    if (!mf_hold_reference()) return false;
    mf_initialized_ = true;

    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) {
        if (use_hardware) attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        // Keep the default throttling: with MF_SINK_WRITER_DISABLE_THROTTLING
        // a slow encoder makes WriteSample queue samples without bound, which
        // balloons RAM during long recordings.
    }

    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer_);
    if (FAILED(hr)) {
        CL_ERROR("Encoder", "MFCreateSinkWriterFromURL failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // --- Video output (H.264) ---
    Microsoft::WRL::ComPtr<IMFMediaType> vout;
    hr = MFCreateMediaType(&vout);
    if (SUCCEEDED(hr)) {
        vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vout->SetUINT32(MF_MT_AVG_BITRATE, cfg_.video_bitrate_bps);
        vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vout.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(vout.Get(), MF_MT_FRAME_RATE, cfg_.fps_num, cfg_.fps_den);
        hr = writer_->AddStream(vout.Get(), &video_stream_);
    }
    if (FAILED(hr)) {
        CL_ERROR("Encoder", "video AddStream failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // --- Video input (NV12) ---
    Microsoft::WRL::ComPtr<IMFMediaType> vin;
    hr = MFCreateMediaType(&vin);
    if (SUCCEEDED(hr)) {
        vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(vin.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(vin.Get(), MF_MT_FRAME_RATE, cfg_.fps_num, cfg_.fps_den);
        MFSetAttributeRatio(vin.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        // Every frame is a keyframe: guarantees each short replay segment is
        // independently decodable and flushes reliably on Finalize (no B-frame
        // lookahead buffering). Trades bitrate for robustness; can be relaxed
        // later to periodic keyframes once flush handling is proven.
        vin->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        hr = writer_->SetInputMediaType(video_stream_, vin.Get(), nullptr);
    }
    if (FAILED(hr)) {
        CL_ERROR("Encoder", "video SetInputMediaType failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // --- Audio (AAC) ---
    if (cfg_.audio_enabled) {
        Microsoft::WRL::ComPtr<IMFMediaType> aout;
        hr = MFCreateMediaType(&aout);
        if (SUCCEEDED(hr)) {
            aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, cfg_.sample_rate);
            aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, cfg_.channels);
            aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, cfg_.audio_bitrate_bps / 8);
            hr = writer_->AddStream(aout.Get(), &audio_stream_);
        }
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IMFMediaType> ain;
            hr = MFCreateMediaType(&ain);
            if (SUCCEEDED(hr)) {
                ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
                ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, cfg_.sample_rate);
                ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, cfg_.channels);
                ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, cfg_.channels * 4);
                ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                               cfg_.sample_rate * cfg_.channels * 4);
                hr = writer_->SetInputMediaType(audio_stream_, ain.Get(), nullptr);
            }
        }
        if (FAILED(hr)) {
            CL_WARN("Encoder", "audio stream setup failed, continuing video-only");
            cfg_.audio_enabled = false;
        }
    } else if (audio) {
        // Compressed audio passthrough from the source file.
        hr = MFCreateSourceReaderFromURL(audio->input.c_str(), nullptr, &pt_reader_);
        if (FAILED(hr)) {
            CL_WARN("Encoder", "passthrough reader open failed, continuing video-only");
            pt_reader_.Reset();
        } else {
            for (DWORD i = 0; i < 4; ++i) {
                Microsoft::WRL::ComPtr<IMFMediaType> nat;
                if (FAILED(pt_reader_->GetNativeMediaType(i, 0, &nat))) break;
                GUID major{};
                nat->GetGUID(MF_MT_MAJOR_TYPE, &major);
                if (major != MFMediaType_Audio) continue;
                pt_src_audio_idx_ = static_cast<int>(i);
                if (SUCCEEDED(writer_->AddStream(nat.Get(), &pt_out_stream_)) &&
                    SUCCEEDED(writer_->SetInputMediaType(pt_out_stream_, nat.Get(), nullptr))) {
                    pt_reader_->SetCurrentMediaType(i, nullptr, nat.Get());
                    pt_active_ = true;
                    pt_seek_ = audio->start_hns;
                } else {
                    CL_WARN("Encoder", "passthrough stream setup failed");
                    pt_reader_.Reset();
                    pt_src_audio_idx_ = -1;
                }
                break;
            }
            if (pt_active_) {
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = audio->start_hns;
                pt_reader_->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
            }
        }
    }

    hr = writer_->BeginWriting();
    if (FAILED(hr)) {
        CL_ERROR("Encoder", "BeginWriting failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    running_ = true;
    return true;
}

bool MediaFoundationEncoder::push_video_frame(const uint8_t* nv12, uint32_t stride,
                                              int64_t time_100ns) {
    if (!running_) return false;
    const uint32_t w = cfg_.width;
    const uint32_t h = cfg_.height;
    const uint32_t buf_size = w * h * 3 / 2;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(buf_size, &buf);
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    hr = buf->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;

    const uint8_t* y_src = nv12;
    const uint8_t* uv_src = nv12 + static_cast<size_t>(stride) * h;
    BYTE* y_dst = dst;
    BYTE* uv_dst = dst + static_cast<size_t>(w) * h;
    for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(y_dst + static_cast<size_t>(row) * w,
                    y_src + static_cast<size_t>(row) * stride, w);
    }
    for (uint32_t row = 0; row < h / 2; ++row) {
        std::memcpy(uv_dst + static_cast<size_t>(row) * w,
                    uv_src + static_cast<size_t>(row) * stride, w);
    }
    buf->Unlock();
    buf->SetCurrentLength(buf_size);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time_100ns);
        sample->SetSampleDuration(10'000'000LL * cfg_.fps_den / cfg_.fps_num);
        hr = writer_->WriteSample(video_stream_, sample.Get());
    }
    return SUCCEEDED(hr);
}

bool MediaFoundationEncoder::push_audio_frames(const float* pcm, uint32_t frame_count,
                                               int64_t time_100ns) {
    if (!running_ || !cfg_.audio_enabled) return true;
    const uint32_t bytes = frame_count * cfg_.channels * sizeof(float);

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    hr = buf->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;
    std::memcpy(dst, pcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time_100ns);
        sample->SetSampleDuration(
            static_cast<LONGLONG>(frame_count) * 10'000'000LL / cfg_.sample_rate);
        hr = writer_->WriteSample(audio_stream_, sample.Get());
    }
    return SUCCEEDED(hr);
}

void MediaFoundationEncoder::pump_audio_until(int64_t video_time_100ns) {
    if (!running_ || !pt_active_) return;
    while (true) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        HRESULT hr = pt_reader_->ReadSample(static_cast<DWORD>(pt_src_audio_idx_), 0, nullptr,
                                            &flags, &ts, &sample);
        if (FAILED(hr)) {
            pt_active_ = false;
            return;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            pt_active_ = false;
            return;
        }
        if (!sample || (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
        if (ts > video_time_100ns + pt_seek_) break;  // not due yet
        if (pt_base_ < 0) pt_base_ = ts;
        sample->SetSampleTime(ts - pt_base_);
        writer_->WriteSample(pt_out_stream_, sample.Get());
    }
}

bool MediaFoundationEncoder::finish() {
    if (!running_) return true;
    if (pt_active_) pump_audio_until(INT64_MAX);
    HRESULT hr = writer_->Finalize();
    writer_.Reset();
    pt_reader_.Reset();
    running_ = false;
    if (mf_initialized_) {
        mf_drop_reference();
        mf_initialized_ = false;
    }
    if (FAILED(hr)) {
        CL_ERROR("Encoder", "Finalize failed (hr=" + hr_hex(hr) + ")");
        return false;
    }
    return true;
}

void MediaFoundationEncoder::abort() {
    if (!running_) return;
    writer_.Reset();
    pt_reader_.Reset();
    pt_active_ = false;
    running_ = false;
    if (mf_initialized_) {
        mf_drop_reference();
        mf_initialized_ = false;
    }
}

}  // namespace cliplite::encoder
