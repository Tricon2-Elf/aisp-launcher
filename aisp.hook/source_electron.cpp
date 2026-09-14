// Off-screen browser source: electron:<http(s) url>, a window in the game's shared Electron
// (browser.cpp, StartElectronSession). On Windows that is aisp.launch.data\electron\electron.exe
// over named pipes. On Wine the hook listens on loopback TCP and execs a stock native Linux
// Electron ([tools] electron_native) with the same app — Wine named pipes are not a Unix socket it could
// connect to. Paint is latest-frame BGRA on the framed channel, which also carries the
// page's title (kept as the stream's media title, for the primary page); live
// scroll/scale/mute/gain on control.
#include "source.h"
#include "browser.h"

#include <strsafe.h>
#include <cstring>
#include <cwchar>

namespace aisp
{
// aisp.launch.data\electron\electron.exe next to the game ([tools] electron or AISP_ELECTRON overrides), with the app at
// resources\app (or a sibling app\ folder).
// A root-relative URL (a page or script of the emulator's own, such as the YouTube embed
// page) is taken at the origin the screen page came from, so the server need not know its
// public address; an absolute one is kept. False when it is neither.
bool ResolveAgainstPage(ScreenStream* stream, const wchar_t* url, wchar_t* out, size_t outCount)
{
    if (url[0] == L'/' && url[1] != L'/')
    {
        const wchar_t* base = stream->pageUrl;
        const wchar_t* scheme = std::wcsstr(base, L"://");
        const wchar_t* pathStart = scheme ? std::wcschr(scheme + 3, L'/') : nullptr;
        if (!scheme || !pathStart)
            return false;
        StringCchCopyNW(out, outCount, base, pathStart - base);
        StringCchCatW(out, outCount, url);
        return true;
    }
    if (_wcsnicmp(url, L"http://", 7) != 0 && _wcsnicmp(url, L"https://", 8) != 0)
        return false;
    StringCchCopyW(out, outCount, url);
    return true;
}

DWORD RunElectronSource(ScreenStream* stream)
{
    wchar_t message[512] = {};
    wchar_t resolved[4096] = {};
    if (!ResolveAgainstPage(stream, stream->source + 9, resolved, 4096))
    {
        SetStatus(stream, L"browser: expected http(s)://... or /path (with a screen page origin)");
        return 0;
    }
    const wchar_t* url = resolved;
    // run=<url> from the title: a script the host runs in the page once loaded.
    wchar_t run[1024] = {};
    EnterCriticalSection(&stream->lock);
    StringCchCopyW(stream->sessionRun, 1024, stream->pageRun);
    const bool haveRun = stream->pageRun[0] != L'\0';
    wchar_t runWanted[1024] = {};
    StringCchCopyW(runWanted, 1024, stream->pageRun);
    LeaveCriticalSection(&stream->lock);
    if (haveRun && !ResolveAgainstPage(stream, runWanted, run, 1024))
    {
        LogLine("browser: run= is not an http(s) URL or /path; ignored\r\n");
        run[0] = L'\0';
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
    bool tcp = false;
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
    request.framed = true;
    request.run = run[0] ? run : nullptr;
    request.stop = &stream->stop;
    request.outControl = &controlPipe;
    request.outVideo = &videoPipe;
    request.outProcess = &process;
    request.outTcp = &tcp;
    if (!StartElectronSession(request, message, 512))
    {
        SetStatus(stream, message);
        return 0;
    }

    EnterCriticalSection(&stream->lock);
    stream->processes[0] = process;
    stream->controlWrite = controlPipe;
    stream->electronTcp = tcp;
    LeaveCriticalSection(&stream->lock);
    SendBrowserControl(stream);

    SetStatus(stream, L"browser: loading");
    struct Reader
    {
        ScreenStream* stream;
        BYTE* window;
        int boxW, boxH, viewW, viewH;
        bool first;
    } reader = {stream, cropped ? new BYTE[stream->frameBytes] : nullptr, boxW, boxH, viewW, viewH, true};
    FramedSink sink;
    sink.context = &reader;
    sink.onFrame = [](void* context, const BYTE* view, DWORD) {
        Reader* r = static_cast<Reader*>(context);
        if (r->first)
        {
            SetStatus(r->stream, L"browser");
            r->first = false;
        }
        EnterCriticalSection(&r->stream->lock);
        const int cx = r->stream->pageCrop[0] > 0 ? r->stream->pageCrop[2] : 0;
        const int cy = r->stream->pageCrop[0] > 0 ? r->stream->pageCrop[3] : 0;
        LeaveCriticalSection(&r->stream->lock);
        if (r->window)
            CopyCropWindow(r->window, r->boxW, r->boxH, view, r->viewW, r->viewH, cx, cy);
        PushLiveFrame(r->stream, r->window ? r->window : view);
    };
    sink.onText = [](void* context, const char* text) {
        Reader* r = static_cast<Reader*>(context);
        if (std::strncmp(text, "title ", 6) == 0)
        {
            // The page's own document.title, as it changes: the primary page is told (page_state.cpp).
            EnterCriticalSection(&r->stream->lock);
            MultiByteToWideChar(CP_UTF8, 0, text + 6, -1, r->stream->mediaTitle, 512);
            LeaveCriticalSection(&r->stream->lock);
        }
        else if (std::strncmp(text, "failed ", 7) == 0)
        {
            wchar_t message[512] = {};
            MultiByteToWideChar(CP_UTF8, 0, text + 7, -1, message, 480);
            wchar_t status[512] = {};
            StringCchPrintfW(status, 512, L"browser: load failed: %s", message);
            SetStatus(r->stream, status);
        }
    };
    ReadFramedChannel(videoPipe, &stream->stop, viewBytes, "browser", sink);
    delete[] reader.window;
    CloseHandle(videoPipe);
    if (!stream->stop)
        SetStatus(stream, L"browser host ended (see aisp.screen.log)");
    return 0;
}
} // namespace aisp
