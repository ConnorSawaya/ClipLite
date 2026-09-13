#include "cliplite/audio/session_monitor.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <cstdio>

#include "cliplite/log.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::audio {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

std::string exe_basename_for_pid(DWORD pid) {
    if (pid == 0) return "System Sounds";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "pid-" + std::to_string(pid);
    wchar_t path[MAX_PATH]{};
    DWORD n = static_cast<DWORD>(std::size(path));
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
        std::wstring w(path, n);
        const auto pos = w.find_last_of(L"\\/");
        const std::wstring base = (pos == std::wstring::npos) ? w : w.substr(pos + 1);
        out = util::wide_to_utf8(base);
        if (out.empty()) out = "pid-" + std::to_string(pid);
    } else {
        out = "pid-" + std::to_string(pid);
    }
    CloseHandle(h);
    return out;
}
}  // namespace

std::vector<AudibleApp> SessionMonitor::poll_audible(float peak_threshold) {
    std::vector<AudibleApp> out;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "session monitor: MMDeviceEnumerator failed (hr=" + hr_hex(hr) + ")");
        return out;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        // Headless / no render device: silence, not an error worth failing over.
        CL_WARN("Audio", "session monitor: no default render device (hr=" + hr_hex(hr) + ")");
        return out;
    }

    Microsoft::WRL::ComPtr<IAudioSessionManager2> mgr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(mgr.GetAddressOf()));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "session monitor: IAudioSessionManager2 failed (hr=" + hr_hex(hr) + ")");
        return out;
    }

    Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
    hr = mgr->GetSessionEnumerator(&sessions);
    if (FAILED(hr)) {
        CL_ERROR("Audio", "session monitor: GetSessionEnumerator failed (hr=" + hr_hex(hr) + ")");
        return out;
    }

    int count = 0;
    if (FAILED(sessions->GetCount(&count))) return out;
    for (int i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IAudioSessionControl> ctl;
        if (FAILED(sessions->GetSession(i, &ctl)) || !ctl) continue;

        Microsoft::WRL::ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED(ctl.As(&ctl2)) || !ctl2) continue;

        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(ctl2->GetState(&state))) continue;
        if (state == AudioSessionStateExpired) continue;

        DWORD pid = 0;
        if (FAILED(ctl2->GetProcessId(&pid))) continue;

        float peak = 0.f;
        Microsoft::WRL::ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(ctl.As(&meter)) && meter) {
            // May fail when the device was invalidated mid-poll; keep 0 then.
            (void)meter->GetPeakValue(&peak);
            if (!(peak >= 0.f)) peak = 0.f;  // guard NaN
        }

        const bool audible =
            (state == AudioSessionStateActive) || (peak > peak_threshold);
        if (!audible) continue;

        // One entry per pid: merge sessions of the same app, keep max peak.
        bool merged = false;
        for (auto& a : out) {
            if (a.pid == pid) {
                if (peak > a.peak) a.peak = peak;
                merged = true;
                break;
            }
        }
        if (!merged) {
            AudibleApp app;
            app.pid = pid;
            app.exe = exe_basename_for_pid(pid);
            app.peak = peak;
            out.push_back(std::move(app));
        }
    }
    return out;
}

}  // namespace cliplite::audio
