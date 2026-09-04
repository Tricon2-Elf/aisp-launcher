// Source: streamlink and ffmpeg. streamlink:<url> pipes a transport stream from streamlink into
// ffmpeg (yt-dlp resolving a playlist URL as the fallback), stream:<url> lets ffmpeg open the
// URL itself. ffmpeg letterboxes into the video box at a constant rate and writes one AVI
// stream to its stdout: raw BGRA frames and float samples at the device's mix format,
// interleaved in timestamp order. One pipe, one reader: a second output on a named pipe used to
// stall ffmpeg on Windows for MPEG-TS inputs before the first picture, and two independent
// outputs let audio run far ahead of video, or behind it, which deadlocked the rings. Jitter is
// absorbed on the compressed side (streamlink's buffer, ffmpeg paced at real time), the rings
// stay short. Which site an id belongs to is the server's business: it hands the page the
// final URL.
//
// Tools: a streamlink install in the game directory (streamlink\bin\streamlink.exe and its
// bundled streamlink\ffmpeg\ffmpeg.exe) and yt-dlp\yt-dlp.exe there; AISP_STREAMLINK, AISP_YTDLP and
// AISP_FFMPEG override the paths.
#include "source.h"

#include <strsafe.h>
#include <cstring>
#include <cwchar>

namespace aisp
{
namespace
{
// Skips `size` bytes of the pipe (no seeking on a pipe).
bool SkipBytes(HANDLE pipe, DWORD size, volatile LONG* stop)
{
    BYTE scratch[4096];
    while (size > 0)
    {
        const DWORD step = size < sizeof(scratch) ? size : sizeof(scratch);
        if (!ReadFully(pipe, scratch, step, stop))
            return false;
        size -= step;
    }
    return true;
}

// Reads the AVI ffmpeg writes to a pipe (RIFF sizes are placeholders, so only the chunk walk
// is used): RIFF and LIST containers are entered, 'strh' headers say which streams exist,
// '00dc' chunks are frames of exactly frameBytes, '01wb' chunks are interleaved float samples,
// everything else (headers, JUNK, index chunks) is skipped. Chunks are padded to even sizes.
LONGLONG ReadAvi(ScreenStream* stream, HANDLE pipe)
{
    LONGLONG frames = 0;
    BYTE* frame = new BYTE[stream->frameBytes];
    const UINT32 bytesPerSample = stream->channels * sizeof(float);
    float* samples = nullptr;
    size_t samplesCapacity = 0;
    size_t pending = 0; // bytes of a partial sample carried between audio chunks
    bool audioSeen = false, headersDone = false;
    BYTE header[12] = {};
    while (!stream->stop && ReadFully(pipe, header, 8, &stream->stop))
    {
        const DWORD size = header[4] | (header[5] << 8) | (header[6] << 16) | (static_cast<DWORD>(header[7]) << 24);
        const DWORD padded = size + (size & 1);
        if (std::memcmp(header, "RIFF", 4) == 0 || std::memcmp(header, "LIST", 4) == 0)
        {
            if (!ReadFully(pipe, header + 8, 4, &stream->stop))
                break;
            if (std::memcmp(header + 8, "movi", 4) == 0 && !headersDone)
            {
                // The headers are complete: with an audio stream, the device becomes the clock.
                headersDone = true;
                if (audioSeen && stream->audioWanted)
                {
                    EnterCriticalSection(&stream->lock);
                    stream->audioActive = true;
                    LeaveCriticalSection(&stream->lock);
                    stream->audioRenderThread = CreateThread(nullptr, 0, AudioRenderThread, stream, 0, nullptr);
                }
            }
            continue; // enter the container
        }
        if (std::memcmp(header, "strh", 4) == 0 && size >= 4)
        {
            BYTE type[4] = {};
            if (!ReadFully(pipe, type, 4, &stream->stop) || !SkipBytes(pipe, padded - 4, &stream->stop))
                break;
            if (std::memcmp(type, "auds", 4) == 0)
                audioSeen = true;
            continue;
        }
        if (std::memcmp(header, "00dc", 4) == 0 && size == stream->frameBytes)
        {
            if (!ReadFully(pipe, frame, size, &stream->stop) || !SkipBytes(pipe, padded - size, &stream->stop))
                break;
            if (!PushVideoFrame(stream, frame))
                break;
            ++frames;
            continue;
        }
        if (std::memcmp(header, "01wb", 4) == 0 && stream->audioActive && bytesPerSample)
        {
            const size_t needed = pending + size;
            if (needed > samplesCapacity * sizeof(float))
            {
                float* grown = new float[(needed + sizeof(float) - 1) / sizeof(float)];
                if (pending)
                    std::memcpy(grown, samples, pending);
                delete[] samples;
                samples = grown;
                samplesCapacity = (needed + sizeof(float) - 1) / sizeof(float);
            }
            if (!ReadFully(pipe, reinterpret_cast<BYTE*>(samples) + pending, size, &stream->stop) || !SkipBytes(pipe, padded - size, &stream->stop))
                break;
            const size_t whole = needed / bytesPerSample;
            if (whole && !PushAudioSamples(stream, samples, static_cast<UINT32>(whole)))
                break;
            pending = needed - whole * bytesPerSample;
            if (pending)
                std::memmove(samples, reinterpret_cast<BYTE*>(samples) + whole * bytesPerSample, pending);
            continue;
        }
        if (!SkipBytes(pipe, padded, &stream->stop))
            break;
    }
    delete[] samples;
    delete[] frame;
    return frames;
}
} // namespace

bool RunFfmpegOnce(ScreenStream* stream, LONGLONG* frames);
constexpr wchar_t kHttpReconnect[] = L"-reconnect 1 -reconnect_on_network_error 1 -reconnect_streamed 1 -reconnect_delay_max 10 ";

DWORD RunFfmpegSource(ScreenStream* stream)
{
    // A video loops: when it ends on its own, it starts again where the timeline says.
    for (;;)
    {
        LONGLONG frames = 0;
        const bool ended = RunFfmpegOnce(stream, &frames);
        // Only a run that showed something loops; a broken URL would otherwise spin.
        if (!ended || stream->stop || frames == 0 || _wcsnicmp(stream->source, L"yt-dlp:", 7) != 0)
            return 0;
        SetStatus(stream, L"video ended; looping");
        Sleep(200);
    }
}

// One run; true when the media ended by itself (as opposed to a failure or a stop).
bool RunFfmpegOnce(ScreenStream* stream, LONGLONG* frames)
{
    wchar_t ffmpeg[MAX_PATH] = {}, streamlink[MAX_PATH] = {}, ytdlp[MAX_PATH] = {};
    wchar_t command[4096] = {};
    wchar_t message[512] = {};

    if (!ToolPath(L"AISP_FFMPEG", L"streamlink\\ffmpeg\\ffmpeg.exe", ffmpeg, MAX_PATH))
    {
        StringCchPrintfW(message, 512, L"ffmpeg not found: %s", ffmpeg);
        SetStatus(stream, message);
        return false;
    }

    // Decide what ffmpeg reads: a piped transport stream from streamlink, or a URL.
    HANDLE ffmpegInput = nullptr;
    wchar_t input[2048] = {};
    wchar_t inputArgs[4600]; // "-i ..." for every input, with a seek where the source supports it
    inputArgs[0] = L'\0';
    int audioInput = 0;             // which input carries the audio track
    // streamlink:<url> goes through streamlink into ffmpeg; stream:<url> straight into ffmpeg.
    // Which site an id belongs to is the server's business: it hands the page the final URL.
    const bool viaStreamlink = _wcsnicmp(stream->source, L"streamlink:", 11) == 0;
    if (viaStreamlink)
    {
        const wchar_t* pageUrl = stream->source + 11;
        if (ToolPath(L"AISP_STREAMLINK", L"streamlink\\bin\\streamlink.exe", streamlink, MAX_PATH))
        {
            StringCchPrintfW(message, 512, L"streamlink: %s", pageUrl);
            SetStatus(stream, message);
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreateInheritablePipe(&readEnd, &writeEnd, false))
            {
                SetStatus(stream, L"pipe creation failed");
                return false;
            }
            // No low-latency mode: it chases the live edge with a two segment window and delivers
            // in stalls and bursts, which is what the screen shows; the default edge and a large
            // ring buffer on streamlink's side keep the delivery steady at a few seconds' delay.
            // Ads are skipped by default in current streamlink; the resolution cap applies where
            // stream names carry one (Twitch) and is ignored elsewhere. streamlink is told where
            // ffmpeg is: sites that serve audio and video as separate variants (Nico Live) need
            // it to mux them, and hand out a silent video-only stream otherwise.
            StringCchPrintfW(command, 4096, L"\"%s\" --stdout --ringbuffer-size 64M --ffmpeg-ffmpeg \"%s\" --stream-sorting-excludes \">=720p\" \"%s\" best", streamlink, ffmpeg, pageUrl);
            stream->processes[1] = LaunchTool(command, nullptr, writeEnd);
            CloseHandle(writeEnd);
            if (!stream->processes[1])
            {
                CloseHandle(readEnd);
                SetStatus(stream, L"streamlink failed to start (see aisp.screen.log)");
                return false;
            }
            // streamlink was started while the read end was not inheritable, so it holds only
            // its write end; ffmpeg must now inherit the read end as its stdin.
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            ffmpegInput = readEnd;
            StringCchCopyW(input, 2048, L"pipe:0");
        }
        else if (ToolPath(L"AISP_YTDLP", L"yt-dlp\\yt-dlp.exe", ytdlp, MAX_PATH))
        {
            StringCchPrintfW(message, 512, L"yt-dlp: resolving %s", pageUrl);
            SetStatus(stream, message);
            StringCchPrintfW(command, 4096, L"\"%s\" -g --no-warnings -f \"best[height<=480]/best\" \"%s\"", ytdlp, pageUrl);
            if (!RunToolForLine(command, input, 2048) || _wcsnicmp(input, L"http", 4) != 0)
            {
                SetStatus(stream, L"yt-dlp returned no playlist URL (offline? see aisp.screen.log)");
                return false;
            }
        }
        else
        {
            StringCchPrintfW(message, 512, L"neither %s nor %s found", streamlink, ytdlp);
            SetStatus(stream, message);
            return false;
        }
    }
    else if (_wcsnicmp(stream->source, L"stream:", 7) == 0)
    {
        StringCchCopyW(input, 2048, stream->source + 7);
        if (_wcsnicmp(input, L"http", 4) == 0)
            StringCchPrintfW(inputArgs, 4600, L"%s-i \"%s\"", kHttpReconnect, input);
    }
    else if (_wcsnicmp(stream->source, L"yt-dlp:", 7) == 0)
    {
        // A video: yt-dlp resolves direct media URLs (one muxed, or video and audio apart),
        // which support range requests, so ffmpeg seeks to the shared timeline's position
        // before reading a byte of the rest.
        const wchar_t* pageUrl = stream->source + 7;
        if (!ToolPath(L"AISP_YTDLP", L"yt-dlp\\yt-dlp.exe", ytdlp, MAX_PATH))
        {
            StringCchPrintfW(message, 512, L"yt-dlp not found: %s", ytdlp);
            SetStatus(stream, message);
            return false;
        }
        StringCchPrintfW(message, 512, L"yt-dlp: resolving %s", pageUrl);
        SetStatus(stream, message);
        // The media URLs (one, or video and audio apart) and then the duration, one per line.
        StringCchPrintfW(command, 4096, L"\"%s\" --no-warnings -f \"bv*[height<=480]+ba/b[height<=480]/b\" --print urls --print duration \"%s\"", ytdlp, pageUrl);
        wchar_t lines[3][2048] = {};
        const int count = RunToolForLines(command, lines[0], 2048, 3);
        int urlCount = 0;
        double duration = 0;
        for (int i = 0; i < count; ++i)
        {
            if (_wcsnicmp(lines[i], L"http", 4) == 0 && urlCount < 2)
                urlCount++;
            else if (i == count - 1)
                duration = wcstod(lines[i], nullptr);
        }
        if (urlCount == 0)
        {
            SetStatus(stream, L"yt-dlp returned no media URL (see aisp.screen.log)");
            return false;
        }
        stream->seekSeconds = TimelinePosition(stream, duration);
        // Media servers drop connections that read at playback pace for long (YouTube does
        // after a minute or so); ffmpeg then re-requests from the current byte on a new
        // connection instead of failing. The seek goes before the input, so it is an HTTP
        // range request rather than a decode-and-discard.
        wchar_t seek[160] = {};
        StringCchPrintfW(seek, 160, L"%s%s", kHttpReconnect, L"");
        if (stream->seekSeconds > 0)
            StringCchPrintfW(seek, 160, L"%s-ss %.3f ", kHttpReconnect, stream->seekSeconds);
        char note[160] = {};
        StringCchPrintfA(note, 160, "yt-dlp: %d url(s), duration %.1f s, seeking to %.1f s\r\n", urlCount, duration, stream->seekSeconds);
        LogLine(note);
        if (urlCount == 2)
        {
            StringCchPrintfW(inputArgs, 4600, L"%s-i \"%s\" %s-i \"%s\"", seek, lines[0], seek, lines[1]);
            audioInput = 1;
        }
        else
            StringCchPrintfW(inputArgs, 4600, L"%s-i \"%s\"", seek, lines[0]);
    }
    else
    {
        SetStatus(stream, L"ffmpeg source: expected streamlink:<url>, stream:<url> or yt-dlp:<url>");
        return false;
    }
    if (!inputArgs[0])
        StringCchPrintfW(inputArgs, 4600, L"-i \"%s\"", input);

    if (stream->stop)
        return false;

    SetStatus(stream, L"ffmpeg: starting");
    // ffmpeg describes its input and outputs in aisp.screen.log (a dozen lines per session,
    // progress stats off); AISP_FFMPEG_LOGLEVEL overrides, e.g. debug or warning.
    wchar_t logLevel[32] = {};
    if (GetEnvironmentVariableW(L"AISP_FFMPEG_LOGLEVEL", logLevel, 32) == 0 || !logLevel[0])
        StringCchCopyW(logLevel, 32, L"info");
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreateInheritablePipe(&outRead, &outWrite, false))
    {
        if (ffmpegInput)
            CloseHandle(ffmpegInput);
        SetStatus(stream, L"pipe creation failed");
        return false;
    }
    // Letterbox into the video box (the part of the crop the panel actually shows) at a constant
    // frame rate; -re makes ffmpeg consume its input at real time so streamlink's buffer and the
    // pipe, not our frame ring, absorb the source's bursts (a Twitch playlist refresh delivers
    // about two segments at once). Audio at the device's format; the '?' keeps ffmpeg going for
    // sources without an audio track, the AVI then simply has no audio stream and the video
    // clock is used. Both start at the stream's own zero so frame f pairs with sample
    // f * samplesPerFrame; the muxer interleaves them by timestamp.
    wchar_t audioOutput[256] = {};
    if (stream->audioWanted)
        StringCchPrintfW(audioOutput, 256, L" -map %d:a:0? -af \"aresample=async=1000:first_pts=0\" -ac %u -ar %u -c:a pcm_f32le", audioInput, stream->channels, stream->sampleRate);
    // With a crop the picture is letterboxed at the crop's render size instead and the box-sized
    // window at its origin is cut out; a window reaching past the render size is padded black
    // first (ffmpeg's crop filter refuses one that does not fit). exact=1 keeps odd origins on
    // subsampled inputs.
    const int w = stream->videoWidth, h = stream->videoHeight;
    const bool cropped = stream->crop[0] > 0;
    const int renderW = cropped ? stream->crop[0] : w, renderH = cropped ? stream->crop[1] : h;
    wchar_t cropFilter[160] = {};
    if (cropped)
    {
        const int cx = stream->crop[2], cy = stream->crop[3];
        const int padW = cx + w > renderW ? cx + w : renderW, padH = cy + h > renderH ? cy + h : renderH;
        wchar_t extend[64] = {};
        if (padW > renderW || padH > renderH)
            StringCchPrintfW(extend, 64, L"pad=%d:%d:0:0,", padW, padH);
        StringCchPrintfW(cropFilter, 160, L"%scrop=%d:%d:%d:%d:exact=1,", extend, w, h, cx, cy);
    }
    wchar_t fullCommand[8192] = {};
    StringCchPrintfW(
        fullCommand,
        8192,
        L"\"%s\" -hide_banner -loglevel %s -nostats -nostdin -fflags nobuffer -re %s -map 0:v:0 -vf \"scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,%sfps=%d:start_time=0\" -c:v rawvideo -pix_fmt bgra%s -f avi pipe:1",
        ffmpeg,
        logLevel,
        inputArgs,
        renderW,
        renderH,
        renderW,
        renderH,
        cropFilter,
        stream->fps,
        audioOutput
    );
    stream->processes[0] = LaunchTool(fullCommand, ffmpegInput, outWrite);
    CloseHandle(outWrite);
    if (ffmpegInput)
        CloseHandle(ffmpegInput);
    if (!stream->processes[0])
    {
        CloseHandle(outRead);
        SetStatus(stream, L"ffmpeg failed to start (see aisp.screen.log)");
        return false;
    }

    SetStatus(stream, L"ffmpeg: buffering");
    *frames = ReadAvi(stream, outRead);
    CloseHandle(outRead);
    if (!stream->stop)
        SetStatus(stream, L"stream ended (see aisp.screen.log)");
    return !stream->stop;
}
} // namespace aisp
