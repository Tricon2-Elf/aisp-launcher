// See ytdlp.h.
#include "ytdlp.h"

#include <strsafe.h>
#include <cstring>
#include <cwchar>
#include <cstdlib>

namespace aisp
{
namespace
{
constexpr double kDurationTtlSeconds = 7 * 24 * 3600.0;  // a cached duration is trusted this long
constexpr double kDefaultUrlTtlSeconds = 600.0;          // media URLs without a visible expiry
constexpr double kExpiryMarginSeconds = 300.0;           // treat URLs as lapsed this early
constexpr int kMaxInFlight = 16;                          // background refreshes at once

CRITICAL_SECTION g_cacheLock;
bool g_cacheLockReady = false;
unsigned long long g_inFlight[kMaxInFlight] = {};

void EnsureLock()
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_cacheLock);
        g_cacheLockReady = true;
    }
    while (!g_cacheLockReady)
        Sleep(1);
}

unsigned long long HashUrl(const wchar_t* url)
{
    // FNV-1a over the UTF-16 code units, case as given: the same page URL is the same video.
    unsigned long long hash = 14695981039346656037ULL;
    for (const wchar_t* p = url; *p; ++p)
    {
        hash ^= static_cast<unsigned long long>(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool CachePath(const wchar_t* pageUrl, wchar_t* out, size_t outCount, bool create)
{
    wchar_t dir[MAX_PATH] = {};
    if (!BuildGameFilePath(L"aisp.cache", dir, MAX_PATH))
        return false;
    if (create)
        CreateDirectoryW(dir, nullptr);
    if (FAILED(StringCchCatW(dir, MAX_PATH, L"\\yt-dlp")))
        return false;
    if (create)
        CreateDirectoryW(dir, nullptr);
    return SUCCEEDED(StringCchPrintfW(out, outCount, L"%s\\%016llx.txt", dir, HashUrl(pageUrl)));
}

// The site's own expiry in a media URL: YouTube puts expire=<unix seconds> in the query.
double UrlExpiry(const wchar_t* url)
{
    for (const wchar_t* p = url; (p = std::wcsstr(p, L"expire=")) != nullptr; ++p)
    {
        if (p == url || p[-1] == L'?' || p[-1] == L'&')
        {
            const double value = wcstod(p + 7, nullptr);
            if (value > 1e9 && value < 1e11)
                return value;
        }
    }
    return 0;
}

bool ReadCache(const wchar_t* pageUrl, YtdlpInfo& info, double* fetched)
{
    wchar_t path[MAX_PATH] = {};
    if (!CachePath(pageUrl, path, MAX_PATH, false))
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char buffer[8192] = {};
    DWORD total = 0, read = 0;
    while (total < sizeof(buffer) - 1 && ReadFile(file, buffer + total, sizeof(buffer) - 1 - total, &read, nullptr) && read > 0)
        total += read;
    CloseHandle(file);
    buffer[total] = '\0';

    info = YtdlpInfo();
    *fetched = 0;
    bool header = false, sameUrl = false;
    char* cursor = buffer;
    while (*cursor)
    {
        char* end = std::strpbrk(cursor, "\r\n");
        if (end)
            *end = '\0';
        if (!header)
            header = std::strcmp(cursor, "aisp yt-dlp cache 1") == 0;
        else if (std::strncmp(cursor, "url ", 4) == 0)
        {
            wchar_t url[4096] = {};
            sameUrl = MultiByteToWideChar(CP_UTF8, 0, cursor + 4, -1, url, 4096) > 0 && std::wcscmp(url, pageUrl) == 0;
        }
        else if (std::strncmp(cursor, "fetched ", 8) == 0)
            *fetched = std::strtod(cursor + 8, nullptr);
        else if (std::strncmp(cursor, "duration ", 9) == 0)
            info.duration = std::strtod(cursor + 9, nullptr);
        else if (std::strncmp(cursor, "expires ", 8) == 0)
            info.expires = std::strtod(cursor + 8, nullptr);
        else if (std::strncmp(cursor, "title ", 6) == 0)
            MultiByteToWideChar(CP_UTF8, 0, cursor + 6, -1, info.title, 512);
        else if (std::strncmp(cursor, "media ", 6) == 0 && info.urlCount < 2)
        {
            if (MultiByteToWideChar(CP_UTF8, 0, cursor + 6, -1, info.urls[info.urlCount], 2048) > 0)
                ++info.urlCount;
        }
        if (!end)
            break;
        cursor = end + 1;
        while (*cursor == '\r' || *cursor == '\n')
            ++cursor;
    }
    if (!header || !sameUrl)
        return false;
    info.fromCache = true;
    return true;
}

void WriteCache(const wchar_t* pageUrl, const YtdlpInfo& info)
{
    wchar_t path[MAX_PATH] = {}, temp[MAX_PATH] = {};
    if (!CachePath(pageUrl, path, MAX_PATH, true) || FAILED(StringCchPrintfW(temp, MAX_PATH, L"%s.%lu.tmp", path, GetCurrentThreadId())))
        return;
    char text[8192] = {};
    char utf8[4096] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, pageUrl, -1, utf8, sizeof(utf8), nullptr, nullptr) <= 0)
        return;
    StringCchPrintfA(text, sizeof(text), "aisp yt-dlp cache 1\nurl %s\nfetched %.0f\nduration %.3f\nexpires %.0f\n", utf8, UnixNow(), info.duration, info.expires);
    if (info.title[0] && WideCharToMultiByte(CP_UTF8, 0, info.title, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
    {
        StringCchCatA(text, sizeof(text), "title ");
        StringCchCatA(text, sizeof(text), utf8);
        StringCchCatA(text, sizeof(text), "\n");
    }
    for (int i = 0; i < info.urlCount; ++i)
    {
        if (WideCharToMultiByte(CP_UTF8, 0, info.urls[i], -1, utf8, sizeof(utf8), nullptr, nullptr) <= 0)
            return;
        StringCchCatA(text, sizeof(text), "media ");
        StringCchCatA(text, sizeof(text), utf8);
        StringCchCatA(text, sizeof(text), "\n");
    }
    // Whole file or nothing: written aside, then moved over whatever is there.
    HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    const bool ok = WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr) && written == std::strlen(text);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(temp);
}

// Runs yt-dlp: the media URLs (one muxed, or video and audio apart) one per line, then one line
// of "<duration>\t<title>" (one template, so an empty title cannot shift the lines).
bool RunYtdlp(const wchar_t* pageUrl, YtdlpInfo& info, wchar_t* error, size_t errorCount)
{
    wchar_t ytdlp[MAX_PATH] = {};
    if (!ToolPath(L"AISP_YTDLP", L"ytdlp", kYtdlpFallback, kYtdlpFallbackLegacy, ytdlp, MAX_PATH))
    {
        StringCchPrintfW(error, errorCount, L"yt-dlp not found: %s", ytdlp);
        return false;
    }
    wchar_t command[4096] = {};
    StringCchPrintfW(command, 4096, L"\"%s\" --no-warnings -f \"bv*[height<=480]+ba/b[height<=480]/b\" --print urls --print \"%%(duration)s\t%%(title)s\" \"%s\"", ytdlp, pageUrl);
    wchar_t lines[3][2048] = {};
    const int count = RunToolForLines(command, lines[0], 2048, 3);
    info = YtdlpInfo();
    for (int i = 0; i < count; ++i)
    {
        if (_wcsnicmp(lines[i], L"http", 4) == 0 && info.urlCount < 2)
            StringCchCopyW(info.urls[info.urlCount++], 2048, lines[i]);
        else if (i == count - 1)
        {
            info.duration = wcstod(lines[i], nullptr); // "NA" for a live stream reads as 0
            if (const wchar_t* tab = std::wcschr(lines[i], L'\t'))
                StringCchCopyW(info.title, 512, tab + 1);
        }
    }
    if (info.urlCount == 0)
    {
        StringCchCopyW(error, errorCount, L"yt-dlp returned no media URL (see aisp.screen.log)");
        return false;
    }
    const double now = UnixNow();
    info.expires = now + kDefaultUrlTtlSeconds;
    for (int i = 0; i < info.urlCount; ++i)
    {
        const double expiry = UrlExpiry(info.urls[i]);
        if (expiry > 0 && (i == 0 || expiry < info.expires))
            info.expires = expiry;
    }
    WriteCache(pageUrl, info);
    return true;
}

bool UrlsValid(const YtdlpInfo& info, double now)
{
    return info.urlCount > 0 && info.expires > now + kExpiryMarginSeconds;
}

struct RefreshJob
{
    unsigned long long hash;
    wchar_t pageUrl[4096];
};

DWORD WINAPI RefreshThread(LPVOID parameter)
{
    RefreshJob* job = static_cast<RefreshJob*>(parameter);
    YtdlpInfo info;
    wchar_t error[512] = {};
    char note[256] = {};
    if (RunYtdlp(job->pageUrl, info, error, 512))
        StringCchPrintfA(note, 256, "yt-dlp: refreshed media URLs ahead of expiry (good for %.0f min)\r\n", (info.expires - UnixNow()) / 60);
    else
        StringCchPrintfA(note, 256, "yt-dlp: background refresh failed (%ls)\r\n", error);
    LogLine(note);
    EnsureLock();
    EnterCriticalSection(&g_cacheLock);
    for (int i = 0; i < kMaxInFlight; ++i)
        if (g_inFlight[i] == job->hash)
            g_inFlight[i] = 0;
    LeaveCriticalSection(&g_cacheLock);
    delete job;
    return 0;
}
} // namespace

bool ResolveYtdlp(ScreenStream* stream, const wchar_t* pageUrl, YtdlpInfo& out)
{
    wchar_t message[512] = {};
    const double now = UnixNow();
    double fetched = 0;
    YtdlpInfo cached;
    const bool have = ReadCache(pageUrl, cached, &fetched);
    if (have && UrlsValid(cached, now) && now - fetched < kDurationTtlSeconds)
    {
        out = cached;
        return true;
    }
    StringCchPrintfW(message, 512, L"yt-dlp: resolving %s", pageUrl);
    SetStatus(stream, message);
    if (RunYtdlp(pageUrl, out, message, 512))
        return true;
    // The duration outlives the URLs: a stale entry still says how long the video is, and a
    // failed run (offline) can at least seek by it once the media comes back another way.
    if (have && cached.duration > 0)
        out.duration = cached.duration;
    SetStatus(stream, message);
    return false;
}

void RefreshYtdlpIfExpiring(const wchar_t* pageUrl, double withinSeconds)
{
    double fetched = 0;
    YtdlpInfo cached;
    if (!ReadCache(pageUrl, cached, &fetched))
        return;
    if (cached.expires <= 0 || cached.expires - kExpiryMarginSeconds > UnixNow() + withinSeconds)
        return;
    const unsigned long long hash = HashUrl(pageUrl);
    EnsureLock();
    EnterCriticalSection(&g_cacheLock);
    int slot = -1;
    for (int i = 0; i < kMaxInFlight; ++i)
    {
        if (g_inFlight[i] == hash)
        {
            slot = -2; // already running
            break;
        }
        if (g_inFlight[i] == 0 && slot < 0)
            slot = i;
    }
    if (slot >= 0)
        g_inFlight[slot] = hash;
    LeaveCriticalSection(&g_cacheLock);
    if (slot < 0)
        return;
    RefreshJob* job = new RefreshJob();
    job->hash = hash;
    StringCchCopyW(job->pageUrl, 4096, pageUrl);
    HANDLE thread = CreateThread(nullptr, 0, RefreshThread, job, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
    {
        EnterCriticalSection(&g_cacheLock);
        g_inFlight[slot] = 0;
        LeaveCriticalSection(&g_cacheLock);
        delete job;
    }
}
} // namespace aisp
