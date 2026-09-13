#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace cliplite::player {

// Native Media Foundation playback: media session + Enhanced Video Renderer (EVR)
// for video and Streaming Audio Renderer (SAR) for audio. Renders into a Win32
// window; supports play/pause/seek/volume/mute.
class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Opens a clip and starts playback into `video_hwnd`.
    bool open(HWND video_hwnd, const std::wstring& url);
    void close();

    void play();
    void pause();
    void toggle_play();
    void seek_ms(int64_t ms);
    void set_volume(float v);  // 0.0 .. 1.0
    void set_mute(bool muted);

    // Drain pending media-session events (call periodically, e.g. on a timer).
    void process_events();

    bool playing() const { return playing_; }
    bool ended() const { return ended_; }
    bool opened() const { return session_ != nullptr; }
    const std::wstring& current_path() const { return current_path_; }
    int64_t position_ms();
    int64_t duration_ms() const { return duration_ms_; }

private:
    HRESULT start_session(const int64_t* seek_ms);
    void ensure_volume();

    Microsoft::WRL::ComPtr<IMFMediaSession> session_;
    Microsoft::WRL::ComPtr<IMFSimpleAudioVolume> volume_;
    HWND hwnd_ = nullptr;
    std::wstring current_path_;
    int64_t duration_ms_ = 0;
    bool playing_ = false;
    bool ended_ = false;
    bool mf_initialized_ = false;
};

}  // namespace cliplite::player
