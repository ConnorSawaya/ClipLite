#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace cliplite::encoder {

struct EncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps_num = 60;
    uint32_t fps_den = 1;
    uint32_t video_bitrate_bps = 15'000'000;
    bool audio_enabled = true;
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t audio_bitrate_bps = 192'000;
};

// Media Foundation process-lifetime pinning. Hold one reference for the whole
// recording pipeline so per-segment encoder create/destroy never cycles
// MFStartup/MFShutdown - frequent cycles leak kernel handles (~1000+/cycle on
// some driver stacks) and were the main RAM/handle growth source.
bool mf_hold_reference();
void mf_drop_reference();

// Copies compressed audio from an existing file into the output untouched.
struct AudioPassthrough {
    std::wstring input;
    int64_t start_hns = 0;  // source time where the new file's t=0 begins
};

// Media Foundation Sink Writer encoder: NV12 video -> H.264, float PCM -> AAC,
// muxed into an MP4. Uses MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS so the OS
// hardware H.264 encoder (Quick Sync/NVENC via MF) is selected when available,
// falling back to software. This is the pragmatic first encoder; dedicated
// NVENC/AMF paths can be added behind the same push/finish interface later.
class MediaFoundationEncoder {
public:
    MediaFoundationEncoder() = default;
    ~MediaFoundationEncoder();
    MediaFoundationEncoder(const MediaFoundationEncoder&) = delete;
    MediaFoundationEncoder& operator=(const MediaFoundationEncoder&) = delete;

    bool start(const std::wstring& path, const EncoderConfig& cfg);
    // Same as start(), but also muxes compressed audio copied from `audio`
    // (seeking to its start offset). Pump it alongside video with
    // pump_audio_until(); finish() drains the remainder.
    bool start(const std::wstring& path, const EncoderConfig& cfg,
               const AudioPassthrough& audio);

    // Copies queued passthrough samples whose timestamps are <= video_time_100ns.
    void pump_audio_until(int64_t video_time_100ns);

    // nv12 points to Y (stride x height) followed by interleaved UV. stride is
    // the source row stride in bytes; the buffer is copied into tight NV12.
    bool push_video_frame(const uint8_t* nv12, uint32_t stride, int64_t time_100ns);

    // Interleaved float PCM.
    bool push_audio_frames(const float* pcm, uint32_t frame_count, int64_t time_100ns);

    // Finalizes the MP4 (writes moov). Returns false if Finalize fails.
    bool finish();

    // Abandons the current output without finalizing (no moov written).
    void abort();

    bool running() const { return running_; }
    uint32_t width() const { return cfg_.width; }
    uint32_t height() const { return cfg_.height; }

private:
    bool start_internal(const std::wstring& path, bool use_hardware,
                        const AudioPassthrough* audio);

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    Microsoft::WRL::ComPtr<IMFSourceReader> pt_reader_;
    int pt_src_audio_idx_ = -1;
    DWORD pt_out_stream_ = 0;
    LONGLONG pt_base_ = -1;       // first written source ts, rebased to t=0
    LONGLONG pt_seek_ = 0;        // requested source start
    bool pt_active_ = false;
    EncoderConfig cfg_{};
    DWORD video_stream_ = 0;
    DWORD audio_stream_ = 0;
    bool running_ = false;
    bool mf_initialized_ = false;
};

}  // namespace cliplite::encoder
