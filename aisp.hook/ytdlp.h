// yt-dlp resolution with a cache: a video's media URLs and its duration, kept on disk under the
// game's aisp.launch.data (aisp.cache\yt-dlp\<hash of the page URL>.txt) and reused across session
// starts, loops and game runs. The duration is what makes a video loop in step with the shared
// timeline, and it never changes; the media URLs are good until the site's expiry (YouTube's
// expire= in the URL, else a short default), and a refresh runs in the background before a
// playing video reaches its loop point with URLs about to lapse, so the restart never waits.
#pragma once

#include "screen.h"

namespace aisp
{
struct YtdlpInfo
{
    double duration = 0;                 // seconds; 0 when the site gives none (live)
    double expires = 0;                  // unix seconds the media URLs are good until; 0 unknown
    int urlCount = 0;                    // one muxed, or video then audio
    wchar_t urls[2][2048] = {};
    wchar_t title[512] = {};             // the video's title as the site has it
    bool fromCache = false;
};

// The media URLs, duration and title for a page URL: from the cache while its URLs are valid,
// else by running yt-dlp (seconds) and storing the result. `stream` gets the status line meanwhile.
bool ResolveYtdlp(ScreenStream* stream, const wchar_t* pageUrl, YtdlpInfo& out);
// Runs yt-dlp for the page URL on a background thread when the cached media URLs lapse within
// `withinSeconds`, so the next resolve (the loop restart) finds fresh ones. No-op otherwise.
void RefreshYtdlpIfExpiring(const wchar_t* pageUrl, double withinSeconds);
} // namespace aisp
