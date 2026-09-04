// Shared state and helpers of the in-game screen playback: the per-screen stream object with its
// frame and sample rings, the constants that size them, and the helpers every source uses. A
// source (source.h) produces frames and samples into the rings; the hook side (aisp.hook.cpp)
// presents them in step with the audio device.
#pragma once

#include <windows.h>
#include <mshtml.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace aisp
{
constexpr int kDefaultFps = 30;           // ffmpeg's fps filter makes the video constant-rate; the page may pick another
// The decoded rings are small on purpose: jitter is absorbed on the compressed side (streamlink's
// lead and buffer, ffmpeg paced at real time). A frame has to wait in the ring only from the
// moment ffmpeg emits it next to its audio until that audio is actually heard, which is the
// audio device's 200 ms head start; twelve frames cover that with slack. (Three, the classic
// video-only queue, would block ffmpeg's video output before the audio thread had its lead.)
constexpr int kRingMs = 400;               // decoded frames kept ahead of the picture
constexpr int kPrerollMs = 200;            // start once this much is queued
constexpr int kResumeMs = 100;             // after an underrun, resume once this much is queued
constexpr size_t kRingBudgetBytes = 64u << 20; // per screen (a full Stage crop at 30 fps is 24 MB)
constexpr int kMinBufferFrames = 3;

inline int FramesFor(int ms, int fps)
{
    const int frames = ms * fps / 1000;
    return frames < 1 ? 1 : frames;
}

// Ring length for a frame size and rate: kRingMs if that fits the budget, fewer frames otherwise.
inline int RingCapacityFor(DWORD frameBytes, int fps)
{
    const int wanted = FramesFor(kRingMs, fps);
    const size_t fit = frameBytes ? kRingBudgetBytes / frameBytes : static_cast<size_t>(wanted);
    return static_cast<int>(fit < static_cast<size_t>(kMinBufferFrames) ? kMinBufferFrames : (fit > static_cast<size_t>(wanted) ? wanted : fit));
}

inline int PrerollFor(int capacity, int fps)
{
    const int wanted = FramesFor(kPrerollMs, fps);
    return wanted < capacity / 2 ? wanted : capacity / 2;
}

inline int ResumeFor(int capacity, int fps)
{
    const int wanted = FramesFor(kResumeMs, fps);
    return wanted < capacity / 3 ? wanted : capacity / 3;
}

// Rates the page may ask for: those that divide 44100 and 48000 evenly, so a frame is a whole
// number of samples and the audio clock maps to frames exactly.
inline bool IsSupportedFps(int fps)
{
    return fps == 15 || fps == 20 || fps == 25 || fps == 30 || fps == 50 || fps == 60;
}
// Full rings block their reader instead of dropping: ffmpeg then blocks on its pipe and
// streamlink's own buffer absorbs the segment bursts, so nothing is ever skipped and the
// latency is bounded by the ring. The audio ring is larger so it never blocks first.
constexpr int kTitlePollFrames = 15;       // read the page title for volume twice a second
constexpr ULONGLONG kAudioHoldMs = 300;    // no draw for this long: silence, nothing advances
constexpr ULONGLONG kIdleStopMs = 3000;    // no draw for this long: stop the source (its session)
constexpr ULONGLONG kIdleFreeMs = 60000;   // no draw for this long: release the rings too

struct ScreenStream
{
    IUnknown* browser = nullptr;         // identity of the WebBrowser this stream belongs to (held)
    IUnknown* document = nullptr;        // the document pointer OleDraw hands us for it (not held)
    IHTMLDocument2* html = nullptr;      // same document as IHTMLDocument2, for the title (held)
    int x = 0, y = 0, width = 0, height = 0; // crop rectangle inside the control
    int videoX = 0, videoY = 0, videoWidth = 0, videoHeight = 0; // where video goes inside the crop
    CRITICAL_SECTION lock;

    // Video ring: frame f lives at slot f % capacity for f in [videoPos, videoWritten).
    BYTE* ring = nullptr;
    DWORD frameBytes = 0;
    int capacity = 0;
    LONGLONG videoWritten = 0;
    LONGLONG videoPos = 0;               // next frame to present; videoPos - 1 is on screen
    bool playing = false;
    LONGLONG nextPresent = 0;            // video-only clock (QPC) when there is no audio
    int titlePoll = 0;
    int fps = kDefaultFps;               // constant output rate ffmpeg is told to produce
    int pageFps = 0;                     // fps=N from the page title (0: none given)
    wchar_t pageSource[512] = {};        // src=... from the page title, applied by OleDraw
    int pageBox[4] = {};                 // box=x,y,w,h from the page title (w == 0: none given)
    // crop=sw,sh,cx,cy from the page title (sw == 0: none): the source renders its picture at
    // sw x sh instead of the box size and the box shows the window starting at cx,cy of it.
    int pageCrop[4] = {};
    int crop[4] = {};                    // the crop the running session was started with
    // A video's shared timeline from the title: at start=<unix seconds> it was at offset=<s>
    // and playing; paused=<unix seconds> is when it stopped advancing. A source that can seek
    // starts at the position this implies (TimelinePosition); a change of start or offset
    // restarts the session there, a pause only holds. hold=1 is this screen's own pause (the
    // TV buttons).
    double pageStart = 0, pageOffset = 0, pagePausedAt = 0;
    bool pageHold = false;
    double sessionStart = 0, sessionOffset = 0; // the timeline the running session was started for
    double seekSeconds = 0;              // where the running session was told to begin
    bool paused = false;                 // presenter and renderer hold
    // Colour keying: when the page names a key colour, only the pixels of the page that are
    // exactly that colour receive video; anything else the page draws inside the box stays on
    // top (the overlay-surface trick of the DirectDraw era).
    bool keyed = false;
    DWORD keyColor = 0;                  // 0x00RRGGBB
    HDC keyDc = nullptr;
    HBITMAP keyBitmap = nullptr;
    HGDIOBJ keyOldBitmap = nullptr;
    BYTE* keyBits = nullptr;
    int keyWidth = 0, keyHeight = 0;

    // Audio ring (float32 interleaved): sample s at s % audioCapacity for s in [audioPos, audioWritten).
    bool audioWanted = false;            // a device was found; the source may deliver audio
    bool audioActive = false;            // the source delivers samples; audio is the clock
    UINT32 sampleRate = 0;
    UINT32 channels = 0;
    UINT32 samplesPerFrame = 0;
    float* audioRing = nullptr;
    LONGLONG audioCapacity = 0;          // in samples (frames of channels)
    LONGLONG audioWritten = 0;
    LONGLONG audioPos = 0;               // next sample the device takes
    LONGLONG audioPlayed = 0;            // samples actually out of the speaker (audioPos - device padding)
    float volume = 1.0f;                 // 0..1 from the page, already perceptual
    bool muted = false;
    // Distance rolloff: the page names the screen's world position, a near/far range and the
    // gains to lerp between over it (rolloff=x,y,z,near,far,max,min); the hook reads the
    // listener from the client every frame. max is the gain at near or closer, min the gain at
    // far or beyond, so the default 1,0 is the plain fade to silence and e.g. 1,0.3 keeps a
    // screen audible across the map.
    bool rolloff = false;
    bool pan = false;                    // ";pan=1": stereo pan by bearing; off by default, a stream is not a point source
    float sourcePos[3] = {};
    float nearDistance = 0, farDistance = 0;
    float nearGain = 1.0f, farGain = 0.0f;
    float distanceGain = 1.0f;           // target from the geometry, per frame
    float panLeft = 1.0f, panRight = 1.0f;
    float smoothGain = 1.0f, smoothLeft = 1.0f, smoothRight = 1.0f; // eased in the render thread
    WAVEFORMATEX* mixFormat = nullptr;
    IAudioClient* audioClient = nullptr;

    // Lifetime: the client draws a screen every frame while it exists, so the last draw time is
    // the lifetime signal. Audio pauses at kAudioHoldMs, the session is torn down at kIdleStopMs
    // and restarted when drawing resumes, and the rings are freed at kIdleFreeMs.
    bool sessionActive = false;
    ULONGLONG sessionStarted = 0;
    ULONGLONG lastDraw = 0;
    LONG underruns = 0;                  // audio ran dry (playback held) since the session started
    LONG videoWaits = 0;                 // reader held because the frame ring was full
    LONG videoDrops = 0;                 // oldest frames discarded while audio could not drive playback yet
    LONG audioDrops = 0;                 // oldest samples discarded while video could not drive playback yet
    LONG audioWaits = 0;                 // reader held because the sample ring was full
    wchar_t status[256] = {};
    wchar_t source[512] = {};
    HANDLE thread = nullptr;
    HANDLE audioRenderThread = nullptr;
    HANDLE processes[2] = {};
    volatile LONG stop = 0;
    ScreenStream* next = nullptr;
};

extern ScreenStream* g_streams;
extern CRITICAL_SECTION g_streamsLock;
extern HANDLE g_job;
extern HANDLE g_toolLog;
extern bool g_screenVideoInitialised;
extern HANDLE g_watchdog;
extern bool g_logStats;

// aisp.screen.log next to the game executable (opened by InitScreenVideo); tool stderr goes there too.
void LogLine(const char* text);
void DebugLog(const wchar_t* format, const wchar_t* arg);
bool BuildGameFilePath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount);
// The line the screen shows while a source has nothing to draw yet (or failed).
void SetStatus(ScreenStream* stream, const wchar_t* text);

// Child processes: resolved from an environment variable or the game directory, attached to
// the job so they die with the game, stderr to the log.
bool ToolPath(const wchar_t* variable, const wchar_t* fallback, wchar_t* out, size_t outCount);
HANDLE LaunchTool(wchar_t* commandLine, HANDLE stdIn, HANDLE stdOut);
bool CreateInheritablePipe(HANDLE* readEnd, HANDLE* writeEnd, bool inheritRead);
bool RunToolForLine(wchar_t* commandLine, wchar_t* out, size_t outCount);
// Up to maxLines lines of a tool's stdout (each up to lineCount characters); returns the count.
int RunToolForLines(wchar_t* commandLine, wchar_t* lines, size_t lineCount, int maxLines);
// Seconds since the Unix epoch, UTC, from the system clock.
double UnixNow();
// Where the page's shared timeline puts a video right now, wrapped into `duration` when known
// (0 for none), and 0 when the page gives no timeline.
double TimelinePosition(ScreenStream* stream, double duration);
bool ReadFully(HANDLE pipe, BYTE* buffer, DWORD size, volatile LONG* stop);

// Ring writers with back pressure: they wait while the ring is full and return false once the
// session is stopping. A source writes frame f and then its samplesPerFrame samples, so the
// presenter can pair them.
bool PushVideoFrame(ScreenStream* stream, const BYTE* frame);
void CopyCropWindow(BYTE* out, int w, int h, const BYTE* picture, int pw, int ph, int cx, int cy);
bool PushAudioSamples(ScreenStream* stream, const float* samples, UINT32 count);

// Audio ring -> WASAPI (aisp.hook.cpp). A source with audio starts this thread once its samples
// are on the way and sets audioActive so the presenter follows the device clock.
DWORD WINAPI AudioRenderThread(LPVOID parameter);
} // namespace aisp
