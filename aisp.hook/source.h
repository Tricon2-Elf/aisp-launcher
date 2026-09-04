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

// source_ffmpeg.cpp: streamlink:<url> through streamlink into ffmpeg, stream:<url> straight
// into ffmpeg; one AVI stream of raw BGRA frames and float samples on a pipe.
DWORD RunFfmpegSource(ScreenStream* stream);
// source_pattern.cpp: pattern:live and pattern:vod, the hook's own calibration picture and test
// tone; the vod follows the page's shared timeline.
DWORD RunPatternSource(ScreenStream* stream);
} // namespace aisp
