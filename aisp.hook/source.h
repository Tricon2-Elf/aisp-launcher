// The sources a screen can play. The page names one in its title as src=<prefix><rest>; the
// prefix picks the source, which runs on the session thread and fills the stream's rings until
// stop is set. Adding a source is a new file with a run function and a row in source.cpp.
#pragma once

#include "screen.h"

namespace aisp
{
struct ScreenSource
{
    const wchar_t* prefix;               // matched case-insensitively at the start of the source
    DWORD (*run)(ScreenStream* stream);  // returns when the source ends or stop is set
};

const ScreenSource* FindSource(const wchar_t* source);
bool IsKnownSource(const wchar_t* source);

// The off-screen browser: it paints latest frames instead of filling the ring, and has no PCM
// tap, so its volume/rolloff fader is a `gain` line on the control pipe, applied inside the page.
inline bool IsBrowserSource(const wchar_t* source)
{
    return source && _wcsnicmp(source, L"electron:", 9) == 0;
}

// source_ffmpeg.cpp: streamlink:<url> through streamlink into ffmpeg, stream:<url> straight
// into ffmpeg, yt-dlp:<url> resolved by yt-dlp to direct media URLs and seeked to the shared
// timeline position; one AVI stream of raw BGRA frames and float samples on a pipe.
DWORD RunFfmpegSource(ScreenStream* stream);
// source_pattern.cpp: pattern:live and pattern:vod, the hook's own calibration picture and test
// tone; the vod follows the page's shared timeline.
DWORD RunPatternSource(ScreenStream* stream);
// source_electron.cpp: electron:<http(s) url> in a separate off-screen Electron process
// (aisp.electron\electron.exe, stock Chromium). crop:sw/sh is the browser's layout viewport
// (otherwise the video box); the box-sized window at cx,cy of that is what we overlay.
// scrollx/scrolly pan the document; scale= zooms Chromium.
DWORD RunElectronSource(ScreenStream* stream);
} // namespace aisp
