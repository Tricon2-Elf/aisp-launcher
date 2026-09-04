#define WIN32_LEAN_AND_MEAN
// C-style COM vtables: the screen hook patches IWebBrowser2's vtable in place and needs the
// slot layout from the SDK header rather than hand-counted offsets.
#define CINTERFACE
#include <windows.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <exdisp.h>
#include <mshtml.h>
#include <oleauto.h>
#include <servprov.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <cstring>
#include <cwchar>

#include "screen.h"
#include "source.h"

namespace aisp
{
constexpr UINT kJapaneseCodePage = 932;
constexpr LCID kJapaneseLcid = 0x0411;
constexpr LANGID kJapaneseLangId = 0x0411;

using GetACP_t = UINT(WINAPI*)();
using GetOEMCP_t = UINT(WINAPI*)();
using GetThreadLocale_t = LCID(WINAPI*)();
using GetUserDefaultLCID_t = LCID(WINAPI*)();
using GetSystemDefaultLCID_t = LCID(WINAPI*)();
using GetUserDefaultLangID_t = LANGID(WINAPI*)();
using GetSystemDefaultLangID_t = LANGID(WINAPI*)();
using MultiByteToWideChar_t = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
using WideCharToMultiByte_t = int(WINAPI*)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);

GetACP_t g_originalGetACP = nullptr;
GetOEMCP_t g_originalGetOEMCP = nullptr;
GetThreadLocale_t g_originalGetThreadLocale = nullptr;
GetUserDefaultLCID_t g_originalGetUserDefaultLCID = nullptr;
GetSystemDefaultLCID_t g_originalGetSystemDefaultLCID = nullptr;
GetUserDefaultLangID_t g_originalGetUserDefaultLangID = nullptr;
GetSystemDefaultLangID_t g_originalGetSystemDefaultLangID = nullptr;
MultiByteToWideChar_t g_originalMultiByteToWideChar = nullptr;
WideCharToMultiByte_t g_originalWideCharToMultiByte = nullptr;
LoadLibraryW_t g_originalLoadLibraryW = nullptr;

UINT WINAPI HookGetACP()
{
    return kJapaneseCodePage;
}

UINT WINAPI HookGetOEMCP()
{
    return kJapaneseCodePage;
}

LCID WINAPI HookGetThreadLocale()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetUserDefaultLCID()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetSystemDefaultLCID()
{
    return kJapaneseLcid;
}

LANGID WINAPI HookGetUserDefaultLangID()
{
    return kJapaneseLangId;
}

LANGID WINAPI HookGetSystemDefaultLangID()
{
    return kJapaneseLangId;
}

int WINAPI HookMultiByteToWideChar(UINT codePage, DWORD dwFlags, LPCCH multiByteStr, int cbMultiByte, LPWSTR wideCharStr, int cchWideChar)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalMultiByteToWideChar ? g_originalMultiByteToWideChar(codePage, dwFlags, multiByteStr, cbMultiByte, wideCharStr, cchWideChar) : 0;
}

int WINAPI HookWideCharToMultiByte(
    UINT codePage,
    DWORD dwFlags,
    LPCWCH wideCharStr,
    int cchWideChar,
    LPSTR multiByteStr,
    int cbMultiByte,
    LPCCH defaultChar,
    LPBOOL usedDefaultChar
)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalWideCharToMultiByte
               ? g_originalWideCharToMultiByte(codePage, dwFlags, wideCharStr, cchWideChar, multiByteStr, cbMultiByte, defaultChar, usedDefaultChar)
               : 0;
}

bool IsD3d9LibraryPath(LPCWSTR path)
{
    if (!path || !*path)
        return false;

    const wchar_t* fileName = path;
    if (const wchar_t* slash = std::wcsrchr(path, L'\\'))
        fileName = slash + 1;
    if (const wchar_t* slash = std::wcsrchr(fileName, L'/'))
        fileName = slash + 1;

    return _wcsicmp(fileName, L"d3d9.dll") == 0 || _wcsicmp(fileName, L"d3d9") == 0;
}

bool BuildLocalD3d9Path(wchar_t* outPath, size_t outPathCount)
{
    if (!outPath || outPathCount == 0)
        return false;

    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    if (FAILED(StringCchCopyW(outPath, outPathCount, processPath)))
        return false;
    if (FAILED(StringCchCatW(outPath, outPathCount, L"d3d9.dll")))
        return false;

    return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!g_originalLoadLibraryW)
        return nullptr;

    if (IsD3d9LibraryPath(lpLibFileName))
    {
        wchar_t localD3d9Path[MAX_PATH] = {};
        if (BuildLocalD3d9Path(localD3d9Path, sizeof(localD3d9Path) / sizeof(localD3d9Path[0])))
            return g_originalLoadLibraryW(localD3d9Path);
    }

    return g_originalLoadLibraryW(lpLibFileName);
}

// ---------------------------------------------------------------------------------------------
// In-game screens.
//
// Every in-game display (room TVs, channel screens, the Nico Live billboard) is an IE WebBrowser
// control hosted by the client's statically linked ATL AxHost. The host creates it with
// CoCreateInstance(CLSID_WebBrowser) and calls IWebBrowser2::Navigate exactly once with a URL
// built from the client's own templates:
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=<id>       a TV given a movie id
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?tvid=<n>&chid=<n>  a channel screen
//   http://live.nicovideo.jp/watch/<id>?npwarn=false#player               the live billboard
// Neither host serves the game any more, so Navigate is patched to send them to the emulator,
// keeping the distinction:
//   <base>room-tv?movieid=<id>         <base>channel-screen?tvid=<n>&chid=<n>
//   <base>live-watch?liveid=<id>       <base>screen?url=<anything else on aisp.jp>
// <base> is http://<download host>/<directory of the download path>/ from connection.txt
// (lines 4 and 5, written by the launcher), e.g. http://host/ai-sp/ or http://host/ai-sp/dev/,
// or the AISP_SCREEN_BASE environment variable verbatim. javascript: navigations pass through.
//
// The client draws a screen by calling OleDraw on the browser's document every frame and
// copying a fixed rectangle out of the result. That import is hooked too: when the page names a
// stream source in its title, the page is left idle and the pixels come from a source instead
// (source.h: streamlink/ffmpeg for streams, yt-dlp for videos, the built-in test pattern; the
// server translates its own ids such as tw: and lv… into those). A source fills a frame ring and a sample ring (screen.h); the audio device is the
// clock and the presenter shows the frame matching the samples played. Volume and mute come
// from the page, which publishes "aisp:vol=<0-100>;mute=<0|1>" in its title when the client
// calls its ext_setVolume / ext_setMute script functions. Child processes are attached to a
// job so they die with the game; stderr of every tool goes to aisp.screen.log next to the game
// executable.
// ---------------------------------------------------------------------------------------------

using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using OleDraw_t = HRESULT(WINAPI*)(LPUNKNOWN, DWORD, HDC, LPCRECT);
using Navigate_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using Navigate2_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, VARIANT*, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using OleClose_t = HRESULT(STDMETHODCALLTYPE*)(IOleObject*, DWORD);

CoCreateInstance_t g_originalCoCreateInstance = nullptr;
OleDraw_t g_originalOleDraw = nullptr;
Navigate_t g_originalNavigate = nullptr;
Navigate2_t g_originalNavigate2 = nullptr;
OleClose_t g_originalOleClose = nullptr;
bool g_webBrowserPatched = false;
wchar_t g_screenBase[1024] = {};

const GUID kClsidWebBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
const GUID kIidWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};
const GUID kIidOleObject = {0x00000112, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidHtmlDocument2 = {0x332C4425, 0x26CB, 0x11D0, {0xB4, 0x83, 0x00, 0xC0, 0x4F, 0xD9, 0x01, 0x19}};
const GUID kIidServiceProvider = {0x6D5140C1, 0x7436, 0x11CE, {0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA}};
const GUID kSidWebBrowserApp = {0x0002DF05, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// The dashed form is the one in mmdeviceapi.h / audioclient.h; check a literal against it
// before trusting it. A wrong IID here is an E_NOINTERFACE at runtime and nothing else.
const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}}; // bcde0395-e52f-467c-8e3d-c4579291692e
const GUID kIidMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};   // a95664d2-9614-4f35-a746-de8db63617e6
const GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};          // 1cb9ad4c-dbfa-4c32-b178-c2f568a703b2
const GUID kIidAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};    // f294acfc-3146-4483-a7bf-addca7c260e2

constexpr wchar_t kTvHost[] = L"http://aisp.jp";
constexpr wchar_t kLiveHost[] = L"http://live.nicovideo.jp/watch/";

bool ReadConnectionValue(char lineNumber, wchar_t* out, size_t outCount)
{
    wchar_t path[MAX_PATH] = {};
    if (!BuildGameFilePath(L"connection.txt", path, MAX_PATH))
        return false;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    char buffer[4096] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;
    buffer[read] = '\0';

    const char* line = buffer;
    while (line && *line)
    {
        const char* end = std::strchr(line, '\n');
        if (line[0] == lineNumber && line[1] == ',')
        {
            const char* value = line + 2;
            const char* comma = std::strchr(value, ',');
            const char* stop = comma ? comma : (end ? end : value + std::strlen(value));
            while (stop > value && (stop[-1] == '\r' || stop[-1] == ' '))
                --stop;
            const int length = static_cast<int>(stop - value);
            if (length <= 0 || static_cast<size_t>(length) >= outCount)
                return false;
            const int converted = MultiByteToWideChar(CP_ACP, 0, value, length, out, static_cast<int>(outCount) - 1);
            if (converted <= 0)
                return false;
            out[converted] = L'\0';
            return true;
        }
        line = end ? end + 1 : nullptr;
    }
    return false;
}

void InitScreenBase()
{
    if (GetEnvironmentVariableW(L"AISP_SCREEN_BASE", g_screenBase, 1024) > 0 && g_screenBase[0])
    {
        DebugLog(L"aisp.hook: screen base from environment: %s\n", g_screenBase);
        return;
    }

    wchar_t host[512] = {}, downloadPath[512] = {};
    if (!ReadConnectionValue('4', host, 512))
    {
        g_screenBase[0] = L'\0';
        OutputDebugStringW(L"aisp.hook: no download host in connection.txt; screens are not redirected\n");
        return;
    }
    // The download path names the environment's endpoint directory: ai-sp/download.php or
    // ai-sp/dev/download.php. The screen pages live next to it.
    if (!ReadConnectionValue('5', downloadPath, 512))
        StringCchCopyW(downloadPath, 512, L"ai-sp/download.php");
    if (wchar_t* slash = std::wcsrchr(downloadPath, L'/'))
        *slash = L'\0';
    else
        StringCchCopyW(downloadPath, 512, L"ai-sp");
    while (downloadPath[0] == L'/')
        std::wmemmove(downloadPath, downloadPath + 1, std::wcslen(downloadPath));

    if (FAILED(StringCchPrintfW(g_screenBase, 1024, L"http://%s/%s/", host, downloadPath)))
        g_screenBase[0] = L'\0';
    DebugLog(L"aisp.hook: screen base: %s\n", g_screenBase);
}

bool AppendW(wchar_t* buffer, size_t count, const wchar_t* text)
{
    return SUCCEEDED(StringCchCatW(buffer, count, text));
}

bool AppendPercentEncoded(wchar_t* buffer, size_t count, const wchar_t* text)
{
    for (const wchar_t* p = text; *p; ++p)
    {
        const wchar_t c = *p;
        const bool safe = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' || c == L'_' || c == L'~';
        wchar_t piece[16] = {};
        if (safe)
        {
            piece[0] = c;
        }
        else if (c < 0x80)
        {
            StringCchPrintfW(piece, 16, L"%%%02X", static_cast<unsigned>(c));
        }
        else
        {
            char utf8[8] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, 8, nullptr, nullptr);
            wchar_t* out = piece;
            for (int i = 0; i < n; ++i)
                out += swprintf(out, 4, L"%%%02X", static_cast<unsigned char>(utf8[i]));
        }
        if (!AppendW(buffer, count, piece))
            return false;
    }
    return true;
}

// The client keeps the current channel and map in two globals written by its notify_change_map
// handler ([0xA5DAE0] channel, [0xA5DAE4] map; verified in the binary, which loads unrelocated
// at 0x400000). The URL a screen asks for only carries the screen's own ids, so these tell the
// server which map's screens a page is for.
bool ReadClientMapContext(DWORD* mapId, DWORD* channelId)
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(0xA5DAE0), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS)
        return false;
    *channelId = *reinterpret_cast<volatile DWORD*>(0xA5DAE0);
    *mapId = *reinterpret_cast<volatile DWORD*>(0xA5DAE4);
    return true;
}

// Appends map=<id>&ch=<id> to a screen URL, ahead of any #fragment.
void AppendClientContext(wchar_t* url, size_t count)
{
    DWORD mapId = 0, channelId = 0;
    if (!ReadClientMapContext(&mapId, &channelId))
        return;
    wchar_t fragment[512] = {};
    if (wchar_t* hash = std::wcschr(url, L'#'))
    {
        StringCchCopyW(fragment, 512, hash);
        *hash = L'\0';
    }
    wchar_t context[64] = {};
    StringCchPrintfW(context, 64, L"%smap=%lu&ch=%lu", std::wcschr(url, L'?') ? L"&" : L"?", mapId, channelId);
    AppendW(url, count, context);
    AppendW(url, count, fragment);
}

// Returns a new BSTR with the emulator URL for a screen URL, or nullptr for anything else.
BSTR RewriteScreenUrl(const wchar_t* url)
{
    if (!url || !g_screenBase[0])
        return nullptr;

    wchar_t rewritten[4096] = {};
    StringCchCopyW(rewritten, 4096, g_screenBase);

    const size_t tvHostLength = std::wcslen(kTvHost);
    const size_t liveHostLength = std::wcslen(kLiveHost);
    if (_wcsnicmp(url, kTvHost, tvHostLength) == 0 && (url[tvHostLength] == L'/' || url[tvHostLength] == L'?' || url[tvHostLength] == L'#' || url[tvHostLength] == L'\0'))
    {
        const wchar_t* query = std::wcschr(url, L'?');
        const wchar_t* fragment = std::wcschr(url, L'#');
        const wchar_t* tail = query ? query + 1 : (fragment ? fragment : L"");
        if (query && std::wcsstr(tail, L"movieid="))
        {
            AppendW(rewritten, 4096, L"room-tv?");
            AppendW(rewritten, 4096, tail);
        }
        else if (query && std::wcsstr(tail, L"tvid="))
        {
            AppendW(rewritten, 4096, L"channel-screen?");
            AppendW(rewritten, 4096, tail);
        }
        else
        {
            AppendW(rewritten, 4096, L"screen?url=");
            AppendPercentEncoded(rewritten, 4096, url);
        }
    }
    else if (_wcsnicmp(url, kLiveHost, liveHostLength) == 0)
    {
        const wchar_t* id = url + liveHostLength;
        size_t idLength = 0;
        while (id[idLength] && id[idLength] != L'?' && id[idLength] != L'#' && id[idLength] != L'/')
            ++idLength;
        AppendW(rewritten, 4096, L"live-watch?liveid=");
        wchar_t idCopy[256] = {};
        StringCchCopyNW(idCopy, 256, id, idLength);
        AppendPercentEncoded(rewritten, 4096, idCopy);
        const wchar_t* rest = id + idLength;
        if (*rest == L'?')
        {
            AppendW(rewritten, 4096, L"&");
            AppendW(rewritten, 4096, rest + 1);
        }
        else if (*rest)
        {
            AppendW(rewritten, 4096, rest);
        }
    }
    else
    {
        return nullptr;
    }

    AppendClientContext(rewritten, 4096);
    DebugLog(L"aisp.hook: screen navigate -> %s\n", rewritten);
    return SysAllocString(rewritten);
}

void StopSession(ScreenStream* stream);
void FreeRings(ScreenStream* stream);
void LogStats(ScreenStream* stream)
{
    char line[512] = {};
    EnterCriticalSection(&stream->lock);
    const LONGLONG queuedFrames = stream->videoWritten - stream->videoPos;
    const LONGLONG queuedSamples = stream->audioWritten - stream->audioPos;
    const int queuedAudioMs = stream->sampleRate ? static_cast<int>(queuedSamples * 1000 / stream->sampleRate) : -1;
    const LONGLONG avDriftMs = (stream->audioActive && stream->samplesPerFrame) ? (stream->audioPlayed / stream->samplesPerFrame - (stream->videoPos - 1)) * 1000 / stream->fps : 0;
    StringCchPrintfA(
        line,
        512,
        "stats %lu: vq=%lld aq=%dms playing=%d audio=%d vpos=%lld vwritten=%lld aplayed=%lld drift=%lldms underruns=%ld vwaits=%ld vdrops=%ld awaits=%ld adrops=%ld held=%d dgain=%.2f pan=%.2f/%.2f\r\n",
        static_cast<unsigned long>(GetTickCount64() / 1000),
        queuedFrames,
        queuedAudioMs,
        stream->playing ? 1 : 0,
        stream->audioActive ? 1 : 0,
        stream->videoPos,
        stream->videoWritten,
        stream->audioPlayed,
        avDriftMs,
        stream->underruns,
        stream->videoWaits,
        stream->videoDrops,
        stream->audioWaits,
        stream->audioDrops,
        (GetTickCount64() - stream->lastDraw > kAudioHoldMs) ? 1 : 0,
        stream->distanceGain,
        stream->panLeft,
        stream->panRight
    );
    LeaveCriticalSection(&stream->lock);
    LogLine(line);
}

// Tears down sessions nobody draws any more (TV switched off, room left, game minimised).
DWORD WINAPI WatchdogThread(LPVOID)
{
    int ticks = 0;
    for (;;)
    {
        Sleep(500);
        const ULONGLONG now = GetTickCount64();
        const bool logNow = (++ticks % 2) == 0;
        EnterCriticalSection(&g_streamsLock);
        for (ScreenStream* stream = g_streams; stream; stream = stream->next)
        {
            if (logNow && g_logStats && stream->sessionActive)
                LogStats(stream);
            if (stream->sessionActive && stream->videoWritten == 0 && ticks % 30 == 0)
            {
                char note[600] = {};
                StringCchPrintfA(note, 600, "session %ls: %lu s without a frame (%ls)\r\n", stream->source, static_cast<unsigned long>((now - stream->sessionStarted) / 1000), stream->status);
                LogLine(note);
            }
            const ULONGLONG idle = now - stream->lastDraw;
            if (stream->sessionActive && idle > kIdleStopMs)
            {
                StopSession(stream);
                DebugLog(L"aisp.hook: screen idle, stream stopped: %s\n", stream->source);
            }
            if (!stream->sessionActive && stream->ring && idle > kIdleFreeMs)
                FreeRings(stream);
        }
        LeaveCriticalSection(&g_streamsLock);
    }
    return 0;
}
void InitScreenVideo()
{
    if (g_screenVideoInitialised)
        return;
    g_screenVideoInitialised = true;
    InitializeCriticalSection(&g_streamsLock);
    g_watchdog = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    wchar_t logPath[MAX_PATH] = {};
    if (BuildGameFilePath(L"aisp.screen.log", logPath, MAX_PATH))
    {
        SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        g_toolLog = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    wchar_t stats[8] = {};
    g_logStats = GetEnvironmentVariableW(L"AISP_SCREEN_STATS", stats, 8) > 0 && stats[0] == L'1';
}
// --- audio -----------------------------------------------------------------------------------

// Opens the default render device in shared mode at its mix format so ffmpeg can be told the
// exact sample rate and channel count. Runs before ffmpeg starts.
bool PrepareAudio(ScreenStream* stream)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator))) && enumerator)
    {
        if (SUCCEEDED(enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)) && device)
        {
            IAudioClient* client = nullptr;
            if (SUCCEEDED(device->lpVtbl->Activate(device, kIidAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client)
            {
                WAVEFORMATEX* format = nullptr;
                if (SUCCEEDED(client->lpVtbl->GetMixFormat(client, &format)) && format && format->nChannels > 0 && format->nSamplesPerSec > 0)
                {
                    stream->audioClient = client;
                    stream->mixFormat = format;
                    stream->sampleRate = format->nSamplesPerSec;
                    stream->channels = format->nChannels;
                    stream->samplesPerFrame = stream->sampleRate / stream->fps;
                    ok = true;
                }
                else
                {
                    if (format)
                        CoTaskMemFree(format);
                    client->lpVtbl->Release(client);
                }
            }
            device->lpVtbl->Release(device);
        }
        enumerator->lpVtbl->Release(enumerator);
    }
    return ok;
}

float Clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float GainOf(const ScreenStream* stream)
{
    return stream->muted ? 0.0f : stream->volume;
}

// The client's own avatar. Found by memory search against the position the server reported,
// verified across movement and turning, and cross-checked against a disassembly of the 6.4 MB
// client (image base 0x400000, loads unrelocated). Layout, all offsets verified live:
//   [0xA489AC]       chara manager block (plain data, no vtable); the CCharaTable* global sits
//                    right after it at 0xA489B0.
//   +0xC -> +0x0     slot holding the local CChara* (vtable 0x93A144).
//   CChara+0xC       chara type; the controller factory (jump table 0x40F68C) makes a
//                    CSelfCharaController for type 9, CPlayerController for type 1. So 9 = self.
//   CChara+0x20      CAIModel* (vtable 0x974E84; CAIModel -> CHLModel -> dxModel -> dxObject).
//                    CChara::GetPos at 0x402300 calls its vtable slot 4 (0x6DC370:
//                    lea eax,[ecx+0x18]) and only falls back to the embedded vec3 at CChara+0x28
//                    when +0x20 is null. That fallback keeps the spawn position on a live avatar,
//                    so always go via the model.
//   CChara+0x60      controller; for the local avatar a CSelfCharaController (vtable 0x93AD24,
//                    +0x4 points back at the CChara). Used below as the "this is us" check. Its
//                    +0x38 is not a position cache (denormal junk), despite looking like one.
//   CAIModel+0x18    x, y, z (world units, the same numbers the server sees in move packets).
//   CAIModel+0x24    rotation quaternion x, y, z, w ((0,1,0,0) at yaw 180 matches the matrix).
//   CAIModel+0x40    the position again (slot 5/16 getter 0x453C50); tracks +0x18 exactly.
//   CAIModel+0x68    4x4 world matrix: row 0 = right, row 2 = forward, row 3 = translation.
// CAIModel shares the getter thunks of the base dxObject table (vtable 0x99DBBC), but the
// field names next to that base table ("slot 4 = scale") do not apply here: slot 4 is position.
constexpr DWORD kSelfCharaControllerVtable = 0x93AD24;

bool ReadableAt(const void* address, size_t size)
{
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    const DWORD protect = info.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE || (info.Protect & PAGE_GUARD))
        return false;
    return reinterpret_cast<const BYTE*>(address) + size <= reinterpret_cast<const BYTE*>(info.BaseAddress) + info.RegionSize;
}

bool ReadListener(float* position, float* forward, float* right)
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return false;
    const BYTE* a = *reinterpret_cast<BYTE* const*>(0xA489AC);
    if (!a || !ReadableAt(a + 0xC, 4))
        return false;
    const BYTE* b = *reinterpret_cast<BYTE* const*>(a + 0xC);
    if (!b || !ReadableAt(b, 4))
        return false;
    const BYTE* chara = *reinterpret_cast<BYTE* const*>(b);
    if (!chara || !ReadableAt(chara, 0x64))
        return false;
    // Only trust the chain while the slot holds the avatar driven by the local controller.
    const BYTE* controller = *reinterpret_cast<BYTE* const*>(chara + 0x60);
    if (!controller || !ReadableAt(controller, 8))
        return false;
    if (*reinterpret_cast<const DWORD*>(controller) != kSelfCharaControllerVtable
        || *reinterpret_cast<BYTE* const*>(controller + 4) != chara)
        return false;
    const BYTE* entity = *reinterpret_cast<BYTE* const*>(chara + 0x20);
    if (!entity || !ReadableAt(entity, 0xA8))
        return false;
    const float* pos = reinterpret_cast<const float*>(entity + 0x18);
    const float* m = reinterpret_cast<const float*>(entity + 0x68);
    for (int i = 0; i < 3; ++i)
    {
        position[i] = pos[i];
        right[i] = m[i];
        forward[i] = m[8 + i];
    }
    // Sanity: a real position is finite and modest, the matrix rows are unit-ish.
    for (int i = 0; i < 3; ++i)
        if (!(position[i] == position[i]) || position[i] > 1e7f || position[i] < -1e7f)
            return false;
    const float f2 = forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    return f2 > 0.25f && f2 < 4.0f;
}

// Distance gain and stereo pan for a stream from the listener. Call under the stream lock.
void UpdateRolloff(ScreenStream* stream, const float* listener, const float* forward, const float* right, bool haveListener)
{
    (void)forward;
    if (!stream->rolloff || !haveListener)
    {
        stream->distanceGain = 1.0f;
        stream->panLeft = stream->panRight = 1.0f;
        return;
    }
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = stream->sourcePos[i] - listener[i];
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    // max at near or closer, min at far or beyond, linear across the range between them.
    float gain = stream->nearGain;
    if (stream->farDistance > stream->nearDistance)
    {
        if (dist >= stream->farDistance)
            gain = stream->farGain;
        else if (dist > stream->nearDistance)
        {
            const float toNear = (stream->farDistance - dist) / (stream->farDistance - stream->nearDistance);
            gain = stream->farGain + (stream->nearGain - stream->farGain) * toNear;
        }
    }
    stream->distanceGain = gain;
    // Optionally pan by the bearing of the source against the avatar's right vector (horizontal
    // only), constant power, never fully one-sided. Off unless the page asks: a screen in a
    // street is not a point source, the sound fills the space.
    float pan = 0.0f;
    const float horiz = sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (stream->pan && horiz > 1.0f)
        pan = (d[0] * right[0] + d[2] * right[2]) / horiz * 0.7f;
    stream->panLeft = sqrtf((1.0f - pan) * 0.5f) * 1.4142f;
    stream->panRight = sqrtf((1.0f + pan) * 0.5f) * 1.4142f;
}

DWORD WINAPI AudioRenderThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    IAudioClient* client = stream->audioClient;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return 0;

    const REFERENCE_TIME bufferDuration = 2000000; // 200 ms
    if (FAILED(client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, bufferDuration, 0, stream->mixFormat, nullptr)))
    {
        SetStatus(stream, L"audio: device initialisation failed; video only");
        CloseHandle(event);
        return 0;
    }
    UINT32 bufferFrames = 0;
    IAudioRenderClient* render = nullptr;
    if (FAILED(client->lpVtbl->SetEventHandle(client, event)) || FAILED(client->lpVtbl->GetBufferSize(client, &bufferFrames)) || FAILED(client->lpVtbl->GetService(client, kIidAudioRenderClient, reinterpret_cast<void**>(&render))) || !render)
    {
        SetStatus(stream, L"audio: render client unavailable; video only");
        CloseHandle(event);
        return 0;
    }
    client->lpVtbl->Start(client);

    const bool isFloat = stream->mixFormat->wBitsPerSample == 32;
    while (!stream->stop)
    {
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0)
            continue;
        UINT32 padding = 0;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &padding)))
            break;
        const UINT32 want = bufferFrames - padding;
        if (want == 0)
            continue;
        BYTE* out = nullptr;
        if (FAILED(render->lpVtbl->GetBuffer(render, want, &out)) || !out)
            break;

        EnterCriticalSection(&stream->lock);
        const bool held = GetTickCount64() - stream->lastDraw > kAudioHoldMs;
        const bool feed = stream->playing && stream->audioActive && !held && !stream->paused;
        const LONGLONG available = stream->audioWritten - stream->audioPos;
        if (feed && available >= static_cast<LONGLONG>(want))
        {
            // Ease the geometry-driven gains over this buffer so steps never click.
            stream->smoothGain += (stream->distanceGain - stream->smoothGain) * 0.2f;
            stream->smoothLeft += (stream->panLeft - stream->smoothLeft) * 0.2f;
            stream->smoothRight += (stream->panRight - stream->smoothRight) * 0.2f;
            const float gain = GainOf(stream) * stream->smoothGain;
            for (UINT32 i = 0; i < want; ++i)
            {
                const float* src = stream->audioRing + ((stream->audioPos + i) % stream->audioCapacity) * stream->channels;
                for (UINT32 c = 0; c < stream->channels; ++c)
                {
                    const float channelGain = c == 0 ? stream->smoothLeft : (c == 1 ? stream->smoothRight : 1.0f);
                    const float sample = src[c] * gain * channelGain;
                    if (isFloat)
                        reinterpret_cast<float*>(out)[i * stream->channels + c] = sample;
                    else
                        reinterpret_cast<short*>(out)[i * stream->channels + c] = static_cast<short>(sample * 32767.0f);
                }
            }
            stream->audioPos += want;
            stream->audioPlayed = stream->audioPos - padding - want;
            LeaveCriticalSection(&stream->lock);
            render->lpVtbl->ReleaseBuffer(render, want, 0);
        }
        else
        {
            if (feed)
            {
                stream->playing = false; // underrun: everything holds until the ring refills
                ++stream->underruns;
            }
            // While held (no draws) nothing advances, so picture and sound resume together.
            LeaveCriticalSection(&stream->lock);
            render->lpVtbl->ReleaseBuffer(render, want, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    client->lpVtbl->Stop(client);
    render->lpVtbl->Release(render);
    CloseHandle(event);
    return 0;
}

// --- sources ---------------------------------------------------------------------------------

// The session thread: whichever source claims the prefix (source.cpp) fills the rings.
DWORD WINAPI StreamThread(LPVOID parameter)
{
    ScreenStream* stream = static_cast<ScreenStream*>(parameter);
    const ScreenSource* source = FindSource(stream->source);
    if (!source)
    {
        SetStatus(stream, L"unknown source (see source.cpp for the prefixes)");
        return 0;
    }
    return source->run(stream);
}

void StopSession(ScreenStream* stream)
{
    if (!stream->sessionActive)
        return;
    stream->sessionActive = false;
    InterlockedExchange(&stream->stop, 1);
    // Take the handles out under the lock: a source in the middle of its own retry (EndAttempt)
    // may be closing some of them, and whoever nulls a field owns it. Processes first, so a
    // source blocked reading ffmpeg's output sees the pipe close.
    HANDLE processes[2] = {};
    EnterCriticalSection(&stream->lock);
    for (int i = 0; i < 2; ++i)
    {
        processes[i] = stream->processes[i];
        stream->processes[i] = nullptr;
    }
    LeaveCriticalSection(&stream->lock);
    for (HANDLE process : processes)
    {
        if (process)
        {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
    }
    // The source thread first (its pipe closes with the process); the renderer's handle is
    // taken only once it is gone, since the source starts it. A wait that times out gets a
    // log line: the workers all watch `stop`.
    const ULONGLONG waitStart = GetTickCount64();
    HANDLE threads[2] = {};
    const char* names[2] = {"source", "audio renderer"};
    EnterCriticalSection(&stream->lock);
    threads[0] = stream->thread;
    stream->thread = nullptr;
    LeaveCriticalSection(&stream->lock);
    for (int i = 0; i < 2; ++i)
    {
        if (i == 1)
        {
            EnterCriticalSection(&stream->lock);
            threads[1] = stream->audioRenderThread;
            stream->audioRenderThread = nullptr;
            LeaveCriticalSection(&stream->lock);
        }
        if (!threads[i])
            continue;
        if (WaitForSingleObject(threads[i], 2000) != WAIT_OBJECT_0)
        {
            char note[128] = {};
            StringCchPrintfA(note, 128, "session stop: %s thread did not end in time\r\n", names[i]);
            LogLine(note);
        }
        CloseHandle(threads[i]);
    }
    char stopped[600] = {};
    StringCchPrintfA(stopped, 600, "session stop: %ls (%lu ms)\r\n", stream->source, static_cast<unsigned long>(GetTickCount64() - waitStart));
    LogLine(stopped);
    EnterCriticalSection(&stream->lock);
    stream->playing = false;
    stream->audioActive = false;
    LeaveCriticalSection(&stream->lock);
    if (stream->audioClient)
    {
        stream->audioClient->lpVtbl->Release(stream->audioClient);
        stream->audioClient = nullptr;
    }
    if (stream->mixFormat)
    {
        CoTaskMemFree(stream->mixFormat);
        stream->mixFormat = nullptr;
    }
}

void FreeRings(ScreenStream* stream)
{
    EnterCriticalSection(&stream->lock);
    if (stream->keyDc)
    {
        SelectObject(stream->keyDc, stream->keyOldBitmap);
        DeleteObject(stream->keyBitmap);
        DeleteDC(stream->keyDc);
        stream->keyDc = nullptr;
        stream->keyBitmap = nullptr;
        stream->keyBits = nullptr;
        stream->keyWidth = stream->keyHeight = 0;
    }
    delete[] stream->ring;
    delete[] stream->audioRing;
    stream->ring = nullptr;
    stream->audioRing = nullptr;
    stream->videoWritten = stream->videoPos = 0;
    LeaveCriticalSection(&stream->lock);
}

// Starts (or restarts) decoding the stream's source into its rings.
void StartSession(ScreenStream* stream)
{
    if (stream->sessionActive || !stream->source[0])
        return;
    EnterCriticalSection(&stream->lock);
    stream->videoWritten = stream->videoPos = 0;
    stream->audioWritten = stream->audioPos = stream->audioPlayed = 0;
    stream->playing = false;
    stream->audioActive = false;
    stream->titlePoll = 0;
    stream->underruns = stream->videoWaits = stream->videoDrops = stream->audioWaits = stream->audioDrops = 0;
    if (!stream->ring)
    {
        stream->capacity = RingCapacityFor(stream->frameBytes, stream->fps);
        stream->ring = new BYTE[static_cast<size_t>(stream->frameBytes) * static_cast<size_t>(stream->capacity)]();
    }
    LeaveCriticalSection(&stream->lock);

    stream->audioWanted = PrepareAudio(stream);
    if (stream->audioWanted)
    {
        // Two seconds at least: a decoder's reordering depth puts audio well ahead of the first
        // picture, and samples are cheap next to frames.
        LONGLONG audioCapacity = static_cast<LONGLONG>(stream->sampleRate) * stream->capacity * 3 / 2 / stream->fps;
        if (audioCapacity < static_cast<LONGLONG>(stream->sampleRate) * 2)
            audioCapacity = static_cast<LONGLONG>(stream->sampleRate) * 2;
        EnterCriticalSection(&stream->lock);
        if (!stream->audioRing || stream->audioCapacity != audioCapacity)
        {
            delete[] stream->audioRing;
            stream->audioCapacity = audioCapacity;
            stream->audioRing = new float[static_cast<size_t>(audioCapacity) * stream->channels]();
        }
        LeaveCriticalSection(&stream->lock);
    }

    StringCchCopyW(stream->status, 256, L"starting");
    stream->lastDraw = stream->sessionStarted = GetTickCount64();
    // Where the shared timeline says the video is right now (seeking sources honour it).
    stream->sessionStart = stream->pageStart;
    stream->sessionOffset = stream->pageOffset;
    stream->seekSeconds = 0;
    if (stream->pageStart > 0)
    {
        const double until = stream->pagePausedAt > 0 ? stream->pagePausedAt : UnixNow();
        const double elapsed = until - stream->pageStart;
        stream->seekSeconds = stream->pageOffset + (elapsed > 0 ? elapsed : 0);
    }
    InterlockedExchange(&stream->stop, 0);
    stream->sessionActive = true;
    DebugLog(L"aisp.hook: screen stream: %s\n", stream->source);
    char started[600] = {};
    char cropNote[64] = {};
    if (stream->crop[0] > 0)
        StringCchPrintfA(cropNote, 64, " crop=%dx%d+%d+%d", stream->crop[0], stream->crop[1], stream->crop[2], stream->crop[3]);
    StringCchPrintfA(started, 600, "session start: %ls %dx%d @%d audio=%d seek=%.1f%s\r\n", stream->source, stream->videoWidth, stream->videoHeight, stream->fps, stream->audioWanted ? 1 : 0, stream->seekSeconds, cropNote);
    LogLine(started);
    stream->thread = CreateThread(nullptr, 0, StreamThread, stream, 0, nullptr);
    if (!stream->thread)
    {
        stream->sessionActive = false;
        SetStatus(stream, L"thread creation failed");
    }
}

IUnknown* IdentityOf(IUnknown* object)
{
    IUnknown* identity = nullptr;
    if (object && SUCCEEDED(object->lpVtbl->QueryInterface(object, kIidUnknown, reinterpret_cast<void**>(&identity))))
        return identity;
    return nullptr;
}

// Called from the Navigate hook with the URL the client built, before the rewrite. Every screen
// gets an entry; what it plays, if anything, is decided by the page the server serves, which
// publishes the source in its title (read by the OleDraw hook).
void OnScreenNavigate(IWebBrowser2* browser, const wchar_t* url)
{
    if (!url || !browser)
        return;
    InitScreenVideo();

    IUnknown* identity = IdentityOf(reinterpret_cast<IUnknown*>(browser));
    if (!identity)
        return;

    // One entry per browser: a new navigation replaces whatever it streamed before. The stop
    // happens outside the list lock: it waits for worker threads, and the watchdog and the
    // draw hook need the list meanwhile.
    EnterCriticalSection(&g_streamsLock);
    ScreenStream* stream = nullptr;
    for (ScreenStream* existing = g_streams; existing && !stream; existing = existing->next)
        if (existing->browser == identity)
            stream = existing;
    const bool found = stream != nullptr;
    if (!found)
    {
        stream = new ScreenStream();
        InitializeCriticalSection(&stream->lock);
        stream->browser = identity; // takes the reference
        stream->next = g_streams;
        g_streams = stream;
    }
    LeaveCriticalSection(&g_streamsLock);
    if (found)
        identity->lpVtbl->Release(identity); // the entry already holds one
    StopSession(stream);

    // Crop rectangles the client copies out of the control (see the frame routine): live pages
    // (URL with /lv) 950x520 placed at the page's flvplayer_container, which the emulator's
    // page puts at the origin; everything else 486x343 at (9,15). The video box is where a
    // stream is painted inside that crop; the page normally states it in its title (box=), these
    // are the fallbacks: a TV's whole crop, the Stage wall's main LED (7,65)-(550,447).
    int x = 9, y = 15, width = 486, height = 343;
    int videoX = 0, videoY = 0, videoWidth = 486, videoHeight = 343;
    if (std::wcsstr(url, L"/lv"))
    {
        x = 0;
        y = 0;
        width = 950;
        height = 520;
        videoX = 7;
        videoY = 65;
        videoWidth = 543;
        videoHeight = 382;
    }
    // Ring frames hold the video box only (a Stage frame would otherwise be 2 MB of mostly black).
    const DWORD frameBytes = static_cast<DWORD>(videoWidth) * static_cast<DWORD>(videoHeight) * 4;
    if (frameBytes != stream->frameBytes)
        FreeRings(stream);
    EnterCriticalSection(&stream->lock);
    stream->x = x;
    stream->y = y;
    stream->width = width;
    stream->height = height;
    stream->videoX = videoX;
    stream->videoY = videoY;
    stream->videoWidth = videoWidth;
    stream->videoHeight = videoHeight;
    stream->frameBytes = frameBytes;
    stream->source[0] = L'\0';
    stream->pageSource[0] = L'\0';
    stream->pageFps = 0;
    std::memset(stream->pageBox, 0, sizeof(stream->pageBox));
    std::memset(stream->pageCrop, 0, sizeof(stream->pageCrop));
    if (stream->html)
    {
        stream->html->lpVtbl->Release(stream->html);
        stream->html = nullptr;
    }
    stream->document = nullptr; // the navigation brings a new document
    stream->lastDraw = GetTickCount64();
    LeaveCriticalSection(&stream->lock);
}

// The document OleDraw hands us belongs to some WebBrowser; ask it which through its service
// provider and match against the streams. Cached per document pointer afterwards.
ScreenStream* FindStream(IUnknown* document)
{
    if (!document)
        return nullptr;

    EnterCriticalSection(&g_streamsLock);
    ScreenStream* found = nullptr;
    for (ScreenStream* stream = g_streams; stream && !found; stream = stream->next)
        if (stream->document == document)
            found = stream;
    LeaveCriticalSection(&g_streamsLock);
    if (found)
        return found;

    IServiceProvider* provider = nullptr;
    if (FAILED(document->lpVtbl->QueryInterface(document, kIidServiceProvider, reinterpret_cast<void**>(&provider))) || !provider)
        return nullptr;
    IUnknown* browser = nullptr;
    provider->lpVtbl->QueryService(provider, kSidWebBrowserApp, kIidWebBrowser2, reinterpret_cast<void**>(&browser));
    provider->lpVtbl->Release(provider);
    if (!browser)
        return nullptr;
    IUnknown* identity = IdentityOf(browser);
    browser->lpVtbl->Release(browser);
    if (!identity)
        return nullptr;

    EnterCriticalSection(&g_streamsLock);
    for (ScreenStream* stream = g_streams; stream && !found; stream = stream->next)
    {
        if (stream->browser == identity)
        {
            stream->document = document;
            IHTMLDocument2* html = nullptr;
            if (SUCCEEDED(document->lpVtbl->QueryInterface(document, kIidHtmlDocument2, reinterpret_cast<void**>(&html))))
            {
                EnterCriticalSection(&stream->lock);
                if (stream->html)
                    stream->html->lpVtbl->Release(stream->html);
                stream->html = html;
                LeaveCriticalSection(&stream->lock);
            }
            found = stream;
        }
    }
    LeaveCriticalSection(&g_streamsLock);
    identity->lpVtbl->Release(identity);
    return found;
}

// The page publishes "aisp:vol=<0-100>;mute=<0|1>[;src=<source>]" in its title: the volume and
// mute whenever the client's ext_setVolume / ext_setMute reach it, the source as the server
// decided when it served the page. Call under the stream lock.
void PollTitle(ScreenStream* stream)
{
    if (!stream->html || ++stream->titlePoll < kTitlePollFrames)
        return;
    stream->titlePoll = 0;
    BSTR title = nullptr;
    if (FAILED(stream->html->lpVtbl->get_title(stream->html, &title)) || !title)
        return;
    if (std::wcsncmp(title, L"aisp:", 5) == 0)
    {
        // Only sources the hook decodes count; the page frames web pages itself.
        const wchar_t* src = std::wcsstr(title, L";src=");
        const bool stream_ = src && IsKnownSource(src + 5);
        StringCchCopyW(stream->pageSource, 512, stream_ ? src + 5 : L"");
        // Where the page wants the video, in crop coordinates; it knows what each panel shows.
        const wchar_t* fpsText = std::wcsstr(title, L";fps=");
        int fps = 0;
        stream->pageFps = (fpsText && swscanf(fpsText + 5, L"%d", &fps) == 1 && IsSupportedFps(fps)) ? fps : 0;
        const wchar_t* keyText = std::wcsstr(title, L";key=");
        unsigned int key = 0;
        stream->keyed = keyText && swscanf(keyText + 5, L"%6x", &key) == 1;
        stream->keyColor = key & 0xFFFFFF;
        int box[4] = {};
        const wchar_t* boxText = std::wcsstr(title, L";box=");
        if (boxText && swscanf(boxText + 5, L"%d,%d,%d,%d", &box[0], &box[1], &box[2], &box[3]) == 4 && box[2] > 0 && box[3] > 0
            && box[0] >= 0 && box[1] >= 0 && box[0] + box[2] <= stream->width && box[1] + box[3] <= stream->height)
            std::memcpy(stream->pageBox, box, sizeof(box));
        int crop[4] = {};
        const wchar_t* cropText = std::wcsstr(title, L";crop=");
        std::memset(stream->pageCrop, 0, sizeof(stream->pageCrop));
        if (cropText && swscanf(cropText + 6, L"%d,%d,%d,%d", &crop[0], &crop[1], &crop[2], &crop[3]) == 4 && crop[0] > 0 && crop[1] > 0
            && crop[2] >= 0 && crop[3] >= 0 && crop[0] <= 4096 && crop[1] <= 4096)
            std::memcpy(stream->pageCrop, crop, sizeof(crop));
    }
    if (const wchar_t* vol = std::wcsstr(title, L"vol="))
    {
        const float percent = static_cast<float>(wcstol(vol + 4, nullptr, 10));
        const float linear = percent < 0 ? 0.0f : (percent > 100 ? 1.0f : percent / 100.0f);
        stream->volume = linear * linear; // perceptual taper
    }
    if (const wchar_t* mute = std::wcsstr(title, L"mute="))
        stream->muted = mute[5] == L'1';
    if (const wchar_t* roll = std::wcsstr(title, L";rolloff="))
    {
        // x,y,z,near,far[,max,min]. The gains are optional so a five-number word still means the
        // plain fade to silence; anything else is a malformed word and turns rolloff off.
        float v[7] = {0, 0, 0, 0, 0, 1.0f, 0.0f};
        const int got = swscanf(roll + 9, L"%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
        stream->rolloff = (got == 5 || got == 7) && v[4] > v[3] && v[3] >= 0;
        if (stream->rolloff)
        {
            stream->sourcePos[0] = v[0];
            stream->sourcePos[1] = v[1];
            stream->sourcePos[2] = v[2];
            stream->nearDistance = v[3];
            stream->farDistance = v[4];
            stream->nearGain = Clamp01(v[5]);
            stream->farGain = Clamp01(v[6]);
        }
    }
    else
    {
        stream->rolloff = false;
    }
    const wchar_t* pan = std::wcsstr(title, L";pan=");
    stream->pan = pan && pan[5] == L'1';
    const wchar_t* start = std::wcsstr(title, L";start=");
    const wchar_t* offset = std::wcsstr(title, L";offset=");
    const wchar_t* paused = std::wcsstr(title, L";paused=");
    const wchar_t* hold = std::wcsstr(title, L";hold=");
    stream->pageStart = start ? wcstod(start + 7, nullptr) : 0;
    stream->pageOffset = offset ? wcstod(offset + 8, nullptr) : 0;
    stream->pagePausedAt = paused ? wcstod(paused + 8, nullptr) : 0;
    stream->pageHold = hold && hold[6] == L'1';
    SysFreeString(title);
}

// A DIB the size of the video box, to read the page's pixels back and composite in.
bool EnsureKeySurface(ScreenStream* stream, HDC reference)
{
    if (stream->keyDc && stream->keyWidth == stream->videoWidth && stream->keyHeight == stream->videoHeight)
        return true;
    if (stream->keyDc)
    {
        SelectObject(stream->keyDc, stream->keyOldBitmap);
        DeleteObject(stream->keyBitmap);
        DeleteDC(stream->keyDc);
        stream->keyDc = nullptr;
        stream->keyBitmap = nullptr;
        stream->keyBits = nullptr;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = stream->videoWidth;
    info.bmiHeader.biHeight = -stream->videoHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    stream->keyDc = CreateCompatibleDC(reference);
    stream->keyBitmap = stream->keyDc ? CreateDIBSection(stream->keyDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!stream->keyBitmap)
    {
        if (stream->keyDc)
            DeleteDC(stream->keyDc);
        stream->keyDc = nullptr;
        return false;
    }
    stream->keyOldBitmap = SelectObject(stream->keyDc, stream->keyBitmap);
    stream->keyBits = static_cast<BYTE*>(bits);
    stream->keyWidth = stream->videoWidth;
    stream->keyHeight = stream->videoHeight;
    return true;
}

HRESULT WINAPI HookOleDraw(LPUNKNOWN unknown, DWORD aspect, HDC hdc, LPCRECT bounds)
{
    ScreenStream* stream = g_screenVideoInitialised ? FindStream(unknown) : nullptr;
    if (!stream || !bounds || !hdc)
        return g_originalOleDraw(unknown, aspect, hdc, bounds);
    stream->lastDraw = GetTickCount64();

    // What the page says to play and where, applied when either changes: start, replace, or stop.
    wchar_t wanted[512] = {};
    int wantedBox[4] = {};
    EnterCriticalSection(&stream->lock);
    PollTitle(stream);
    StringCchCopyW(wanted, 512, stream->pageSource);
    std::memcpy(wantedBox, stream->pageBox, sizeof(wantedBox));
    const bool boxChanged = wantedBox[2] > 0
        && (wantedBox[0] != stream->videoX || wantedBox[1] != stream->videoY || wantedBox[2] != stream->videoWidth || wantedBox[3] != stream->videoHeight);
    const int wantedFps = stream->pageFps ? stream->pageFps : kDefaultFps;
    const bool fpsChanged = wantedFps != stream->fps;
    // The crop is the session's: the source renders at that size, so a change restarts it.
    const bool cropChanged = std::memcmp(stream->pageCrop, stream->crop, sizeof(stream->crop)) != 0;
    // A moved timeline (resume, seek) restarts the session at the new position; a pause only holds.
    const bool timelineChanged = stream->sessionActive && (stream->pageStart != stream->sessionStart || stream->pageOffset != stream->sessionOffset);
    stream->paused = stream->pagePausedAt > 0 || stream->pageHold;
    const bool changed = std::wcscmp(wanted, stream->source) != 0 || boxChanged || fpsChanged || cropChanged || timelineChanged;
    LeaveCriticalSection(&stream->lock);
    if (changed)
    {
        StopSession(stream);
        StringCchCopyW(stream->source, 512, wanted);
        std::memcpy(stream->crop, stream->pageCrop, sizeof(stream->crop));
        if (fpsChanged)
        {
            FreeRings(stream); // the ring length is a duration; re-derive it for the new rate
            stream->fps = wantedFps;
        }
        if (boxChanged)
        {
            const DWORD frameBytes = static_cast<DWORD>(wantedBox[2]) * static_cast<DWORD>(wantedBox[3]) * 4;
            if (frameBytes != stream->frameBytes)
                FreeRings(stream);
            stream->videoX = wantedBox[0];
            stream->videoY = wantedBox[1];
            stream->videoWidth = wantedBox[2];
            stream->videoHeight = wantedBox[3];
            stream->frameBytes = frameBytes;
        }
    }
    if (!stream->sessionActive)
    {
        // Also the restart after an idle stop (game minimised, or the TV came back).
        if (stream->source[0])
            StartSession(stream);
        else
            return g_originalOleDraw(unknown, aspect, hdc, bounds); // the page is the content
    }

    // When the video box is only part of the crop (the Stage wall's main LED), let the page draw
    // itself first so the rest of the crop, the banner, still shows the page; a TV's video box is
    // the whole crop, so the page draw is skipped there.
    const bool videoFillsCrop = stream->videoX == 0 && stream->videoY == 0 && stream->videoWidth == stream->width && stream->videoHeight == stream->height;
    if (videoFillsCrop && !stream->keyed)
        FillRect(hdc, bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    else
        g_originalOleDraw(unknown, aspect, hdc, bounds);
    const int x = bounds->left + stream->x;
    const int y = bounds->top + stream->y;

    EnterCriticalSection(&stream->lock);
    if (stream->rolloff)
    {
        float listener[3], forward[3], right[3];
        const bool have = ReadListener(listener, forward, right);
        UpdateRolloff(stream, listener, forward, right, have);
    }

    // Pre-roll, then present: with audio, the frame that matches what the speaker has played;
    // without, one frame per interval on the performance counter. Underruns hold the last
    // frame (the audio thread clears `playing`); a full ring holds the reader instead.
    const BYTE* shown = nullptr;
    {
        const LONGLONG queued = stream->videoWritten - stream->videoPos;
        const bool hasAudio = stream->audioActive;
        if (!stream->playing)
        {
            const int needed = stream->videoPos == 0 ? PrerollFor(stream->capacity, stream->fps) : ResumeFor(stream->capacity, stream->fps);
            if (hasAudio && stream->samplesPerFrame)
            {
                // Audio older than the oldest frame still queued (the silence ffmpeg pads in front
                // of late-starting audio, or frames dropped while waiting) is skipped, so sound and
                // picture start together instead of the picture freezing until the sound catches up.
                const LONGLONG aligned = stream->videoPos * stream->samplesPerFrame;
                if (stream->audioPos < aligned)
                    stream->audioPos = aligned < stream->audioWritten ? aligned : stream->audioWritten;
                else if (stream->audioPos > aligned)
                {
                    // Audio ran ahead (samples were dropped): the frames older than it are stale.
                    const LONGLONG frame = stream->audioPos / stream->samplesPerFrame;
                    stream->videoPos = frame < stream->videoWritten ? frame : stream->videoWritten;
                }
                stream->audioPlayed = stream->audioPos;
            }
            const bool audioReady = !hasAudio || stream->audioWritten - stream->audioPos >= static_cast<LONGLONG>(needed) * stream->samplesPerFrame;
            if (queued >= needed && audioReady)
            {
                stream->playing = true;
                LARGE_INTEGER now = {};
                QueryPerformanceCounter(&now);
                stream->nextPresent = now.QuadPart;
            }
        }
        if (stream->playing && !stream->paused)
        {
            if (hasAudio)
            {
                const LONGLONG target = stream->audioPlayed / stream->samplesPerFrame + 1;
                if (target > stream->videoPos)
                    stream->videoPos = target < stream->videoWritten ? target : stream->videoWritten;
            }
            else
            {
                LARGE_INTEGER now = {}, frequency = {};
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&frequency);
                const LONGLONG interval = frequency.QuadPart / stream->fps;
                while (now.QuadPart >= stream->nextPresent)
                {
                    if (stream->videoPos >= stream->videoWritten)
                    {
                        stream->playing = false;
                        break;
                    }
                    ++stream->videoPos;
                    stream->nextPresent += interval;
                }
            }
        }
        if (stream->videoPos > 0 && stream->ring)
            shown = stream->ring + static_cast<size_t>((stream->videoPos - 1) % stream->capacity) * stream->frameBytes;
    }

    if (shown)
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = stream->videoWidth;
        info.bmiHeader.biHeight = -stream->videoHeight; // top-down, as the sources write it
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const int bx = x + stream->videoX, by = y + stream->videoY;
        if (stream->keyed && EnsureKeySurface(stream, hdc))
        {
            // Read the page under the box, put video where it painted the key colour, write back.
            BitBlt(stream->keyDc, 0, 0, stream->videoWidth, stream->videoHeight, hdc, bx, by, SRCCOPY);
            GdiFlush();
            const DWORD key = stream->keyColor;
            DWORD* page = reinterpret_cast<DWORD*>(stream->keyBits);
            const DWORD* video = reinterpret_cast<const DWORD*>(shown);
            const size_t count = static_cast<size_t>(stream->videoWidth) * static_cast<size_t>(stream->videoHeight);
            for (size_t i = 0; i < count; ++i)
                if ((page[i] & 0xFFFFFF) == key)
                    page[i] = video[i];
            BitBlt(hdc, bx, by, stream->videoWidth, stream->videoHeight, stream->keyDc, 0, 0, SRCCOPY);
        }
        else
        {
            SetDIBitsToDevice(hdc, bx, by, stream->videoWidth, stream->videoHeight, 0, 0, 0, stream->videoHeight, shown, &info, DIB_RGB_COLORS);
        }
    }
    else
    {
        RECT videoBox = {x + stream->videoX, y + stream->videoY, x + stream->videoX + stream->videoWidth, y + stream->videoY + stream->videoHeight};
        FillRect(hdc, &videoBox, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutW(hdc, videoBox.left + 10, videoBox.top + 10, stream->source, static_cast<int>(std::wcslen(stream->source)));
        TextOutW(hdc, videoBox.left + 10, videoBox.top + 30, stream->status, static_cast<int>(std::wcslen(stream->status)));
    }
    LeaveCriticalSection(&stream->lock);
    return S_OK;
}

// The ATL host calls IOleObject::Close on the control when the client destroys a screen's
// window: the TV switched off or closed, the map unloaded. Stop that screen's stream at once and
// drop the entry; the draw watchdog stays for pauses that are not teardowns.
HRESULT STDMETHODCALLTYPE HookOleClose(IOleObject* self, DWORD saveOption)
{
    if (g_screenVideoInitialised)
    {
        IUnknown* identity = IdentityOf(reinterpret_cast<IUnknown*>(self));
        if (identity)
        {
            ScreenStream* stream = nullptr;
            EnterCriticalSection(&g_streamsLock);
            ScreenStream** link = &g_streams;
            while (*link && !stream)
            {
                if ((*link)->browser == identity)
                {
                    stream = *link;
                    *link = stream->next; // unlinked: the watchdog will not see it again
                }
                else
                {
                    link = &(*link)->next;
                }
            }
            LeaveCriticalSection(&g_streamsLock);
            identity->lpVtbl->Release(identity);

            if (stream)
            {
                DebugLog(L"aisp.hook: screen closed: %s\n", stream->source);
                StopSession(stream);
                FreeRings(stream);
                EnterCriticalSection(&stream->lock);
                if (stream->html)
                {
                    stream->html->lpVtbl->Release(stream->html);
                    stream->html = nullptr;
                }
                stream->document = nullptr;
                stream->source[0] = L'\0';
                LeaveCriticalSection(&stream->lock);
                stream->browser->lpVtbl->Release(stream->browser);
                stream->browser = nullptr;
                // The struct itself is kept: a worker that outlived the 5 s join would still hold it.
            }
        }
    }
    return g_originalOleClose ? g_originalOleClose(self, saveOption) : S_OK;
}

// --- browser patch -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookNavigate(IWebBrowser2* self, BSTR url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    BSTR rewritten = RewriteScreenUrl(url);
    if (rewritten)
        OnScreenNavigate(self, url);
    const HRESULT hr = g_originalNavigate(self, rewritten ? rewritten : url, flags, targetFrameName, postData, headers);
    if (rewritten)
        SysFreeString(rewritten);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookNavigate2(IWebBrowser2* self, VARIANT* url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    if (url && V_VT(url) == VT_BSTR)
    {
        BSTR rewritten = RewriteScreenUrl(V_BSTR(url));
        if (rewritten)
        {
            OnScreenNavigate(self, V_BSTR(url));
            VARIANT replaced;
            VariantInit(&replaced);
            V_VT(&replaced) = VT_BSTR;
            V_BSTR(&replaced) = rewritten;
            const HRESULT hr = g_originalNavigate2(self, &replaced, flags, targetFrameName, postData, headers);
            SysFreeString(rewritten);
            return hr;
        }
    }
    return g_originalNavigate2(self, url, flags, targetFrameName, postData, headers);
}

// All WebBrowser instances share ieframe's vtable, so one patch covers every screen.
void PatchWebBrowserVtable(IUnknown* unknown)
{
    if (g_webBrowserPatched || !unknown)
        return;

    IWebBrowser2* browser = nullptr;
    if (FAILED(unknown->lpVtbl->QueryInterface(unknown, kIidWebBrowser2, reinterpret_cast<void**>(&browser))) || !browser)
        return;

    IWebBrowser2Vtbl* vtable = browser->lpVtbl;
    DWORD oldProtect = 0;
    if (VirtualProtect(vtable, sizeof(*vtable), PAGE_READWRITE, &oldProtect))
    {
        g_originalNavigate = vtable->Navigate;
        g_originalNavigate2 = vtable->Navigate2;
        vtable->Navigate = HookNavigate;
        vtable->Navigate2 = HookNavigate2;
        DWORD ignored = 0;
        VirtualProtect(vtable, sizeof(*vtable), oldProtect, &ignored);
        g_webBrowserPatched = true;
        OutputDebugStringW(L"aisp.hook: IWebBrowser2::Navigate patched\n");
    }

    browser->lpVtbl->Release(browser);

    IOleObject* oleObject = nullptr;
    if (SUCCEEDED(unknown->lpVtbl->QueryInterface(unknown, kIidOleObject, reinterpret_cast<void**>(&oleObject))) && oleObject)
    {
        IOleObjectVtbl* oleVtable = oleObject->lpVtbl;
        if (VirtualProtect(oleVtable, sizeof(*oleVtable), PAGE_READWRITE, &oldProtect))
        {
            g_originalOleClose = oleVtable->Close;
            oleVtable->Close = HookOleClose;
            DWORD ignored = 0;
            VirtualProtect(oleVtable, sizeof(*oleVtable), oldProtect, &ignored);
            OutputDebugStringW(L"aisp.hook: IOleObject::Close patched\n");
        }
        oleObject->lpVtbl->Release(oleObject);
    }
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out)
{
    if (!g_originalCoCreateInstance)
        return E_FAIL;

    const HRESULT hr = g_originalCoCreateInstance(clsid, outer, context, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(clsid, kClsidWebBrowser))
        PatchWebBrowserVtable(static_cast<IUnknown*>(*out));
    return hr;
}

template <typename T>
bool PatchSingleImport(HMODULE module, const char* importedModuleName, const char* importName, void* replacement, T* original)
{
    if (!module)
        return false;

    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress || !importDir.Size)
        return false;

    auto importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    while (importDesc->Name)
    {
        auto moduleName = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(moduleName, importedModuleName) == 0)
        {
            auto originalThunk = importDesc->OriginalFirstThunk
                                     ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->OriginalFirstThunk)
                                     : reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);

            while (originalThunk->u1.AddressOfData)
            {
                if ((originalThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) == 0)
                {
                    auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), importName) == 0)
                    {
                        DWORD oldProtect = 0;
                        if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                            return false;

                        if (original && !*original)
                            *original = reinterpret_cast<T>(thunk->u1.Function);
                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

                        FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));

                        DWORD ignored = 0;
                        VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignored);
                        return true;
                    }
                }

                ++originalThunk;
                ++thunk;
            }
        }
        ++importDesc;
    }

    return false;
}

template <typename T>
void PatchImport(HMODULE module, const char* importName, void* replacement, T* original)
{
    PatchSingleImport(module, "KERNEL32.dll", importName, replacement, original);
    PatchSingleImport(module, "KERNELBASE.dll", importName, replacement, original);
}

void PatchModule(HMODULE module)
{
    PatchImport(module, "GetACP", reinterpret_cast<void*>(HookGetACP), &g_originalGetACP);
    PatchImport(module, "GetOEMCP", reinterpret_cast<void*>(HookGetOEMCP), &g_originalGetOEMCP);
    PatchImport(module, "GetThreadLocale", reinterpret_cast<void*>(HookGetThreadLocale), &g_originalGetThreadLocale);
    PatchImport(module, "GetUserDefaultLCID", reinterpret_cast<void*>(HookGetUserDefaultLCID), &g_originalGetUserDefaultLCID);
    PatchImport(module, "GetSystemDefaultLCID", reinterpret_cast<void*>(HookGetSystemDefaultLCID), &g_originalGetSystemDefaultLCID);
    PatchImport(module, "GetUserDefaultLangID", reinterpret_cast<void*>(HookGetUserDefaultLangID), &g_originalGetUserDefaultLangID);
    PatchImport(module, "GetSystemDefaultLangID", reinterpret_cast<void*>(HookGetSystemDefaultLangID), &g_originalGetSystemDefaultLangID);
    PatchImport(module, "MultiByteToWideChar", reinterpret_cast<void*>(HookMultiByteToWideChar), &g_originalMultiByteToWideChar);
    PatchImport(module, "WideCharToMultiByte", reinterpret_cast<void*>(HookWideCharToMultiByte), &g_originalWideCharToMultiByte);
    PatchImport(module, "LoadLibraryW", reinterpret_cast<void*>(HookLoadLibraryW), &g_originalLoadLibraryW);
}

void PatchLoadedModules()
{
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32 moduleEntry = {};
    moduleEntry.dwSize = sizeof(moduleEntry);

    if (Module32First(snapshot, &moduleEntry))
    {
        do
        {
            PatchModule(moduleEntry.hModule);
        } while (Module32Next(snapshot, &moduleEntry));
    }

    CloseHandle(snapshot);
}

} // namespace aisp

using namespace aisp;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        PatchLoadedModules();
        // The screen hooks only concern the game executable's own imports (the ATL host is
        // linked into it); other modules keep the real functions.
        InitScreenBase();
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(HookCoCreateInstance), &g_originalCoCreateInstance);
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "OleDraw", reinterpret_cast<void*>(HookOleDraw), &g_originalOleDraw);
    }
    return TRUE;
}
