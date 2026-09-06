// Off-screen browser host: electron:<http(s) url> runs aisp.electron\electron.exe with the app
// in aisp.electron\app (stock Chromium, H.264 included) in a separate process, through the
// session helper shared with the primary browser (browser.cpp). It paints raw BGRA of the
// layout viewport on a named video pipe (latest frame; the hook blits the newest) and takes
// live scroll, scale, mute and gain lines on a named control pipe. There is no PCM tap: the
// hook sends a `gain` line (volume x distance x the game mixer mute/volume) and the host scales
// the page's own media elements, so the Windows mixer slider of the host stays the user's.
#include "source.h"
#include "browser.h"

#include <strsafe.h>
#include <cstring>
#include <cwchar>

namespace aisp
{
// aisp.electron\electron.exe next to the game ([tools] electron or AISP_ELECTRON overrides), with the app folder
// beside it.
DWORD RunElectronSource(ScreenStream* stream)
{
    wchar_t message[512] = {};
    const wchar_t* url = stream->source + 9;
    if (_wcsnicmp(url, L"http://", 7) != 0 && _wcsnicmp(url, L"https://", 8) != 0)
    {
        SetStatus(stream, L"browser: expected http(s)://...");
        return 0;
    }

    const int boxW = stream->videoWidth, boxH = stream->videoHeight, fps = stream->fps;
    const bool cropped = stream->crop[0] > 0 && stream->crop[1] > 0;
    const int viewW = cropped ? stream->crop[0] : boxW, viewH = cropped ? stream->crop[1] : boxH;
    const DWORD viewBytes = static_cast<DWORD>(viewW) * static_cast<DWORD>(viewH) * 4;
    EnterCriticalSection(&stream->lock);
    int scrollX = stream->pageScroll[0], scrollY = stream->pageScroll[1];
    const int hideScroll = stream->pageScrollLock ? 1 : 0;
    const int mute = stream->muted ? 1 : 0;
    const float gain = stream->pageGain;
    float scale = stream->pageScale > 0 ? stream->pageScale : 1.0f;
    LeaveCriticalSection(&stream->lock);

    StringCchPrintfW(message, 512, L"browser: %s view %dx%d box %dx%d scroll %d,%d scale %.3f", url, viewW, viewH, boxW, boxH, scrollX, scrollY, scale);
    SetStatus(stream, message);

    HANDLE controlPipe = nullptr, videoPipe = nullptr, process = nullptr;
    ElectronSessionRequest request;
    request.url = url;
    request.width = viewW;
    request.height = viewH;
    request.fps = fps > 0 ? fps : kDefaultFps;
    request.scrollx = scrollX;
    request.scrolly = scrollY;
    request.hideScroll = hideScroll;
    request.scale = scale;
    request.mute = mute;
    request.gain = gain;
    request.stop = &stream->stop;
    request.outControl = &controlPipe;
    request.outVideo = &videoPipe;
    request.outProcess = &process;
    if (!StartElectronSession(request, message, 512))
    {
        SetStatus(stream, message);
        return 0;
    }

    EnterCriticalSection(&stream->lock);
    stream->processes[0] = process;
    stream->controlWrite = controlPipe;
    LeaveCriticalSection(&stream->lock);
    SendBrowserControl(stream);

    SetStatus(stream, L"browser: loading");
    BYTE* view = new BYTE[viewBytes];
    BYTE* window = cropped ? new BYTE[stream->frameBytes] : nullptr;
    bool first = true;
    while (!stream->stop)
    {
        if (!ReadFully(videoPipe, view, viewBytes, &stream->stop))
            break;
        if (first)
        {
            SetStatus(stream, L"browser");
            first = false;
        }
        EnterCriticalSection(&stream->lock);
        const int cx = stream->pageCrop[0] > 0 ? stream->pageCrop[2] : 0;
        const int cy = stream->pageCrop[0] > 0 ? stream->pageCrop[3] : 0;
        LeaveCriticalSection(&stream->lock);
        if (window)
            CopyCropWindow(window, boxW, boxH, view, viewW, viewH, cx, cy);
        PushLiveFrame(stream, window ? window : view);
    }
    delete[] window;
    delete[] view;
    CloseHandle(videoPipe);
    if (!stream->stop)
        SetStatus(stream, L"browser host ended (see aisp.screen.log)");
    return 0;
}
} // namespace aisp
