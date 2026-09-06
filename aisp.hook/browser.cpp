// Primary off-screen Electron for in-game screens. See browser.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "browser.h"
#include "config.h"

#include <strsafe.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace aisp
{
namespace
{
bool g_usePrimaryBrowser = false;
bool g_browserModeReady = false;

void CloseHandleIf(HANDLE* handle)
{
    if (handle && *handle && *handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(*handle);
        *handle = nullptr;
    }
}

void JoinPrimaryThread(HANDLE thread, const char* name)
{
    if (!thread)
        return;
    CancelSynchronousIo(thread);
    if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0)
    {
        char note[96] = {};
        StringCchPrintfA(note, 96, "primary browser: %s thread did not end in time\r\n", name);
        LogLine(note);
    }
    CloseHandle(thread);
}

void PushPageFrame(ScreenStream* stream, const BYTE* frame, DWORD bytes)
{
    EnterCriticalSection(&stream->lock);
    if (stream->pageFrame && bytes && bytes == stream->pageBytes)
    {
        std::memcpy(stream->pageFrame, frame, bytes);
        stream->pageReady = true;
    }
    LeaveCriticalSection(&stream->lock);
}

void SetElectronTitle(ScreenStream* stream, const wchar_t* title)
{
    EnterCriticalSection(&stream->lock);
    StringCchCopyW(stream->electronTitle, 1024, title ? title : L"");
    stream->electronTitleNew = true;
    LeaveCriticalSection(&stream->lock);
}

// aisp.electron\electron.exe ([tools] electron) with the app folder beside it.
bool ResolveElectronPaths(wchar_t* browser, size_t browserCount, wchar_t* appPath, size_t appCount, wchar_t* error, size_t errorCount)
{
    if (!ToolPath(L"AISP_ELECTRON", L"electron", L"aisp.electron\\electron.exe", browser, browserCount))
    {
        if (error)
            StringCchPrintfW(error, errorCount, L"browser host not found: %s", browser);
        return false;
    }
    StringCchCopyW(appPath, appCount, browser);
    wchar_t* slash = std::wcsrchr(appPath, L'\\');
    if (!slash)
    {
        if (error)
            StringCchCopyW(error, errorCount, L"browser: host path has no directory");
        return false;
    }
    slash[1] = 0;
    StringCchCatW(appPath, appCount, L"app");
    if (GetFileAttributesW(appPath) == INVALID_FILE_ATTRIBUTES)
    {
        if (error)
            StringCchPrintfW(error, errorCount, L"browser app not found: %s", appPath);
        return false;
    }
    return true;
}
} // namespace

void InitBrowserMode()
{
    if (g_browserModeReady)
        return;
    // electron, or ie (the default; anything else reads as ie).
    wchar_t value[32] = {};
    ConfigString(L"AISP_PRIMARY_BROWSER", L"screens", L"primary_browser", value, 32);
    g_usePrimaryBrowser = _wcsicmp(value, L"electron") == 0;
    g_browserModeReady = true;
    wchar_t line[160] = {};
    StringCchPrintfW(line, 160, L"aisp.hook: primary-browser=%s\n", g_usePrimaryBrowser ? L"electron" : L"ie");
    OutputDebugStringW(line);
}

bool UsePrimaryBrowser()
{
    if (!g_browserModeReady)
        InitBrowserMode();
    return g_usePrimaryBrowser;
}

bool StartElectronSession(const ElectronSessionRequest& request, wchar_t* error, size_t errorCount)
{
    if (!request.url || !request.outControl || !request.outVideo)
    {
        if (error)
            StringCchCopyW(error, errorCount, L"browser: bad session request");
        return false;
    }
    *request.outControl = nullptr;
    *request.outVideo = nullptr;
    if (request.outProcess)
        *request.outProcess = nullptr;

    wchar_t controlSpec[128] = {}, videoSpec[128] = {};
    HANDLE controlListen = INVALID_HANDLE_VALUE, videoListen = INVALID_HANDLE_VALUE;
    {
        wchar_t browser[MAX_PATH] = {}, appPath[MAX_PATH] = {};
        if (!ResolveElectronPaths(browser, MAX_PATH, appPath, MAX_PATH, error, errorCount))
            return false;
        controlListen = CreateNamedPipePair(controlSpec, 128, L"ctl");
        videoListen = CreateNamedPipePair(videoSpec, 128, L"vid");
        if (controlListen == INVALID_HANDLE_VALUE || videoListen == INVALID_HANDLE_VALUE)
        {
            if (controlListen != INVALID_HANDLE_VALUE)
                CloseHandle(controlListen);
            if (videoListen != INVALID_HANDLE_VALUE)
                CloseHandle(videoListen);
            if (error)
                StringCchCopyW(error, errorCount, L"browser: pipe creation failed");
            return false;
        }
        wchar_t command[4096] = {};
        StringCchPrintfW(
            command,
            4096,
            L"\"%s\" \"%s\" --width=%d --height=%d --fps=%d --scrollx=%d --scrolly=%d --hide-scrollbars=%d --scale=%.4f --mute=%d --gain=%.4f --framed=%d --control=\"%s\" --video=\"%s\" --url=\"%s\"",
            browser,
            appPath,
            request.width,
            request.height,
            request.fps > 0 ? request.fps : kDefaultFps,
            request.scrollx,
            request.scrolly,
            request.hideScroll,
            request.scale > 0 ? request.scale : 1.0f,
            request.mute,
            request.gain,
            request.framed ? 1 : 0,
            controlSpec,
            videoSpec,
            request.url
        );
        HANDLE process = LaunchTool(command, nullptr, nullptr);
        if (!process)
        {
            CloseHandle(controlListen);
            CloseHandle(videoListen);
            if (error)
                StringCchCopyW(error, errorCount, L"browser host failed to start (see aisp.screen.log)");
            return false;
        }
        if (request.outProcess)
            *request.outProcess = process;
    }

    HANDLE control = nullptr, video = nullptr;
    {
        // The video pipe first, with the whole budget: the host connects both pipes at once,
        // so once that one is in the other needs a moment at most.
        if (!ConnectNamedPipeWait(videoListen, request.stop))
        {
            if (error)
                StringCchCopyW(error, errorCount, L"browser: video pipe connect failed");
            CloseHandle(controlListen);
            CloseHandle(videoListen);
            if (request.outProcess && *request.outProcess)
            {
                TerminateProcess(*request.outProcess, 0);
                CloseHandle(*request.outProcess);
                *request.outProcess = nullptr;
            }
            return false;
        }
        if (!ConnectNamedPipeWait(controlListen, request.stop, 40))
            LogLine("browser: control pipe connect failed\r\n");
        control = controlListen;
        video = videoListen;
    }
    *request.outControl = control;
    *request.outVideo = video;
    return true;
}

bool BlitPrimaryPage(ScreenStream* stream, HDC hdc, int destX, int destY)
{
    if (!stream->pageReady || !stream->pagePresent || !stream->pageFrame || !stream->pageBytes)
        return false;
    if (stream->x + stream->width > stream->pageViewWidth || stream->y + stream->height > stream->pageViewHeight)
        return false; // the view was sized for another crop; the next start fixes it
    const size_t rowBytes = static_cast<size_t>(stream->width) * 4;
    const size_t viewStride = static_cast<size_t>(stream->pageViewWidth) * 4;
    const BYTE* source = stream->pageFrame + static_cast<size_t>(stream->y) * viewStride + static_cast<size_t>(stream->x) * 4;
    for (int row = 0; row < stream->height; ++row)
        std::memcpy(stream->pagePresent + row * rowBytes, source + row * viewStride, rowBytes);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = stream->width;
    info.bmiHeader.biHeight = -stream->height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, destX, destY, stream->width, stream->height, 0, 0, 0, stream->height, stream->pagePresent, &info, DIB_RGB_COLORS);
    return true;
}

void SendPrimaryControl(ScreenStream* stream)
{
    if (!stream->primaryControl)
        return;
    const int mute = stream->muted || (stream->rolloff && stream->distanceGain < 0.02f) ? 1 : 0;
    const float gain = stream->pageGain;
    DWORD written = 0;
    if (mute != stream->sentPrimaryMute)
    {
        char line[32] = {};
        StringCchPrintfA(line, 32, "mute %d\n", mute);
        WriteFile(stream->primaryControl, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        stream->sentPrimaryMute = mute;
    }
    const float sent = stream->sentPrimaryGain;
    if (sent >= 0.0f && gain > sent - 0.002f && gain < sent + 0.002f)
        return;
    char line[32] = {};
    StringCchPrintfA(line, 32, "gain %.4f\n", gain);
    WriteFile(stream->primaryControl, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    stream->sentPrimaryGain = gain;
}

void ForwardScriptToPrimary(ScreenStream* stream, const wchar_t* script)
{
    if (!stream || !stream->primaryControl || !script || !script[0])
        return;
    char utf8[3500] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, script, -1, utf8, static_cast<int>(sizeof(utf8) - 1), nullptr, nullptr);
    if (n <= 1)
        return;
    for (char* p = utf8; *p; ++p)
    {
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    }
    char line[3600] = {};
    if (FAILED(StringCchPrintfA(line, 3600, "eval %s\n", utf8)))
        return;
    DWORD written = 0;
    WriteFile(stream->primaryControl, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    EnterCriticalSection(&stream->lock);
    ++stream->primaryEvals;
    LeaveCriticalSection(&stream->lock);
}

void CountPrimaryRead(ScreenStream* stream, const wchar_t* id, double waitMs, bool ok)
{
    EnterCriticalSection(&stream->lock);
    if (std::wcscmp(id, L"statusForm") == 0)
        ++stream->primaryReadsStatus;
    else
        ++stream->primaryReadsOther;
    if (!ok)
        ++stream->primaryReadsFailed;
    stream->primaryWaitMs += waitMs;
    if (waitMs > stream->primaryWaitMaxMs)
        stream->primaryWaitMaxMs = waitMs;
    LeaveCriticalSection(&stream->lock);
}

// Every five seconds while the primary runs: how often the client read the page through the
// synthesized document and how long the game's thread waited for it. Caller does not hold
// stream->lock.
void LogPrimaryStats(ScreenStream* stream)
{
    const ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&stream->lock);
    if (!stream->primaryStatsAt)
        stream->primaryStatsAt = now;
    const ULONGLONG elapsed = now - stream->primaryStatsAt;
    if (elapsed < 5000 || !stream->primaryActive)
    {
        LeaveCriticalSection(&stream->lock);
        return;
    }
    const DWORD reads = stream->primaryReadsStatus + stream->primaryReadsOther;
    char line[400] = {};
    StringCchPrintfA(line, 400, "primary stats: %.1f s: %lu reads (%lu statusForm, %lu other, %lu failed), %lu evals, waited %.1f ms total, %.2f ms avg, %.1f ms max, %.2f%% of the interval\r\n",
                     elapsed / 1000.0, static_cast<unsigned long>(reads), static_cast<unsigned long>(stream->primaryReadsStatus), static_cast<unsigned long>(stream->primaryReadsOther),
                     static_cast<unsigned long>(stream->primaryReadsFailed), static_cast<unsigned long>(stream->primaryEvals), stream->primaryWaitMs, reads ? stream->primaryWaitMs / reads : 0.0,
                     stream->primaryWaitMaxMs, stream->primaryWaitMs / elapsed * 100.0);
    stream->primaryReadsStatus = stream->primaryReadsOther = stream->primaryEvals = stream->primaryReadsFailed = 0;
    stream->primaryWaitMs = stream->primaryWaitMaxMs = 0;
    stream->primaryStatsAt = now;
    LeaveCriticalSection(&stream->lock);
    LogLine(line);
}

void FreePrimary(ScreenStream* stream)
{
    if (!stream->primaryActive && stream->primaryThread)
    {
        JoinPrimaryThread(stream->primaryThread, "video");
        stream->primaryThread = nullptr;
    }
    EnterCriticalSection(&stream->lock);
    delete[] stream->pageFrame;
    delete[] stream->pagePresent;
    stream->pageFrame = stream->pagePresent = nullptr;
    stream->pageReady = false;
    stream->pageBytes = stream->pagePresentBytes = 0;
    stream->pageViewWidth = stream->pageViewHeight = 0;
    stream->electronTitle[0] = L'\0';
    stream->electronTitleNew = false;
    CloseHandleIf(&stream->primaryCallEvent);
    LeaveCriticalSection(&stream->lock);
}

void StopPrimaryBrowser(ScreenStream* stream)
{
    if (!stream->primaryActive)
        return;
    stream->primaryActive = false;
    InterlockedExchange(&stream->primaryStop, 1);
    HANDLE process = nullptr;
    HANDLE control = nullptr;
    HANDLE video = nullptr;
    HANDLE videoThread = nullptr;
    EnterCriticalSection(&stream->lock);
    process = stream->primaryProcess;
    stream->primaryProcess = nullptr;
    control = stream->primaryControl;
    stream->primaryControl = nullptr;
    video = stream->primaryVideo;
    stream->primaryVideo = nullptr;
    videoThread = stream->primaryThread;
    stream->primaryThread = nullptr;
    stream->pageReady = false;
    stream->sentPrimaryMute = -1;
    stream->sentPrimaryGain = -1.0f;
    // A call still waiting gets its answer: none.
    stream->primaryCallOk = false;
    stream->primaryCallLoading = false;
    stream->primaryCallDone = stream->primaryCallId;
    if (stream->primaryCallEvent)
        SetEvent(stream->primaryCallEvent);
    LeaveCriticalSection(&stream->lock);
    // The video thread sits in a synchronous ReadFile on its pipe, and closing such a handle
    // waits for that read to finish; a settled page sends nothing, so the close would never
    // return. The host goes first (its end of the pipe breaks the read), the thread is joined,
    // and the handles are closed last.
    if (process)
    {
        TerminateProcess(process, 0);
        CloseHandle(process);
    }
    JoinPrimaryThread(videoThread, "video");
    CloseHandleIf(&control);
    CloseHandleIf(&video);
    LogLine("primary browser stopped\r\n");
}

DWORD WINAPI PrimaryVideoThread(LPVOID parameter);

void StartPrimaryBrowser(ScreenStream* stream, const wchar_t* url)
{
    if (!UsePrimaryBrowser() || !stream || !url || !url[0])
        return;
    if (stream->primaryActive)
        StopPrimaryBrowser(stream);
    else if (stream->primaryThread)
    {
        // The last start failed and its thread has ended (or is about to).
        JoinPrimaryThread(stream->primaryThread, "video");
        stream->primaryThread = nullptr;
    }

    const int viewWidth = stream->x + stream->width;
    const int viewHeight = stream->y + stream->height;
    const DWORD pageBytes = static_cast<DWORD>(viewWidth) * static_cast<DWORD>(viewHeight) * 4;
    const DWORD presentBytes = static_cast<DWORD>(stream->width) * static_cast<DWORD>(stream->height) * 4;
    if (!pageBytes || !presentBytes)
        return;
    stream->primaryRetryAt = 0;
    EnterCriticalSection(&stream->lock);
    if (pageBytes != stream->pageBytes)
    {
        delete[] stream->pageFrame;
        stream->pageFrame = new BYTE[pageBytes]();
        stream->pageBytes = pageBytes;
    }
    if (presentBytes != stream->pagePresentBytes)
    {
        delete[] stream->pagePresent;
        stream->pagePresent = new BYTE[presentBytes]();
        stream->pagePresentBytes = presentBytes;
    }
    stream->pageViewWidth = viewWidth;
    stream->pageViewHeight = viewHeight;
    stream->pageReady = false;
    stream->electronTitle[0] = L'\0';
    stream->electronTitleNew = false;
    stream->sentPrimaryMute = -1;
    stream->sentPrimaryGain = -1.0f;
    if (!stream->primaryCallEvent)
        stream->primaryCallEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    StringCchCopyW(stream->pageUrl, 4096, url);
    LeaveCriticalSection(&stream->lock);

    InterlockedExchange(&stream->primaryStop, 0);
    stream->primaryActive = true;
    stream->primaryThread = CreateThread(nullptr, 0, PrimaryVideoThread, stream, 0, nullptr);
    if (!stream->primaryThread)
    {
        stream->primaryActive = false;
        LogLine("primary browser: thread creation failed\r\n");
    }
}

namespace
{
// The value of a JSON literal as text: a string unescaped, anything else verbatim; null (or
// nothing) is no value.
bool JsonValueToText(const char* json, std::string& out)
{
    while (*json == ' ')
        ++json;
    if (!*json || std::strncmp(json, "null", 4) == 0)
        return false;
    if (*json != '"')
    {
        out.assign(json);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\r'))
            out.pop_back();
        return true;
    }
    out.clear();
    unsigned pendingHigh = 0;
    for (const char* p = json + 1; *p && *p != '"'; ++p)
    {
        if (*p != '\\')
        {
            out += *p;
            continue;
        }
        ++p;
        switch (*p)
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u':
        {
            unsigned code = 0;
            for (int i = 1; i <= 4 && p[i]; ++i)
            {
                const char c = p[i];
                code = code * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 0);
            }
            p += 4;
            if (code >= 0xD800 && code < 0xDC00)
            {
                pendingHigh = code;
                break;
            }
            if (code >= 0xDC00 && code < 0xE000 && pendingHigh)
            {
                code = 0x10000 + ((pendingHigh - 0xD800) << 10) + (code - 0xDC00);
                pendingHigh = 0;
            }
            if (code < 0x80)
                out += static_cast<char>(code);
            else if (code < 0x800)
            {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
        }
        case '\0': return true;
        default: out += *p; break; // \" \\ \/
        }
    }
    return true;
}

// A text message from the host: a title line, or the answer to the call in flight.
void HandlePrimaryText(ScreenStream* stream, const char* text)
{
    if (std::strncmp(text, "title ", 6) == 0)
    {
        wchar_t title[1024] = {};
        MultiByteToWideChar(CP_UTF8, 0, text + 6, -1, title, 1024);
        SetElectronTitle(stream, title);
        return;
    }
    const bool ret = std::strncmp(text, "ret ", 4) == 0;
    const bool err = std::strncmp(text, "err ", 4) == 0;
    if (!ret && !err)
        return;
    char* rest = nullptr;
    const DWORD id = static_cast<DWORD>(std::strtoul(text + 4, &rest, 10));
    if (!rest || *rest != ' ')
        return;
    std::string value;
    const bool ok = ret && JsonValueToText(rest + 1, value);
    const bool loading = err && std::strcmp(rest + 1, "loading") == 0;
    if (err && !loading)
    {
        char note[600] = {};
        StringCchPrintfA(note, 600, "primary browser: call failed: %s\r\n", rest + 1);
        LogLine(note);
    }
    EnterCriticalSection(&stream->lock);
    if (id == stream->primaryCallId)
    {
        stream->primaryCallOk = ok;
        stream->primaryCallLoading = loading;
        if (ok)
            StringCchCopyA(stream->primaryCallResult, sizeof(stream->primaryCallResult), value.c_str());
        stream->primaryCallDone = id;
        if (stream->primaryCallEvent)
            SetEvent(stream->primaryCallEvent);
    }
    LeaveCriticalSection(&stream->lock);
}
} // namespace

bool CallPrimary(ScreenStream* stream, const char* script, char* out, size_t outCount, DWORD timeoutMs, bool* loading)
{
    if (loading)
        *loading = false;
    if (!stream || !script || !out || outCount == 0)
        return false;
    out[0] = '\0';
    EnterCriticalSection(&stream->lock);
    HANDLE control = stream->primaryControl;
    HANDLE event = stream->primaryCallEvent;
    if (!stream->primaryActive || !control || !event)
    {
        LeaveCriticalSection(&stream->lock);
        return false;
    }
    const DWORD id = ++stream->primaryCallId;
    stream->primaryCallDone = 0;
    stream->primaryCallOk = false;
    stream->primaryCallLoading = false;
    ResetEvent(event);
    char head[32] = {};
    StringCchPrintfA(head, 32, "call %lu ", static_cast<unsigned long>(id));
    std::string line = head;
    line += script;
    for (char& c : line)
        if (c == '\r' || c == '\n')
            c = ' ';
    line += '\n';
    DWORD written = 0;
    const bool sent = WriteFile(control, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != 0;
    LeaveCriticalSection(&stream->lock);
    if (!sent)
        return false;
    WaitForSingleObject(event, timeoutMs);
    EnterCriticalSection(&stream->lock);
    const bool ok = stream->primaryCallDone == id && stream->primaryCallOk;
    if (ok)
        StringCchCopyA(out, outCount, stream->primaryCallResult);
    if (loading)
        *loading = stream->primaryCallDone == id && stream->primaryCallLoading;
    LeaveCriticalSection(&stream->lock);
    return ok;
}

DWORD WINAPI PrimaryVideoThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    const int width = stream->pageViewWidth;
    const int height = stream->pageViewHeight;
    const int fps = stream->fps > 0 ? stream->fps : kDefaultFps;
    const DWORD pageBytes = static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4;
    wchar_t url[4096] = {};
    EnterCriticalSection(&stream->lock);
    StringCchCopyW(url, 4096, stream->pageUrl);
    const int mute = stream->muted ? 1 : 0;
    const float gain = stream->pageGain > 0 ? stream->pageGain : 1.0f;
    LeaveCriticalSection(&stream->lock);

    HANDLE control = nullptr, video = nullptr, process = nullptr;
    ElectronSessionRequest request;
    request.url = url;
    request.width = width;
    request.height = height;
    request.fps = fps;
    request.mute = mute;
    request.gain = gain;
    request.framed = true;
    request.stop = &stream->primaryStop;
    request.outControl = &control;
    request.outVideo = &video;
    request.outProcess = &process;
    wchar_t message[512] = {};
    if (!StartElectronSession(request, message, 512))
    {
        char line[600] = {};
        WideCharToMultiByte(CP_UTF8, 0, message, -1, line, 580, nullptr, nullptr);
        StringCchCatA(line, 600, "\r\n");
        LogLine(line);
        // Not again on the next draw: a host that cannot start would otherwise be launched
        // once per frame.
        stream->primaryRetryAt = GetTickCount64() + 5000;
        stream->primaryActive = false;
        return 0;
    }
    if (stream->primaryStop)
    {
        // Stopped while the host was starting: nobody else holds these yet.
        if (process)
        {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
        CloseHandleIf(&control);
        CloseHandleIf(&video);
        return 0;
    }

    EnterCriticalSection(&stream->lock);
    stream->primaryProcess = process;
    stream->primaryControl = control;
    stream->primaryVideo = video;
    LeaveCriticalSection(&stream->lock);
    SendPrimaryControl(stream);

    char started[700] = {};
    char urlUtf8[500] = {};
    WideCharToMultiByte(CP_UTF8, 0, url, -1, urlUtf8, 500, nullptr, nullptr);
    StringCchPrintfA(started, 700, "primary browser: %dx%d %s\r\n", width, height, urlUtf8);
    LogLine(started);

    // Framed: an 8-byte header (type, length) before each message. 1 is a frame of the view
    // size, 2 a text line; anything else is skipped by its length. The first message is the
    // app's hello with its protocol number; an app that sends something else is a stale copy
    // writing bare frames, and there is no reading those.
    constexpr DWORD kMaxText = 64 * 1024;
    constexpr int kProtocol = 2;
    BYTE* frame = new BYTE[pageBytes];
    std::string text;
    bool first = true;
    while (!stream->primaryStop)
    {
        BYTE header[8] = {};
        if (!ReadFully(video, header, 8, &stream->primaryStop))
            break;
        DWORD type = 0, length = 0;
        std::memcpy(&type, header, 4);
        std::memcpy(&length, header + 4, 4);
        if (first)
        {
            first = false;
            bool hello = type == 2 && length > 6 && length < kMaxText;
            if (hello)
            {
                text.assign(length, '\0');
                hello = ReadFully(video, reinterpret_cast<BYTE*>(&text[0]), length, &stream->primaryStop) && std::strncmp(text.c_str(), "hello ", 6) == 0;
            }
            int protocol = 0;
            if (hello)
            {
                if (const char* p = std::strstr(text.c_str(), "(protocol "))
                    protocol = std::atoi(p + 10);
                char line[700] = {};
                StringCchPrintfA(line, 700, "primary browser: %s; hook protocol %d\r\n", text.c_str() + 6, kProtocol);
                LogLine(line);
            }
            if (!hello || protocol != kProtocol)
            {
                LogLine(hello ? "primary browser: the aisp.electron app speaks another protocol than this aisp.hook.dll; install the aisp.electron\\app\\main.js that came with the DLL\r\n"
                              : "primary browser: the aisp.electron app sent no hello, it is a stale copy writing bare frames; install the aisp.electron\\app\\main.js that came with this aisp.hook.dll\r\n");
                break;
            }
            continue;
        }
        if (type == 1 && length == pageBytes)
        {
            if (!ReadFully(video, frame, pageBytes, &stream->primaryStop))
                break;
            PushPageFrame(stream, frame, pageBytes);
        }
        else if (type == 2 && length < kMaxText)
        {
            text.assign(length, '\0');
            if (length && !ReadFully(video, reinterpret_cast<BYTE*>(&text[0]), length, &stream->primaryStop))
                break;
            HandlePrimaryText(stream, text.c_str());
        }
        else
        {
            BYTE skip[4096];
            bool ok = true;
            while (length && ok)
            {
                const DWORD chunk = length > sizeof(skip) ? sizeof(skip) : length;
                ok = ReadFully(video, skip, chunk, &stream->primaryStop);
                length -= chunk;
            }
            if (!ok)
                break;
        }
    }
    delete[] frame;
    if (!stream->primaryStop)
        LogLine("primary browser: host ended (see aisp.screen.log)\r\n");
    return 0;
}
} // namespace aisp
