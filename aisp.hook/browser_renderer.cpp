#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>

#include <cstdint>
#include <cwchar>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

namespace
{
constexpr std::uint32_t kFrameMagic = 0x50534941; // "AISP" in little endian.
constexpr int kBrowserWidth = 1024;
constexpr int kBrowserHeight = 1024;
constexpr int kTvLeft = 9;
constexpr int kTvTop = 15;
constexpr int kTvWidth = 486;
constexpr int kTvHeight = 343;

#pragma pack(push, 1)
struct FrameHeader
{
    std::uint32_t magic;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t byteCount;
};
#pragma pack(pop)

CRITICAL_SECTION g_frameLock;
LONG g_initialized = 0;
LONG g_started = 0;
BYTE* g_frame = nullptr;
DWORD g_frameSize = 0;
int g_frameWidth = 0;
int g_frameHeight = 0;
wchar_t g_gameDirectory[MAX_PATH] = {};
wchar_t g_pipeName[128] = {};

void WriteTrace(const wchar_t* text)
{
    wchar_t path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(path, _countof(path), L"%s\\aisp.electron-renderer.log", g_gameDirectory)))
        return;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::wcslen(text) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

bool ResolveGameDirectory()
{
    const DWORD length = GetModuleFileNameW(nullptr, g_gameDirectory, _countof(g_gameDirectory));
    if (!length || length >= _countof(g_gameDirectory))
        return false;

    wchar_t* slash = std::wcsrchr(g_gameDirectory, L'\\');
    if (!slash)
        return false;
    *slash = L'\0';
    return true;
}

bool ReadExact(HANDLE pipe, void* destination, DWORD byteCount)
{
    auto* output = static_cast<BYTE*>(destination);
    while (byteCount)
    {
        DWORD read = 0;
        if (!ReadFile(pipe, output, byteCount, &read, nullptr) || !read)
            return false;
        output += read;
        byteCount -= read;
    }
    return true;
}

bool LaunchElectron()
{
    wchar_t executable[MAX_PATH] = {};
    wchar_t application[MAX_PATH] = {};
    wchar_t commandLine[2048] = {};
    if (
        FAILED(StringCchPrintfW(executable, _countof(executable), L"%s\\aisp.electron\\electron.exe", g_gameDirectory))
        || FAILED(StringCchPrintfW(application, _countof(application), L"%s\\aisp.electron\\app", g_gameDirectory))
        || FAILED(StringCchPrintfW(commandLine, _countof(commandLine), L"\"%s\" \"%s\" \"--aisp-pipe=%s\"", executable, application, g_pipeName))
    )
        return false;

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_gameDirectory, &startup, &process))
    {
        wchar_t line[128] = {};
        StringCchPrintfW(line, _countof(line), L"Electron: CreateProcessW failed (%lu)\r\n", GetLastError());
        WriteTrace(line);
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    WriteTrace(L"Electron: helper launched\r\n");
    return true;
}

DWORD WINAPI ReceiveFrames(void*)
{
    HANDLE pipe = CreateNamedPipeW(
        g_pipeName,
        // Node opens named pipes as duplex sockets. A read-only server makes
        // Node observe EOF on its read side and close its write side before
        // the first paint can arrive.
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        0,
        kTvWidth * kTvHeight * 4 * 2,
        0,
        nullptr
    );
    if (pipe == INVALID_HANDLE_VALUE)
    {
        WriteTrace(L"Electron: unable to create frame pipe\r\n");
        return 0;
    }

    if (!LaunchElectron())
    {
        CloseHandle(pipe);
        return 0;
    }

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        WriteTrace(L"Electron: frame pipe connection failed\r\n");
        CloseHandle(pipe);
        return 0;
    }
    WriteTrace(L"Electron: frame pipe connected\r\n");

    bool firstFrame = true;
    for (;;)
    {
        FrameHeader header = {};
        if (!ReadExact(pipe, &header, sizeof(header)))
            break;

        const std::uint64_t expectedBytes = static_cast<std::uint64_t>(header.stride) * header.height;
        if (
            header.magic != kFrameMagic || !header.width || !header.height || header.width > kBrowserWidth || header.height > kBrowserHeight
            || header.stride != header.width * 4 || header.byteCount != expectedBytes || header.byteCount > kBrowserWidth * kBrowserHeight * 4
        )
        {
            WriteTrace(L"Electron: invalid frame header\r\n");
            break;
        }

        BYTE* nextFrame = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, header.byteCount));
        if (!nextFrame || !ReadExact(pipe, nextFrame, header.byteCount))
        {
            if (nextFrame)
                HeapFree(GetProcessHeap(), 0, nextFrame);
            break;
        }

        EnterCriticalSection(&g_frameLock);
        BYTE* previousFrame = g_frame;
        g_frame = nextFrame;
        g_frameSize = header.byteCount;
        g_frameWidth = static_cast<int>(header.width);
        g_frameHeight = static_cast<int>(header.height);
        LeaveCriticalSection(&g_frameLock);
        if (previousFrame)
            HeapFree(GetProcessHeap(), 0, previousFrame);

        if (firstFrame)
        {
            firstFrame = false;
            WriteTrace(L"Electron: first frame received\r\n");
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    WriteTrace(L"Electron: frame pipe disconnected\r\n");
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) void WINAPI StartBrowserRenderer(LPCWSTR source, int width, int height)
{
    (void)source;
    (void)width;
    (void)height;

    if (InterlockedCompareExchange(&g_initialized, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_frameLock);
        if (!ResolveGameDirectory())
            return;
        StringCchPrintfW(g_pipeName, _countof(g_pipeName), L"\\\\.\\pipe\\aisp-electron-%lu", GetCurrentProcessId());
    }

    if (InterlockedCompareExchange(&g_started, 1, 0) != 0 || !g_gameDirectory[0])
        return;

    HANDLE thread = CreateThread(nullptr, 0, ReceiveFrames, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        WriteTrace(L"Electron: unable to start frame receiver\r\n");
}

extern "C" __declspec(dllexport) bool WINAPI DrawBrowserFrame(HDC destination, const RECT* destinationRect)
{
    if (!destination || !destinationRect || !g_initialized)
        return false;

    EnterCriticalSection(&g_frameLock);
    if (!g_frame || !g_frameSize || !g_frameWidth || !g_frameHeight)
    {
        LeaveCriticalSection(&g_frameLock);
        return false;
    }

    const int destinationWidth = destinationRect->right - destinationRect->left;
    const int destinationHeight = destinationRect->bottom - destinationRect->top;
    if (destinationWidth <= 0 || destinationHeight <= 0)
    {
        LeaveCriticalSection(&g_frameLock);
        return false;
    }

    const int left = destinationRect->left + (kTvLeft * destinationWidth / kBrowserWidth);
    const int top = destinationRect->top + (kTvTop * destinationHeight / kBrowserHeight);
    const int width = kTvWidth * destinationWidth / kBrowserWidth;
    const int height = kTvHeight * destinationHeight / kBrowserHeight;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = g_frameWidth;
    info.bmiHeader.biHeight = -g_frameHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int result = StretchDIBits(destination, left, top, width, height, 0, 0, g_frameWidth, g_frameHeight, g_frame, &info, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_frameLock);
    return result != GDI_ERROR;
}
