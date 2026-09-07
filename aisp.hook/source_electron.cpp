// Off-screen browser host: electron:<http(s) url> runs aisp.electron\electron.exe with the app
// in aisp.electron\app (stock Chromium, H.264 included) in a separate process. It paints raw
// BGRA of the layout viewport on a named video pipe (latest frame; the hook blits the newest)
// and takes live scroll, scale, mute and gain lines on a named control pipe. Chromium helpers
// inherit stdin and stdout, so neither is used. There is no PCM tap: the hook sends a `gain`
// line (volume × distance × the game mixer mute/volume) and the host scales the page's own
// media elements, so the Windows mixer slider of the host stays the user's.
#include "source.h"

#include <strsafe.h>
#include <cstring>
#include <cwchar>

namespace aisp
{
namespace
{
bool ConnectPipe(HANDLE pipe, int tries = 160)
{
    for (int i = 0; i < tries; ++i)
    {
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
        {
            DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            return true;
        }
        Sleep(50);
    }
    return false;
}
} // namespace

// aisp.electron\electron.exe next to the game ([tools] electron or AISP_ELECTRON overrides), with the app folder
// beside it.
DWORD RunElectronSource(ScreenStream* stream)
{
    wchar_t browser[MAX_PATH] = {};
    wchar_t appPath[MAX_PATH] = {};
    wchar_t message[512] = {};
    if (!ToolPath(L"AISP_ELECTRON", L"electron", L"aisp.electron\\electron.exe", browser, MAX_PATH))
    {
        StringCchPrintfW(message, 512, L"browser host not found: %s", browser);
        SetStatus(stream, message);
        return 0;
    }
    StringCchCopyW(appPath, MAX_PATH, browser);
    wchar_t* slash = std::wcsrchr(appPath, L'\\');
    if (!slash)
    {
        SetStatus(stream, L"browser: host path has no directory");
        return 0;
    }
    slash[1] = 0;
    StringCchCatW(appPath, MAX_PATH, L"app");
    if (GetFileAttributesW(appPath) == INVALID_FILE_ATTRIBUTES)
    {
        StringCchPrintfW(message, 512, L"browser app not found: %s", appPath);
        SetStatus(stream, message);
        return 0;
    }

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

    wchar_t controlName[128] = {}, videoName[128] = {};
    HANDLE controlPipe = CreateNamedPipePair(controlName, 128, L"ctl");
    HANDLE videoPipe = CreateNamedPipePair(videoName, 128, L"vid");
    if (controlPipe == INVALID_HANDLE_VALUE || videoPipe == INVALID_HANDLE_VALUE)
    {
        if (controlPipe != INVALID_HANDLE_VALUE)
            CloseHandle(controlPipe);
        if (videoPipe != INVALID_HANDLE_VALUE)
            CloseHandle(videoPipe);
        SetStatus(stream, L"browser: pipe creation failed");
        return 0;
    }

    wchar_t command[4096] = {};
    StringCchPrintfW(
        command,
        4096,
        L"\"%s\" \"%s\" --width=%d --height=%d --fps=%d --scrollx=%d --scrolly=%d --hide-scrollbars=%d --scale=%.4f --mute=%d --gain=%.4f --control=\"%s\" --video=\"%s\" --url=\"%s\"",
        browser,
        appPath,
        viewW,
        viewH,
        fps > 0 ? fps : kDefaultFps,
        scrollX,
        scrollY,
        hideScroll,
        scale,
        mute,
        gain,
        controlName,
        videoName,
        url
    );
    HANDLE process = LaunchTool(command, nullptr, nullptr);
    if (!process)
    {
        CloseHandle(controlPipe);
        CloseHandle(videoPipe);
        SetStatus(stream, L"browser host failed to start (see aisp.screen.log)");
        return 0;
    }

    if (!ConnectPipe(controlPipe))
        LogLine("browser: control pipe connect failed\r\n");
    if (!ConnectPipe(videoPipe))
    {
        LogLine("browser: video pipe connect failed\r\n");
        SetStatus(stream, L"browser: video pipe connect failed");
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
