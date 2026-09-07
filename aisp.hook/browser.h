// Off-screen Electron as the primary in-game screen browser: paint, title and page instead of
// ieframe. The game still hosts an IE WebBrowser, but with this path on it is never navigated:
// OleDraw of it presents Chromium's frames, and the document the client takes from it (for
// statusForm and its ext_* scripts) is the hook's own, answered from Electron's page
// (document.h). A second Electron (electron:) or ffmpeg still composites into the video box —
// sites block iframes, and streams need a real decoder.
#pragma once

#include "screen.h"

namespace aisp
{
// Whether ntdll exports wine_get_version; cached on the first call.
bool IsRunningOnWine();
// [screens] primary_browser (or AISP_PRIMARY_BROWSER): electron or ie. Default: electron.
bool UsePrimaryBrowser();
void InitBrowserMode();

// Named pipe on Windows, SOCKET on Wine. Wine must not CloseHandle a socket, and its WriteFile
// on a socket never returns, so writes go through send there.
void CloseBrowserChannel(HANDLE handle, bool tcp);
bool WriteBrowserChannel(HANDLE handle, bool tcp, const void* data, size_t length);

// Starts the off-screen host. Windows: aisp.electron\electron.exe + named pipes. Wine: listen on
// 127.0.0.1 TCP and ask the native broker (AISP_ELECTRON_NATIVE, default 127.0.0.1:18764) to
// spawn stock Linux Electron with the same app. `framed` makes the video channel carry headed
// messages (frames, title lines, call replies) instead of bare frames.
struct ElectronSessionRequest
{
    const wchar_t* url = nullptr;
    int width = 0, height = 0, fps = 0;
    int scrollx = 0, scrolly = 0, hideScroll = 0;
    float scale = 1.0f, gain = 1.0f;
    int mute = 0;
    bool framed = false;
    volatile LONG* stop = nullptr;
    HANDLE* outControl = nullptr;
    HANDLE* outVideo = nullptr;
    HANDLE* outProcess = nullptr;
    bool* outTcp = nullptr;
};
bool StartElectronSession(const ElectronSessionRequest& request, wchar_t* error, size_t errorCount);

// One off-screen Electron on the rewritten screen URL, crop-sized, reporting document.title.
void StartPrimaryBrowser(ScreenStream* stream, const wchar_t* url);
void StopPrimaryBrowser(ScreenStream* stream);
void FreePrimary(ScreenStream* stream);
void SendPrimaryControl(ScreenStream* stream);
void ForwardScriptToPrimary(ScreenStream* stream, const wchar_t* script);
// Evaluates `script` (UTF-8) in the primary's page and waits for the value: the text of a
// string, the literal of anything else. False on null, an error, a timeout, or no primary.
// `loading`, when given, is set when the host answered at once that its page is still loading
// (no wait was spent), so the caller can tell that apart from a page without the value.
bool CallPrimary(ScreenStream* stream, const char* script, char* out, size_t outCount, DWORD timeoutMs, bool* loading = nullptr);
// Latest primary paint into the crop rectangle. Caller holds stream->lock.
bool BlitPrimaryPage(ScreenStream* stream, HDC hdc, int destX, int destY);
// Counts a client page read answered from Electron ([screens] stats), and the periodic line.
void CountPrimaryRead(ScreenStream* stream, const wchar_t* id, double waitMs, bool ok);
void LogPrimaryStats(ScreenStream* stream);
} // namespace aisp
