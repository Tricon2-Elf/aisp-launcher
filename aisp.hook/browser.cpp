// Primary off-screen Electron for in-game screens. See browser.h.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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
    if (g_logStats)
    {
        char line[1200] = {};
        StringCchPrintfA(line, 1200, "primary title: %ls\r\n", title ? title : L"");
        LogLine(line);
    }
}

// aisp.launch.data\electron\electron.exe ([tools] electron), with the app at resources\app
// (launcher bootstrap) or a sibling app\ (Wine / install-electron-runtime.sh).
bool ResolveElectronPaths(wchar_t* browser, size_t browserCount, wchar_t* appPath, size_t appCount, wchar_t* error, size_t errorCount)
{
    if (!ToolPath(
            L"AISP_ELECTRON",
            L"electron",
            kElectronFallback,
            kElectronFallbackLegacy,
            browser,
            browserCount
        ))
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
    wchar_t dir[MAX_PATH] = {};
    StringCchCopyW(dir, MAX_PATH, appPath);
    StringCchCopyW(appPath, appCount, dir);
    StringCchCatW(appPath, appCount, L"resources\\app");
    if (GetFileAttributesW(appPath) != INVALID_FILE_ATTRIBUTES)
        return true;
    StringCchCopyW(appPath, appCount, dir);
    StringCchCatW(appPath, appCount, L"app");
    if (GetFileAttributesW(appPath) != INVALID_FILE_ATTRIBUTES)
        return true;
    if (error)
        StringCchPrintfW(error, errorCount, L"browser app not found: %sresources\\app", dir);
    return false;
}
} // namespace

bool IsRunningOnWine()
{
    static int cached = -1;
    if (cached < 0)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        cached = ntdll && GetProcAddress(ntdll, "wine_get_version") ? 1 : 0;
    }
    return cached == 1;
}

void InitBrowserMode()
{
    if (g_browserModeReady)
        return;
    // electron or ie; the default is electron everywhere: it costs the game thread a bitmap copy
    // per draw where ieframe rendered the page, and under Wine ieframe stays black anyway.
    // Anything else reads as the default.
    const bool wine = IsRunningOnWine();
    wchar_t value[32] = {};
    ConfigString(L"AISP_PRIMARY_BROWSER", L"screens", L"primary_browser", value, 32);
    if (_wcsicmp(value, L"ie") == 0 || _wcsicmp(value, L"ieframe") == 0)
        g_usePrimaryBrowser = false;
    else
        g_usePrimaryBrowser = true;
    g_browserModeReady = true;
    if (wine)
    {
        // The channels to the native Electron are loopback sockets.
        WSADATA wsa = {};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    char text[160] = {};
    StringCchPrintfA(text, 160, "wine=%d primary-browser=%s", wine ? 1 : 0, g_usePrimaryBrowser ? "electron" : "ie");
    AppendInitLog(text);
    wchar_t line[160] = {};
    StringCchPrintfW(line, 160, L"aisp.hook: wine=%d primary-browser=%s\n", wine ? 1 : 0, g_usePrimaryBrowser ? L"electron" : L"ie");
    OutputDebugStringW(line);
}

bool UsePrimaryBrowser()
{
    if (!g_browserModeReady)
        InitBrowserMode();
    return g_usePrimaryBrowser;
}

bool WriteBrowserChannel(HANDLE handle, bool tcp, const void* data, size_t length)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return false;
    if (tcp)
    {
        const char* p = static_cast<const char*>(data);
        while (length)
        {
            const int n = send(reinterpret_cast<SOCKET>(handle), p, static_cast<int>(length > 65536 ? 65536 : length), 0);
            if (n <= 0)
                return false;
            p += n;
            length -= static_cast<size_t>(n);
        }
        return true;
    }
    DWORD written = 0;
    return WriteFile(handle, data, static_cast<DWORD>(length), &written, nullptr) != 0;
}

void CloseBrowserChannel(HANDLE handle, bool tcp)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return;
    if (tcp)
    {
        SOCKET sock = reinterpret_cast<SOCKET>(handle);
        shutdown(sock, SD_BOTH);
        closesocket(sock);
    }
    else
    {
        CloseHandle(handle);
    }
}

namespace
{
SOCKET ListenLoopback(int* port)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        return INVALID_SOCKET;
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 1) != 0)
    {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    sockaddr_in got = {};
    int gotLen = sizeof(got);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&got), &gotLen) != 0)
    {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    *port = ntohs(got.sin_port);
    return sock;
}

SOCKET AcceptLoopback(SOCKET listenSock, volatile LONG* stop)
{
    while (!stop || !*stop)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval timeout = {};
        timeout.tv_usec = 50000;
        const int ready = select(static_cast<int>(listenSock) + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0)
            continue;
        SOCKET accepted = accept(listenSock, nullptr, nullptr);
        if (accepted != INVALID_SOCKET)
            return accepted;
    }
    return INVALID_SOCKET;
}

bool RequestNativeElectron(const char* line, char* error, size_t errorCount)
{
    wchar_t spec[64] = {};
    // Session state of the Wine runner, not a setting: environment only.
    if (GetEnvironmentVariableW(L"AISP_ELECTRON_NATIVE", spec, 64) == 0 || !spec[0])
        StringCchCopyW(spec, 64, L"127.0.0.1:18764");
    char host[32] = "127.0.0.1";
    int port = 18764;
    char utf[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, spec, -1, utf, 64, nullptr, nullptr);
    if (char* colon = std::strrchr(utf, ':'))
    {
        *colon = 0;
        if (utf[0])
            StringCchCopyA(host, 32, utf);
        port = atoi(colon + 1);
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        StringCchCopyA(error, errorCount, "native electron: socket failed");
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        StringCchCopyA(error, errorCount, "native electron: broker not listening (start aisp.electron/host.js)");
        closesocket(sock);
        return false;
    }
    const int length = static_cast<int>(std::strlen(line));
    if (send(sock, line, length, 0) != length)
    {
        StringCchCopyA(error, errorCount, "native electron: send failed");
        closesocket(sock);
        return false;
    }
    char reply[256] = {};
    int got = 0;
    while (got < static_cast<int>(sizeof(reply) - 1))
    {
        const int n = recv(sock, reply + got, static_cast<int>(sizeof(reply) - 1 - got), 0);
        if (n <= 0)
            break;
        got += n;
        reply[got] = 0;
        if (std::strchr(reply, '\n'))
            break;
    }
    closesocket(sock);
    if (std::strncmp(reply, "ok", 2) != 0)
    {
        StringCchPrintfA(error, errorCount, "native electron: %s", reply[0] ? reply : "empty reply");
        return false;
    }
    return true;
}

// Accepts one connection on a listening loopback socket, or gives up after `waitMs`.
SOCKET AcceptLoopbackFor(SOCKET listenSock, DWORD waitMs)
{
    const ULONGLONG deadline = GetTickCount64() + waitMs;
    while (GetTickCount64() < deadline)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval timeout = {};
        timeout.tv_usec = 50000;
        const int ready = select(static_cast<int>(listenSock) + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0)
            continue;
        SOCKET accepted = accept(listenSock, nullptr, nullptr);
        if (accepted != INVALID_SOCKET)
            return accepted;
    }
    return INVALID_SOCKET;
}

// The one Electron of this game: every screen (the primary pages and the electron: sources) is a
// window in it, opened with a line on the hub channel. Started by the first screen that needs
// it, in the job so it ends with the game (Windows) or when the hub channel closes (Wine, where
// the native broker spawns it). A hub whose process is gone or whose channel breaks is dropped
// and the next screen starts a fresh one.
struct ElectronHub
{
    CRITICAL_SECTION lock;
    bool lockReady = false;
    HANDLE process = nullptr; // Windows: the host process; Wine: none (the broker's child)
    HANDLE control = nullptr; // the hub channel, `open` lines go here
    bool tcp = false;
    LONG nextScreenId = 0;
};
ElectronHub g_hub;

void EnsureHubLock()
{
    if (!g_hub.lockReady)
    {
        InitializeCriticalSection(&g_hub.lock);
        g_hub.lockReady = true;
    }
}

// Caller holds g_hub.lock.
bool HubAlive()
{
    if (!g_hub.control)
        return false;
    if (g_hub.process && WaitForSingleObject(g_hub.process, 0) == WAIT_OBJECT_0)
        return false;
    return true;
}

// Caller holds g_hub.lock. Ends the host (if it is ours and still there) and forgets it.
void DropHub(const char* why)
{
    if (g_hub.control || g_hub.process)
    {
        char note[200] = {};
        StringCchPrintfA(note, 200, "browser host dropped: %s\r\n", why);
        LogLine(note);
    }
    if (g_hub.control)
        CloseBrowserChannel(g_hub.control, g_hub.tcp);
    g_hub.control = nullptr;
    if (g_hub.process)
    {
        if (WaitForSingleObject(g_hub.process, 0) != WAIT_OBJECT_0)
            TerminateProcess(g_hub.process, 0);
        CloseHandle(g_hub.process);
        g_hub.process = nullptr;
    }
    g_hub.tcp = false;
}

// Caller holds g_hub.lock. Starts the host if there is none, and connects its hub channel.
bool EnsureHub(wchar_t* error, size_t errorCount)
{
    if (HubAlive())
        return true;
    DropHub("gone");
    const bool tcp = IsRunningOnWine();
    if (tcp)
    {
        int port = 0;
        SOCKET listenSock = ListenLoopback(&port);
        if (listenSock == INVALID_SOCKET)
        {
            if (error)
                StringCchCopyW(error, errorCount, L"browser: tcp listen failed");
            return false;
        }
        char line[64] = {};
        StringCchPrintfA(line, 64, "hub 127.0.0.1:%d\n", port);
        char nativeError[256] = {};
        if (!RequestNativeElectron(line, nativeError, 256))
        {
            closesocket(listenSock);
            if (error)
                MultiByteToWideChar(CP_UTF8, 0, nativeError, -1, error, static_cast<int>(errorCount));
            return false;
        }
        SOCKET hub = AcceptLoopbackFor(listenSock, 15000);
        closesocket(listenSock);
        if (hub == INVALID_SOCKET)
        {
            if (error)
                StringCchCopyW(error, errorCount, L"browser: the host did not connect its hub channel");
            return false;
        }
        g_hub.control = reinterpret_cast<HANDLE>(hub);
        g_hub.tcp = true;
        g_hub.process = nullptr;
    }
    else
    {
        wchar_t browser[MAX_PATH] = {}, appPath[MAX_PATH] = {};
        if (!ResolveElectronPaths(browser, MAX_PATH, appPath, MAX_PATH, error, errorCount))
            return false;
        wchar_t hubSpec[128] = {};
        HANDLE hubListen = CreateNamedPipePair(hubSpec, 128, L"hub");
        if (hubListen == INVALID_HANDLE_VALUE)
        {
            if (error)
                StringCchCopyW(error, errorCount, L"browser: pipe creation failed");
            return false;
        }
        wchar_t command[1024] = {};
        // Chromium switches must come before the app path or Electron treats them as argv for main.js.
        StringCchPrintfW(
            command,
            1024,
            L"\"%s\" --disable-gpu --disable-gpu-compositing --disable-gpu-sandbox \"%s\" --hub=\"%s\"",
            browser,
            appPath,
            hubSpec
        );
        HANDLE process = LaunchBrowserHost(command);
        if (!process)
        {
            CloseHandle(hubListen);
            if (error)
                StringCchCopyW(error, errorCount, L"browser host failed to start (see aisp.screen.log)");
            return false;
        }
        // Electron's own start: allow it the full budget (160 tries of 50 ms). A host that ends
        // meanwhile is the usual sign of an app older than this DLL (it wants --url, not --hub).
        bool connected = false;
        for (int i = 0; i < 160 && !connected; ++i)
        {
            if (ConnectNamedPipe(hubListen, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
            {
                DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
                SetNamedPipeHandleState(hubListen, &mode, nullptr, nullptr);
                connected = true;
                break;
            }
            if (WaitForSingleObject(process, 50) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                GetExitCodeProcess(process, &code);
                if (error)
                    StringCchPrintfW(error, errorCount, L"browser host exited (code %lu) before connecting its hub channel: the Electron app (resources\\app or app) must be the one that came with this aisp.hook.dll", static_cast<unsigned long>(code));
                break;
            }
        }
        if (!connected)
        {
            if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
                TerminateProcess(process, 0);
            CloseHandle(process);
            CloseHandle(hubListen);
            if (error && !error[0])
                StringCchCopyW(error, errorCount, L"browser: the host did not connect its hub channel");
            return false;
        }
        g_hub.control = hubListen;
        g_hub.tcp = false;
        g_hub.process = process;
    }
    LogLine("browser host started: one Electron for every screen\r\n");
    return true;
}

// Caller holds g_hub.lock.
bool HubSend(const char* line)
{
    return WriteBrowserChannel(g_hub.control, g_hub.tcp, line, std::strlen(line));
}
} // namespace

bool StartElectronSession(const ElectronSessionRequest& request, wchar_t* error, size_t errorCount)
{
    if (!request.url || !request.outControl || !request.outVideo)
    {
        if (error)
            StringCchCopyW(error, errorCount, L"browser: bad session request");
        return false;
    }
    const bool tcp = IsRunningOnWine();
    if (request.outTcp)
        *request.outTcp = tcp;
    *request.outControl = nullptr;
    *request.outVideo = nullptr;
    // The host is shared (see ElectronHub); no screen owns a process.
    if (request.outProcess)
        *request.outProcess = nullptr;

    // This screen's own channels, listening before the host is told about them.
    wchar_t controlSpec[128] = {}, videoSpec[128] = {};
    HANDLE controlListen = INVALID_HANDLE_VALUE, videoListen = INVALID_HANDLE_VALUE;
    if (tcp)
    {
        int controlPort = 0, videoPort = 0;
        SOCKET controlSock = ListenLoopback(&controlPort);
        SOCKET videoSock = ListenLoopback(&videoPort);
        if (controlSock == INVALID_SOCKET || videoSock == INVALID_SOCKET)
        {
            if (controlSock != INVALID_SOCKET)
                closesocket(controlSock);
            if (videoSock != INVALID_SOCKET)
                closesocket(videoSock);
            if (error)
                StringCchCopyW(error, errorCount, L"browser: tcp listen failed");
            return false;
        }
        StringCchPrintfW(controlSpec, 128, L"127.0.0.1:%d", controlPort);
        StringCchPrintfW(videoSpec, 128, L"127.0.0.1:%d", videoPort);
        controlListen = reinterpret_cast<HANDLE>(controlSock);
        videoListen = reinterpret_cast<HANDLE>(videoSock);
    }
    else
    {
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
    }

    // The open line, to the shared host: the same words on both platforms (the channel names
    // tell them apart), the url last since it may hold anything.
    char urlUtf8[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, request.url, -1, urlUtf8, 4096, nullptr, nullptr);
    // run=<url> rides along as one word (a URL has no spaces); the page url stays last.
    char runWord[1100] = {};
    if (request.run && request.run[0] && !std::wcschr(request.run, L' '))
    {
        char runUtf8[1040] = {};
        WideCharToMultiByte(CP_UTF8, 0, request.run, -1, runUtf8, 1040, nullptr, nullptr);
        StringCchPrintfA(runWord, 1100, " run=%s", runUtf8);
    }
    char open[6100] = {};
    StringCchPrintfA(
        open,
        6100,
        "open id=%ld width=%d height=%d fps=%d control=%ls video=%ls framed=%d scrollx=%d scrolly=%d hide=%d scale=%.4f mute=%d gain=%.4f%s url=%s\n",
        InterlockedIncrement(&g_hub.nextScreenId),
        request.width,
        request.height,
        request.fps > 0 ? request.fps : kDefaultFps,
        controlSpec,
        videoSpec,
        request.framed ? 1 : 0,
        request.scrollx,
        request.scrolly,
        request.hideScroll,
        request.scale > 0 ? request.scale : 1.0f,
        request.mute,
        request.gain,
        runWord,
        urlUtf8
    );
    EnsureHubLock();
    EnterCriticalSection(&g_hub.lock);
    bool opened = EnsureHub(error, errorCount) && HubSend(open);
    if (!opened && g_hub.control)
    {
        // The channel broke under us: the host is gone; once more with a fresh one.
        DropHub("hub channel write failed");
        opened = EnsureHub(error, errorCount) && HubSend(open);
    }
    LeaveCriticalSection(&g_hub.lock);
    if (!opened)
    {
        CloseBrowserChannel(controlListen, tcp);
        CloseBrowserChannel(videoListen, tcp);
        if (error && !error[0])
            StringCchCopyW(error, errorCount, L"browser: the host did not take the open");
        return false;
    }

    HANDLE control = nullptr, video = nullptr;
    if (tcp)
    {
        SOCKET c = AcceptLoopback(reinterpret_cast<SOCKET>(controlListen), request.stop);
        SOCKET v = AcceptLoopback(reinterpret_cast<SOCKET>(videoListen), request.stop);
        CloseBrowserChannel(controlListen, true);
        CloseBrowserChannel(videoListen, true);
        if (c == INVALID_SOCKET || v == INVALID_SOCKET)
        {
            if (c != INVALID_SOCKET)
                closesocket(c);
            if (v != INVALID_SOCKET)
                closesocket(v);
            if (error)
                StringCchCopyW(error, errorCount, L"browser: tcp accept failed");
            return false;
        }
        control = reinterpret_cast<HANDLE>(c);
        video = reinterpret_cast<HANDLE>(v);
    }
    else
    {
        // The video pipe first, with the whole budget: the host connects both pipes at once,
        // so once that one is in the other needs a moment at most.
        if (!ConnectNamedPipeWait(videoListen, request.stop))
        {
            if (error)
                StringCchCopyW(error, errorCount, L"browser: video pipe connect failed");
            CloseHandle(controlListen);
            CloseHandle(videoListen);
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
    if (mute != stream->sentPrimaryMute)
    {
        char line[32] = {};
        StringCchPrintfA(line, 32, "mute %d\n", mute);
        WriteBrowserChannel(stream->primaryControl, stream->primaryTcp, line, std::strlen(line));
        stream->sentPrimaryMute = mute;
    }
    // Under the clear colour nothing of the page is shown, so it need not be painted either;
    // the page itself keeps running (title, the client's reads).
    const int paint = stream->pageClear ? 0 : 1;
    if (paint != stream->sentPrimaryPaint)
    {
        char line[32] = {};
        StringCchPrintfA(line, 32, "paint %d\n", paint);
        WriteBrowserChannel(stream->primaryControl, stream->primaryTcp, line, std::strlen(line));
        stream->sentPrimaryPaint = paint;
    }
    const float sent = stream->sentPrimaryGain;
    if (sent >= 0.0f && gain > sent - 0.002f && gain < sent + 0.002f)
        return;
    char line[32] = {};
    StringCchPrintfA(line, 32, "gain %.4f\n", gain);
    WriteBrowserChannel(stream->primaryControl, stream->primaryTcp, line, std::strlen(line));
    stream->sentPrimaryGain = gain;
}

void SendPrimaryEval(ScreenStream* stream, const char* utf8Script)
{
    if (!stream || !stream->primaryControl || !utf8Script || !utf8Script[0])
        return;
    std::string line("eval ");
    line += utf8Script;
    for (char& c : line)
        if (c == '\r' || c == '\n')
            c = ' ';
    line += '\n';
    WriteBrowserChannel(stream->primaryControl, stream->primaryTcp, line.data(), line.size());
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
    WriteBrowserChannel(stream->primaryControl, stream->primaryTcp, line, std::strlen(line));
    EnterCriticalSection(&stream->lock);
    ++stream->primaryEvals;
    LeaveCriticalSection(&stream->lock);
}

void CountPrimaryRead(ScreenStream* stream, const wchar_t* id, double waitMs, bool ok, bool loading)
{
    EnterCriticalSection(&stream->lock);
    if (std::wcscmp(id, L"statusForm") == 0)
        ++stream->primaryReadsStatus;
    else
        ++stream->primaryReadsOther;
    if (!ok)
        ++stream->primaryReadsFailed;
    if (loading)
        ++stream->primaryReadsLoading;
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
    StringCchPrintfA(line, 400, "primary stats: %.1f s: %lu reads (%lu statusForm, %lu other, %lu failed, %lu loading), %lu evals, waited %.1f ms total, %.2f ms avg, %.1f ms max, %.2f%% of the interval\r\n",
                     elapsed / 1000.0, static_cast<unsigned long>(reads), static_cast<unsigned long>(stream->primaryReadsStatus), static_cast<unsigned long>(stream->primaryReadsOther),
                     static_cast<unsigned long>(stream->primaryReadsFailed), static_cast<unsigned long>(stream->primaryReadsLoading), static_cast<unsigned long>(stream->primaryEvals), stream->primaryWaitMs, reads ? stream->primaryWaitMs / reads : 0.0,
                     stream->primaryWaitMaxMs, stream->primaryWaitMs / elapsed * 100.0);
    stream->primaryReadsStatus = stream->primaryReadsOther = stream->primaryEvals = stream->primaryReadsFailed = stream->primaryReadsLoading = 0;
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
    bool tcp = false;
    EnterCriticalSection(&stream->lock);
    process = stream->primaryProcess;
    stream->primaryProcess = nullptr;
    control = stream->primaryControl;
    stream->primaryControl = nullptr;
    video = stream->primaryVideo;
    stream->primaryVideo = nullptr;
    videoThread = stream->primaryThread;
    stream->primaryThread = nullptr;
    tcp = stream->primaryTcp;
    stream->primaryTcp = false;
    stream->pageReady = false;
    stream->sentPrimaryMute = -1;
    stream->sentPrimaryGain = -1.0f;
    stream->sentPrimaryPaint = -1;
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
    // and the handles are closed last. A socket is the other way round: there is no host
    // process to end, and shutdown releases a blocked recv.
    if (process)
    {
        TerminateProcess(process, 0);
        CloseHandle(process);
    }
    if (tcp)
    {
        CloseBrowserChannel(control, true);
        CloseBrowserChannel(video, true);
    }
    JoinPrimaryThread(videoThread, "video");
    if (!tcp)
    {
        CloseHandleIf(&control);
        CloseHandleIf(&video);
    }
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
    stream->sentPrimaryPaint = -1;
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
    if (std::strncmp(text, "failed ", 7) == 0)
    {
        // The page did not come: it has nothing to say, so what the last page had playing stops
        // rather than staying up under an error page.
        char note[600] = {};
        StringCchPrintfA(note, 600, "primary browser: load failed: %s\r\n", text + 7);
        LogLine(note);
        SetElectronTitle(stream, L"aisp:");
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

bool ReadFramedChannel(HANDLE video, volatile LONG* stop, DWORD frameBytes, const char* tag, const FramedSink& sink)
{
    constexpr DWORD kMaxText = 64 * 1024;
    constexpr int kProtocol = 2;
    BYTE* frame = new BYTE[frameBytes];
    std::string text;
    bool first = true;
    bool accepted = false;
    while (!*stop)
    {
        BYTE header[8] = {};
        if (!ReadFully(video, header, 8, stop))
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
                hello = ReadFully(video, reinterpret_cast<BYTE*>(&text[0]), length, stop) && std::strncmp(text.c_str(), "hello ", 6) == 0;
            }
            int protocol = 0;
            if (hello)
            {
                if (const char* p = std::strstr(text.c_str(), "(protocol "))
                    protocol = std::atoi(p + 10);
                char line[700] = {};
                StringCchPrintfA(line, 700, "%s: %s; hook protocol %d\r\n", tag, text.c_str() + 6, kProtocol);
                LogLine(line);
            }
            if (!hello || protocol != kProtocol)
            {
                char line[400] = {};
                StringCchPrintfA(line, 400, hello ? "%s: the Electron app speaks another protocol than this aisp.hook.dll; install the resources\\app (or app) main.js that came with the DLL\r\n"
                                                  : "%s: the Electron app sent no hello, it is a stale copy writing bare frames; install the resources\\app (or app) main.js that came with this aisp.hook.dll\r\n", tag);
                LogLine(line);
                break;
            }
            accepted = true;
            continue;
        }
        if (type == 1 && length == frameBytes)
        {
            if (!ReadFully(video, frame, frameBytes, stop))
                break;
            if (sink.onFrame)
                sink.onFrame(sink.context, frame, frameBytes);
        }
        else if (type == 2 && length < kMaxText)
        {
            text.assign(length, '\0');
            if (length && !ReadFully(video, reinterpret_cast<BYTE*>(&text[0]), length, stop))
                break;
            if (sink.onText)
                sink.onText(sink.context, text.c_str());
        }
        else
        {
            BYTE skip[4096];
            bool ok = true;
            while (length && ok)
            {
                const DWORD chunk = length > sizeof(skip) ? sizeof(skip) : length;
                ok = ReadFully(video, skip, chunk, stop);
                length -= chunk;
            }
            if (!ok)
                break;
        }
    }
    delete[] frame;
    return accepted;
}

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
    const bool sent = WriteBrowserChannel(control, stream->primaryTcp, line.data(), line.size());
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
    bool tcp = false;
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
    request.outTcp = &tcp;
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
        CloseBrowserChannel(control, tcp);
        CloseBrowserChannel(video, tcp);
        return 0;
    }

    EnterCriticalSection(&stream->lock);
    stream->primaryProcess = process;
    stream->primaryControl = control;
    stream->primaryVideo = video;
    stream->primaryTcp = tcp;
    LeaveCriticalSection(&stream->lock);
    SendPrimaryControl(stream);

    char started[700] = {};
    char urlUtf8[500] = {};
    WideCharToMultiByte(CP_UTF8, 0, url, -1, urlUtf8, 500, nullptr, nullptr);
    StringCchPrintfA(started, 700, "primary browser: %dx%d %s\r\n", width, height, urlUtf8);
    LogLine(started);

    FramedSink sink;
    sink.context = stream;
    sink.onFrame = [](void* context, const BYTE* frame, DWORD bytes) { PushPageFrame(static_cast<ScreenStream*>(context), frame, bytes); };
    sink.onText = [](void* context, const char* text) { HandlePrimaryText(static_cast<ScreenStream*>(context), text); };
    ReadFramedChannel(video, &stream->primaryStop, pageBytes, "primary browser", sink);
    if (!stream->primaryStop)
        LogLine("primary browser: host ended (see aisp.screen.log)\r\n");
    return 0;
}
} // namespace aisp
