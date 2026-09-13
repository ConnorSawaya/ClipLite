#include "cliplite/player/video_player.h"

#include <mferror.h>
#include <propvarutil.h>

#include <cstdio>

#include "cliplite/log.h"

namespace cliplite::player {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}
}  // namespace

// IMFMediaSession::Start rejects a null position PROPVARIANT on some versions
// (E_POINTER); always pass a valid VT_EMPTY (resume) or VT_I8 (seek) variant.
HRESULT VideoPlayer::start_session(const int64_t* seek_ms) {
    PROPVARIANT var;
    PropVariantInit(&var);
    if (seek_ms) InitPropVariantFromInt64(*seek_ms * 10000, &var);
    const HRESULT hr = session_->Start(nullptr, &var);
    PropVariantClear(&var);
    return hr;
}

VideoPlayer::~VideoPlayer() {
    close();
}

bool VideoPlayer::open(HWND video_hwnd, const std::wstring& url) {
    if (session_) close();
    hwnd_ = video_hwnd;
    current_path_ = url;
    duration_ms_ = 0;
    ended_ = false;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        CL_ERROR("Player", "MFStartup failed (hr=" + hr_hex(hr) + ")");
        return false;
    }
    mf_initialized_ = true;

    hr = MFCreateMediaSession(nullptr, &session_);
    if (FAILED(hr)) {
        CL_ERROR("Player", "MFCreateMediaSession failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSourceResolver> resolver;
    hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) return false;

    MF_OBJECT_TYPE obj_type = MF_OBJECT_INVALID;
    Microsoft::WRL::ComPtr<IUnknown> source_unk;
    hr = resolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr, &obj_type,
                                       &source_unk);
    if (FAILED(hr)) {
        CL_ERROR("Player", "CreateObjectFromURL failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaSource> source;
    hr = source_unk.As(&source);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd;
    hr = source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) return false;
    UINT64 dur100ns = 0;
    if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &dur100ns))) duration_ms_ = dur100ns / 10000;

    Microsoft::WRL::ComPtr<IMFTopology> topology;
    hr = MFCreateTopology(&topology);
    if (FAILED(hr)) return false;

    DWORD count = 0;
    pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        Microsoft::WRL::ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
        if (FAILED(hr) || !selected) continue;

        Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
        if (FAILED(sd->GetMediaTypeHandler(&handler))) continue;
        GUID major{};
        if (FAILED(handler->GetMajorType(&major))) continue;

        Microsoft::WRL::ComPtr<IMFActivate> activate;
        if (major == MFMediaType_Video) {
            hr = MFCreateVideoRendererActivate(hwnd_, &activate);
        } else if (major == MFMediaType_Audio) {
            hr = MFCreateAudioRendererActivate(&activate);
        } else {
            continue;
        }
        if (FAILED(hr)) continue;

        Microsoft::WRL::ComPtr<IMFTopologyNode> src_node;
        MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &src_node);
        src_node->SetUnknown(MF_TOPONODE_SOURCE, source.Get());
        src_node->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd.Get());
        src_node->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd.Get());

        Microsoft::WRL::ComPtr<IMFTopologyNode> out_node;
        MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &out_node);
        out_node->SetObject(activate.Get());

        topology->AddNode(src_node.Get());
        topology->AddNode(out_node.Get());
        src_node->ConnectOutput(0, out_node.Get(), 0);
    }

    hr = session_->SetTopology(0, topology.Get());
    if (FAILED(hr)) {
        CL_ERROR("Player", "SetTopology failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSimpleAudioVolume> vol;
    if (SUCCEEDED(MFGetService(session_.Get(), MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&vol)))) {
        volume_ = vol;
    }

    hr = start_session(nullptr);
    if (FAILED(hr)) {
        CL_ERROR("Player", "Start failed (hr=" + hr_hex(hr) + ")");
        return false;
    }
    playing_ = true;
    CL_INFO("Player", "opened (" + std::to_string(duration_ms_) + " ms)");
    return true;
}

void VideoPlayer::close() {
    if (session_) {
        session_->Stop();
        session_->Close();
        // MF requires waiting for MESessionClosed before releasing the session.
        for (int i = 0; i < 50; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
            if (SUCCEEDED(session_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) {
                MediaEventType type = MEUnknown;
                ev->GetType(&type);
                if (type == MESessionClosed) break;
            }
            Sleep(10);
        }
        session_->Shutdown();
        session_.Reset();
    }
    volume_.Reset();
    playing_ = false;
    ended_ = false;
    current_path_.clear();
    hwnd_ = nullptr;
    if (mf_initialized_) {
        MFShutdown();
        mf_initialized_ = false;
    }
}

void VideoPlayer::ensure_volume() {
    if (!volume_ && session_) {
        Microsoft::WRL::ComPtr<IMFSimpleAudioVolume> vol;
        if (SUCCEEDED(MFGetService(session_.Get(), MR_POLICY_VOLUME_SERVICE,
                                   IID_PPV_ARGS(&vol)))) {
            volume_ = vol;
        }
    }
}

void VideoPlayer::play() {
    if (!session_) return;
    HRESULT hr;
    if (ended_) {
        const int64_t zero = 0;
        hr = start_session(&zero);
    } else {
        hr = start_session(nullptr);
    }
    if (SUCCEEDED(hr)) {
        playing_ = true;
        ended_ = false;
    }
}

void VideoPlayer::pause() {
    if (!session_) return;
    session_->Pause();
    playing_ = false;
}

void VideoPlayer::toggle_play() {
    if (playing_) pause();
    else play();
}

void VideoPlayer::seek_ms(int64_t ms) {
    if (!session_) return;
    if (ms < 0) ms = 0;
    const HRESULT hr = start_session(&ms);
    if (SUCCEEDED(hr)) {
        playing_ = true;
        ended_ = false;
    }
}

void VideoPlayer::set_volume(float v) {
    ensure_volume();
    if (volume_) volume_->SetMasterVolume(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
}

void VideoPlayer::set_mute(bool muted) {
    ensure_volume();
    if (volume_) volume_->SetMute(muted ? TRUE : FALSE);
}

int64_t VideoPlayer::position_ms() {
    if (!session_) return 0;
    Microsoft::WRL::ComPtr<IMFClock> clock;
    if (SUCCEEDED(session_->GetClock(&clock))) {
        MFTIME time = 0;
        if (SUCCEEDED(clock->GetCorrelatedTime(0, &time, nullptr))) {
            return static_cast<int64_t>(time / 10000);
        }
    }
    return 0;
}

void VideoPlayer::process_events() {
    if (!session_) return;
    Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
    while (SUCCEEDED(session_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) {
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == MESessionEnded) {
            playing_ = false;
            ended_ = true;
        }
    }
}

}  // namespace cliplite::player
