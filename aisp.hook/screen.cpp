// Shared screen state and helpers; see screen.h.
#include "screen.h"

#include <strsafe.h>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace aisp
{
ScreenStream* g_streams = nullptr;
CRITICAL_SECTION g_streamsLock;
HANDLE g_job = nullptr;
HANDLE g_toolLog = INVALID_HANDLE_VALUE;
bool g_screenVideoInitialised = false;
HANDLE g_watchdog = nullptr;
bool g_logStats = false;                 // AISP_SCREEN_STATS=1: the per-second queue line in aisp.screen.log

void DebugLog(const wchar_t* format, const wchar_t* arg)
{
    wchar_t line[2048] = {};
    if (SUCCEEDED(StringCchPrintfW(line, 2048, format, arg)))
        OutputDebugStringW(line);
}

bool BuildGameFilePath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount)
{
    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    return SUCCEEDED(StringCchCopyW(outPath, outPathCount, processPath)) && SUCCEEDED(StringCchCatW(outPath, outPathCount, fileName));
}


void LogLine(const char* text)
{
    if (g_toolLog == INVALID_HANDLE_VALUE)
        return;
    // Our own lines carry the UTC time; the tools' stderr lines land in between as they are.
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    char stamp[32] = {};
    StringCchPrintfA(stamp, 32, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    DWORD written = 0;
    WriteFile(g_toolLog, stamp, static_cast<DWORD>(std::strlen(stamp)), &written, nullptr);
    WriteFile(g_toolLog, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

// One line per second per running stream: queue depths and positions, to see where a stutter

void SetStatus(ScreenStream* stream, const wchar_t* text)
{
    EnterCriticalSection(&stream->lock);
    StringCchCopyW(stream->status, 256, text);
    LeaveCriticalSection(&stream->lock);
    DebugLog(L"aisp.hook: screen: %s\n", text);
}

// The tool path from the environment variable, or `fallback` relative to the game directory.
bool ToolPath(const wchar_t* variable, const wchar_t* fallback, wchar_t* out, size_t outCount)
{
    if (GetEnvironmentVariableW(variable, out, static_cast<DWORD>(outCount)) == 0 || !out[0])
    {
        if (!BuildGameFilePath(fallback, out, outCount))
            return false;
    }
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// Starts a child with the given standard handles (nullptr = the log file / nothing) and puts it
// in the job. The command line buffer is modified by CreateProcessW.
HANDLE LaunchTool(wchar_t* commandLine, HANDLE stdIn, HANDLE stdOut)
{
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdIn ? stdIn : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdOut ? stdOut : g_toolLog;
    startup.hStdError = g_toolLog;

    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        return nullptr;
    if (g_job)
        AssignProcessToJobObject(g_job, info.hProcess);
    CloseHandle(info.hThread);
    return info.hProcess;
}

bool CreateInheritablePipe(HANDLE* readEnd, HANDLE* writeEnd, bool inheritRead)
{
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(readEnd, writeEnd, &inheritable, 1 << 20))
        return false;
    // Only the end the child uses may be inherited, or the pipe never reports EOF.
    SetHandleInformation(inheritRead ? *writeEnd : *readEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}

double UnixNow()
{
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG ticks = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return static_cast<double>(ticks - 116444736000000000ULL) / 1e7;
}

double TimelinePosition(ScreenStream* stream, double duration)
{
    EnterCriticalSection(&stream->lock);
    const double start = stream->pageStart, offset = stream->pageOffset, pausedAt = stream->pagePausedAt;
    LeaveCriticalSection(&stream->lock);
    if (start <= 0)
        return 0;
    // The position stops advancing at the pause time, not at the last start.
    const double elapsed = (pausedAt > 0 ? pausedAt : UnixNow()) - start;
    double position = offset + (elapsed > 0 ? elapsed : 0);
    if (duration > 0)
        position = std::fmod(position, duration);
    return position > 0 ? position : 0;
}

bool ReadFully(HANDLE pipe, BYTE* buffer, DWORD size, volatile LONG* stop)
{
    DWORD total = 0;
    while (total < size)
    {
        if (*stop)
            return false;
        DWORD read = 0;
        if (!ReadFile(pipe, buffer + total, size - total, &read, nullptr) || read == 0)
            return false;
        total += read;
    }
    return true;
}

bool PushVideoFrame(ScreenStream* stream, const BYTE* frame)
{
    EnterCriticalSection(&stream->lock);
    // Keep the frame on screen (videoPos - 1) intact: wait for room. Not while playback is
    // waiting on audio, though: a source whose audio starts later than its video (Nico Live
    // runs three seconds apart) would fill the ring, block ffmpeg on its video output before it
    // reaches the audio packets, and never start. The oldest frame goes instead; the presenter
    // aligns the audio to whatever is left when it starts.
    while (!stream->stop && stream->videoWritten - stream->videoPos >= stream->capacity - 2)
    {
        const bool audioNotDriving = stream->audioActive && !stream->playing
            && stream->audioWritten - stream->audioPos < static_cast<LONGLONG>(PrerollFor(stream->capacity, stream->fps)) * stream->samplesPerFrame;
        if (audioNotDriving)
        {
            ++stream->videoPos;
            ++stream->videoDrops;
            break;
        }
        ++stream->videoWaits;
        LeaveCriticalSection(&stream->lock);
        Sleep(5);
        EnterCriticalSection(&stream->lock);
    }
    if (stream->stop)
    {
        LeaveCriticalSection(&stream->lock);
        return false;
    }
    std::memcpy(stream->ring + static_cast<size_t>(stream->videoWritten % stream->capacity) * stream->frameBytes, frame, stream->frameBytes);
    ++stream->videoWritten;
    LeaveCriticalSection(&stream->lock);
    return true;
}

bool PushAudioSamples(ScreenStream* stream, const float* samples, UINT32 count)
{
    EnterCriticalSection(&stream->lock);
    // Never overwrite samples the device has not taken yet: wait for room. The mirror of the
    // rule in PushVideoFrame: before playback starts, audio arrives long before the first
    // picture when the video decoder has reordering depth, and a full ring would block ffmpeg's
    // audio output, and with it its demuxer, before the first frame is ever decoded. The
    // oldest samples go instead; the presenter aligns the two when it starts.
    while (!stream->stop && stream->audioWritten + count - stream->audioPos > stream->audioCapacity - count)
    {
        const bool videoNotDriving = !stream->playing
            && stream->videoWritten - stream->videoPos < PrerollFor(stream->capacity, stream->fps);
        if (videoNotDriving)
        {
            const LONGLONG keep = stream->audioCapacity - 2 * static_cast<LONGLONG>(count);
            const LONGLONG newest = stream->audioWritten - (keep > 0 ? keep : 0);
            if (newest > stream->audioPos)
            {
                stream->audioDrops += static_cast<LONG>(newest - stream->audioPos);
                stream->audioPos = newest;
            }
            break;
        }
        ++stream->audioWaits;
        LeaveCriticalSection(&stream->lock);
        Sleep(5);
        EnterCriticalSection(&stream->lock);
    }
    if (stream->stop)
    {
        LeaveCriticalSection(&stream->lock);
        return false;
    }
    for (UINT32 i = 0; i < count; ++i)
    {
        const LONGLONG slot = (stream->audioWritten + i) % stream->audioCapacity;
        std::memcpy(stream->audioRing + slot * stream->channels, samples + static_cast<size_t>(i) * stream->channels, stream->channels * sizeof(float));
    }
    stream->audioWritten += count;
    LeaveCriticalSection(&stream->lock);
    return true;
}
} // namespace aisp
