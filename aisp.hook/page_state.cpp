// What the screen page is told about its stream: pushed into the primary Electron's page as
// `window.aisp`, an object merged on every push, with `window.onAisp(state)` called when the
// page defines it and an "aisp" CustomEvent on document (detail: the object). One way, for the
// page to show or decide by; the page's own title stays the way it talks back.
//
//   source     the src= the page named ("yt-dlp:https://…", "electron:https://…", …)
//   kind       its prefix: yt-dlp, streamlink, stream, electron, pattern
//   url        what follows the prefix, for the URL-shaped kinds
//   title      the video's title as yt-dlp knows it, or a browser secondary's own document.title
//              as it changes, or the pattern's name; "" when nothing is known
//   duration   seconds; 0 for a live stream or when unknown
//   position   seconds into the media (wraps with the loop); for a live stream or a browser,
//              seconds since the session started
//   elapsed    seconds since the session started
//   playing    frames are being presented (false while buffering or held)
//   paused     the page's timeline pause or hold is in effect
//   active     a session runs at all (false: the page shows itself)
//   status     the hook's status line for the session ("ffmpeg: buffering", …)
//   fps        the session's frame rate
//   box        [x, y, w, h] of the video inside the crop; crop [sw, sh, cx, cy] or [] when none
//
// Sent at most twice a second, and only when something in it changed; a few hundred bytes.
#include "browser.h"

#include <strsafe.h>
#include <cmath>
#include <cstring>
#include <string>

namespace aisp
{
namespace
{
constexpr ULONGLONG kPushIntervalMs = 500;
constexpr ULONGLONG kLogIntervalMs = 5000; // [screens] stats: the object, this often at most

void AppendJsonString(std::string& out, const wchar_t* text)
{
    char utf8[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    out += '"';
    for (const char* p = utf8; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += *p;
        }
        else if (c < 0x20)
        {
            char escaped[8] = {};
            StringCchPrintfA(escaped, 8, "\\u%04x", c);
            out += escaped;
        }
        else
            out += *p;
    }
    out += '"';
}

void AppendNumber(std::string& out, double value)
{
    char text[64] = {};
    if (std::fabs(value - std::floor(value)) < 1e-9 && std::fabs(value) < 1e15)
        StringCchPrintfA(text, 64, "%.0f", value);
    else
        StringCchPrintfA(text, 64, "%.3f", value);
    out += text;
}
} // namespace

void PushPageState(ScreenStream* stream)
{
    if (!stream->primaryActive || !stream->primaryControl)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - stream->statePushedAt < kPushIntervalMs)
        return;
    stream->statePushedAt = now;

    std::string json;
    json.reserve(1024);
    EnterCriticalSection(&stream->lock);
    const wchar_t* source = stream->source;
    const wchar_t* colon = std::wcschr(source, L':');
    wchar_t kind[32] = {};
    if (colon && colon - source < 31)
        StringCchCopyNW(kind, 32, source, colon - source);
    const bool urlKind = colon && (_wcsnicmp(source, L"yt-dlp:", 7) == 0 || _wcsnicmp(source, L"streamlink:", 11) == 0 || _wcsnicmp(source, L"stream:", 7) == 0 || _wcsnicmp(source, L"electron:", 9) == 0);
    const bool active = stream->sessionActive;
    const double elapsed = active ? static_cast<double>(now - stream->sessionStarted) / 1000.0 : 0;
    double position = 0;
    if (active)
    {
        if (stream->liveVideo || stream->fps <= 0)
            position = elapsed;
        else
        {
            const LONGLONG presented = stream->videoPos - stream->runStartFrame;
            position = stream->runSeek + (presented > 0 ? static_cast<double>(presented) / stream->fps : 0);
            if (stream->mediaDuration > 0)
                position = std::fmod(position, stream->mediaDuration);
        }
    }
    json += "{\"source\":";
    AppendJsonString(json, source);
    json += ",\"kind\":";
    AppendJsonString(json, kind);
    json += ",\"url\":";
    AppendJsonString(json, urlKind ? colon + 1 : L"");
    json += ",\"title\":";
    AppendJsonString(json, active ? stream->mediaTitle : L"");
    json += ",\"duration\":";
    AppendNumber(json, active ? stream->mediaDuration : 0);
    json += ",\"position\":";
    AppendNumber(json, std::floor(position * 10) / 10);
    json += ",\"elapsed\":";
    AppendNumber(json, std::floor(elapsed * 10) / 10);
    json += ",\"playing\":";
    json += active && stream->playing ? "true" : "false";
    json += ",\"paused\":";
    json += stream->paused ? "true" : "false";
    json += ",\"active\":";
    json += active ? "true" : "false";
    json += ",\"status\":";
    AppendJsonString(json, active ? stream->status : L"");
    json += ",\"fps\":";
    AppendNumber(json, stream->fps);
    char geometry[160] = {};
    if (stream->crop[0] > 0)
        StringCchPrintfA(geometry, 160, ",\"box\":[%d,%d,%d,%d],\"crop\":[%d,%d,%d,%d]}", stream->videoX, stream->videoY, stream->videoWidth, stream->videoHeight, stream->crop[0], stream->crop[1], stream->crop[2], stream->crop[3]);
    else
        StringCchPrintfA(geometry, 160, ",\"box\":[%d,%d,%d,%d],\"crop\":[]}", stream->videoX, stream->videoY, stream->videoWidth, stream->videoHeight);
    json += geometry;
    const bool same = std::strcmp(json.c_str(), stream->stateSent) == 0;
    if (!same)
        StringCchCopyA(stream->stateSent, sizeof(stream->stateSent), json.c_str());
    const bool logIt = g_logStats && now - stream->stateLoggedAt >= kLogIntervalMs;
    if (logIt)
        stream->stateLoggedAt = now;
    LeaveCriticalSection(&stream->lock);
    if (same)
        return;

    std::string script("(function(s){var w=window;w.aisp=Object.assign(w.aisp||{},s);try{if(typeof w.onAisp===\"function\")w.onAisp(w.aisp)}catch(e){}try{document.dispatchEvent(new CustomEvent(\"aisp\",{detail:w.aisp}))}catch(e){}})(");
    script += json;
    script += ")";
    SendPrimaryEval(stream, script.c_str());
    if (logIt)
    {
        std::string line("primary state: ");
        line += json;
        line += "\r\n";
        LogLine(line.c_str());
    }
}
} // namespace aisp
