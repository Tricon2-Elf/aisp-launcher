// Source: the hook's own test pattern, a calibration picture and tone generated in-process, no
// ffmpeg involved, so the overlay path (box, key, the engine scaling the texture onto the mesh,
// the audio clock, rolloff) can be checked on its own. pattern:live counts from zero whenever
// the session starts, like a stream; pattern:vod is a fixed-length video that follows the
// page's shared timeline (start, offset, paused, hold), so pause, seek and late join can be
// checked without a real video. Pixel exact at whatever size the page asks for: a 1 px white ring and a grey ring inside it (edge pixels visible?), corner blocks,
// 75% colour bars, a grey ramp and a 16-step staircase, 1 px and 2 px checkerboards and 1 px
// line pairs (does one texel land on one pixel?), R/G/B ramps, a magenta crosshair and circle
// (aspect), a bar bouncing between the edges in step with the tone (left beep at the left edge,
// right beep at the right edge: audio/video sync and channel identity), a binary frame counter,
// and a label with the size and rate.
#include "source.h"

#include <strsafe.h>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace aisp
{
namespace
{
constexpr float kPatternPeriodSeconds = 2.0f;
constexpr float kPatternBeepSeconds = 0.15f;
constexpr float kPatternToneAmplitude = 0.25f; // about -12 dBFS
constexpr double kPatternVodSeconds = 60.0;    // pattern:vod loops after this (a whole number of periods)

inline void PatternPixel(BYTE* frame, int w, int h, int x, int y, DWORD rgb)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    *reinterpret_cast<DWORD*>(frame + (static_cast<size_t>(y) * w + x) * 4) = 0xFF000000u | rgb;
}

// Half-open rectangle, clipped.
void PatternRect(BYTE* frame, int w, int h, int x0, int y0, int x1, int y1, DWORD rgb)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            *reinterpret_cast<DWORD*>(frame + (static_cast<size_t>(y) * w + x) * 4) = 0xFF000000u | rgb;
}

void PatternCircle(BYTE* frame, int w, int h, int cx, int cy, int r, DWORD rgb)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y)
    {
        PatternPixel(frame, w, h, cx + x, cy + y, rgb); PatternPixel(frame, w, h, cx - x, cy + y, rgb);
        PatternPixel(frame, w, h, cx + x, cy - y, rgb); PatternPixel(frame, w, h, cx - x, cy - y, rgb);
        PatternPixel(frame, w, h, cx + y, cy + x, rgb); PatternPixel(frame, w, h, cx - y, cy + x, rgb);
        PatternPixel(frame, w, h, cx + y, cy - x, rgb); PatternPixel(frame, w, h, cx - y, cy - x, rgb);
        ++y;
        if (err < 0)
            err += 2 * y + 1;
        else
        {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

struct PatternLayout
{
    int x0, y0, x1, y1;                  // content area inside the rings
    int barsY1, rampY1, stepsY1, pixelsY1, rgbY1; // band boundaries (each band ends here)
};

PatternLayout PatternLayoutFor(int w, int h)
{
    PatternLayout l = {};
    l.x0 = 3; l.y0 = 3; l.x1 = w - 3; l.y1 = h - 3;
    if (l.x1 <= l.x0 || l.y1 <= l.y0)
    {
        l.x0 = l.y0 = 0; l.x1 = w; l.y1 = h;
    }
    const int ch = l.y1 - l.y0;
    if (ch < 80)
    {
        // A banner: bars, pixel structure and motion only.
        l.barsY1 = l.y0 + ch * 35 / 100;
        l.rampY1 = l.stepsY1 = l.barsY1;
        l.pixelsY1 = l.barsY1 + ch * 30 / 100;
        l.rgbY1 = l.pixelsY1;
        return l;
    }
    // Fractions of the height; the motion band takes what is left (at least a fifth).
    l.barsY1 = l.y0 + ch * 22 / 100;
    l.rampY1 = l.barsY1 + ch * 8 / 100;
    l.stepsY1 = l.rampY1 + ch * 7 / 100;
    l.pixelsY1 = l.stepsY1 + ch * 25 / 100;
    l.rgbY1 = l.pixelsY1 + ch * 12 / 100;
    return l;
}

void DrawPatternBase(BYTE* f, int w, int h)
{
    PatternRect(f, w, h, 0, 0, w, h, 0x000000);
    // Rings: 1 px white outside, 1 px black, 1 px mid grey.
    PatternRect(f, w, h, 0, 0, w, 1, 0xFFFFFF); PatternRect(f, w, h, 0, h - 1, w, h, 0xFFFFFF);
    PatternRect(f, w, h, 0, 0, 1, h, 0xFFFFFF); PatternRect(f, w, h, w - 1, 0, w, h, 0xFFFFFF);
    PatternRect(f, w, h, 2, 2, w - 2, 3, 0x808080); PatternRect(f, w, h, 2, h - 3, w - 2, h - 2, 0x808080);
    PatternRect(f, w, h, 2, 2, 3, h - 2, 0x808080); PatternRect(f, w, h, w - 3, 2, w - 2, h - 2, 0x808080);
    const PatternLayout l = PatternLayoutFor(w, h);
    const int cw = l.x1 - l.x0;
    // Colour bars at 75%.
    static const DWORD bars[8] = {0xBFBFBF, 0xBFBF00, 0x00BFBF, 0x00BF00, 0xBF00BF, 0xBF0000, 0x0000BF, 0x000000};
    for (int i = 0; i < 8; ++i)
        PatternRect(f, w, h, l.x0 + cw * i / 8, l.y0, l.x0 + cw * (i + 1) / 8, l.barsY1, bars[i]);
    // Grey ramp and staircase.
    for (int x = l.x0; x < l.x1; ++x)
    {
        const DWORD v = cw > 1 ? static_cast<DWORD>((x - l.x0) * 255 / (cw - 1)) : 255;
        PatternRect(f, w, h, x, l.barsY1, x + 1, l.rampY1, v * 0x010101);
    }
    for (int i = 0; i < 16; ++i)
        PatternRect(f, w, h, l.x0 + cw * i / 16, l.rampY1, l.x0 + cw * (i + 1) / 16, l.stepsY1, static_cast<DWORD>(i * 17) * 0x010101);
    // Pixel structure: 1 px checker, 1 px vertical then horizontal lines, 2 px checker.
    const int third = cw / 3;
    for (int y = l.stepsY1; y < l.pixelsY1; ++y)
        for (int x = l.x0; x < l.x1; ++x)
        {
            const int rx = x - l.x0;
            DWORD v;
            if (rx < third)
                v = ((x + y) & 1) ? 0xFFFFFF : 0x000000;
            else if (rx < 2 * third)
                v = (rx < third + third / 2 ? (x & 1) : (y & 1)) ? 0xFFFFFF : 0x000000;
            else
                v = (((x >> 1) + (y >> 1)) & 1) ? 0xFFFFFF : 0x000000;
            PatternPixel(f, w, h, x, y, v);
        }
    // R, G, B ramps.
    const int rgbH = l.rgbY1 - l.pixelsY1;
    for (int x = l.x0; x < l.x1; ++x)
    {
        const DWORD v = cw > 1 ? static_cast<DWORD>((x - l.x0) * 255 / (cw - 1)) : 255;
        PatternRect(f, w, h, x, l.pixelsY1, x + 1, l.pixelsY1 + rgbH / 3, v << 16);
        PatternRect(f, w, h, x, l.pixelsY1 + rgbH / 3, x + 1, l.pixelsY1 + rgbH * 2 / 3, v << 8);
        PatternRect(f, w, h, x, l.pixelsY1 + rgbH * 2 / 3, x + 1, l.rgbY1, v);
    }
    // Motion band background: dark with a faint 16 px grid.
    PatternRect(f, w, h, l.x0, l.rgbY1, l.x1, l.y1, 0x181818);
    for (int x = l.x0; x < l.x1; x += 16)
        PatternRect(f, w, h, x, l.rgbY1, x + 1, l.y1, 0x303030);
    for (int y = l.rgbY1; y < l.y1; y += 16)
        PatternRect(f, w, h, l.x0, y, l.x1, y + 1, 0x303030);
    // Corner blocks (5 px) just inside the rings, crosshair and circle over everything.
    for (int cx = 0; cx < 2; ++cx)
        for (int cy = 0; cy < 2; ++cy)
        {
            const int bx = cx ? l.x1 - 5 : l.x0, by = cy ? l.y1 - 5 : l.y0;
            PatternRect(f, w, h, bx, by, bx + 5, by + 5, 0xFFFFFF);
            PatternRect(f, w, h, bx + 2, by + 2, bx + 3, by + 3, 0x000000);
        }
    const int mx = (l.x0 + l.x1) / 2, my = (l.y0 + l.y1) / 2;
    PatternRect(f, w, h, mx, l.y0, mx + 1, l.y1, 0xFF00FF);
    PatternRect(f, w, h, l.x0, my, l.x1, my + 1, 0xFF00FF);
    const int r = ((cw < l.y1 - l.y0) ? cw : (l.y1 - l.y0)) / 2 - 2;
    if (r > 4)
        PatternCircle(f, w, h, mx, my, r, 0xFF00FF);
}

struct PatternState
{
    HFONT labelFont = nullptr;
};

void DrawPatternFrame(HDC dc, BYTE* f, int w, int h, LONGLONG frame, int fps, const int* crop, const wchar_t* kind, PatternState& st)
{
    const PatternLayout l = PatternLayoutFor(w, h);
    const int cw = l.x1 - l.x0;
    const double t = static_cast<double>(frame) / fps;
    const double phase = std::fmod(t, static_cast<double>(kPatternPeriodSeconds));
    const int my0 = l.rgbY1, my1 = l.y1;
    // The bouncing bar: at the left edge when the left beep starts, at the right edge for the right one.
    const double pos = phase < 1.0 ? phase : 2.0 - phase;
    const int barX = l.x0 + static_cast<int>(pos * (cw - 3) + 0.5);
    PatternRect(f, w, h, barX, my0, barX + 3, my1, 0xFFFFFF);
    // Beep flashes (the first frames of each beep), and a binary frame counter.
    const int sq = (my1 - my0) / 3 > 4 ? (my1 - my0) / 3 : 4;
    if (phase < 0.05)
        PatternRect(f, w, h, l.x0 + 4, my0 + 2, l.x0 + 4 + sq, my0 + 2 + sq, 0xFFFFFF);
    if (phase >= 1.0 && phase < 1.05)
        PatternRect(f, w, h, l.x1 - 4 - sq, my0 + 2, l.x1 - 4, my0 + 2 + sq, 0xFFFFFF);
    const int bit = cw / 40 > 3 ? cw / 40 : 3;
    for (int i = 0; i < 16; ++i)
    {
        const int bx = l.x0 + (l.x1 - l.x0) / 2 - 8 * bit + i * bit;
        PatternRect(f, w, h, bx, my1 - bit - 2, bx + bit - 1, my1 - 2, ((frame >> (15 - i)) & 1) ? 0xFFFFFF : 0x404040);
    }
    // Everything from here uses GDI on the same bits.
    SetBkMode(dc, TRANSPARENT);
    // Label: kind, size, rate, frame and time, next to the counter.
    wchar_t label[128] = {};
    if (crop[0] > 0)
        StringCchPrintfW(label, 128, L"aisp-emu pattern:%s %dx%d+%d+%d @%d f=%lld t=%.1fs", kind, w, h, crop[2], crop[3], fps, frame, t);
    else
        StringCchPrintfW(label, 128, L"aisp-emu pattern:%s %dx%d @%d f=%lld t=%.1fs", kind, w, h, fps, frame, t);
    SelectObject(dc, st.labelFont);
    SetTextColor(dc, RGB(255, 255, 255));
    SIZE size = {};
    GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &size);
    if (size.cx + 8 < cw)
    {
        // Top right of the pixel band, or in the motion band beside the flash square when the
        // pixel band is too short for it.
        const bool fits = l.pixelsY1 - l.stepsY1 >= size.cy + 4;
        const int lx = fits ? l.x1 - size.cx - 6 : l.x0 + 8 + sq;
        const int ly = fits ? l.stepsY1 + 2 : my0 + 2;
        PatternRect(f, w, h, lx - 2, ly - 1, lx + size.cx + 2, ly + size.cy + 1, 0x000000);
        TextOutW(dc, lx, ly, label, static_cast<int>(std::wcslen(label)));
    }
}

// Left beep at 1 kHz on the period start, right beep at 1.5 kHz half way, 5 ms ramps so
// nothing clicks; the remaining channels stay silent.
void GeneratePatternTone(float* out, LONGLONG firstSample, UINT32 count, UINT32 sampleRate, UINT32 channels)
{
    const double twoPi = 6.283185307179586;
    for (UINT32 i = 0; i < count; ++i)
    {
        const double ts = static_cast<double>(firstSample + i) / sampleRate;
        const double phase = std::fmod(ts, static_cast<double>(kPatternPeriodSeconds));
        float left = 0.0f, right = 0.0f;
        auto envelope = [](double into) -> float {
            const double edge = 0.005;
            double e = into < edge ? into / edge : ((kPatternBeepSeconds - into) < edge ? (kPatternBeepSeconds - into) / edge : 1.0);
            return static_cast<float>(e < 0 ? 0 : e);
        };
        if (phase < kPatternBeepSeconds)
            left = kPatternToneAmplitude * envelope(phase) * static_cast<float>(std::sin(twoPi * 1000.0 * ts));
        else if (phase >= 1.0 && phase < 1.0 + kPatternBeepSeconds)
            right = kPatternToneAmplitude * envelope(phase - 1.0) * static_cast<float>(std::sin(twoPi * 1500.0 * ts));
        for (UINT32 c = 0; c < channels; ++c)
            out[i * channels + c] = c == 0 ? left : (c == 1 ? right : 0.0f);
    }
}

} // namespace

// The pattern is drawn at the box size, or with a crop at the crop's render size, of which the
// box-sized window at the crop origin is pushed (black outside the picture).
void CopyCropWindow(BYTE* out, int w, int h, const BYTE* picture, int pw, int ph, int cx, int cy)
{
    for (int y = 0; y < h; ++y)
    {
        BYTE* row = out + static_cast<size_t>(y) * w * 4;
        const int sy = cy + y;
        if (sy >= ph || cx >= pw)
        {
            std::memset(row, 0, static_cast<size_t>(w) * 4);
            continue;
        }
        const int copy = cx + w <= pw ? w : pw - cx;
        std::memcpy(row, picture + (static_cast<size_t>(sy) * pw + cx) * 4, static_cast<size_t>(copy) * 4);
        if (copy < w)
            std::memset(row + static_cast<size_t>(copy) * 4, 0, static_cast<size_t>(w - copy) * 4);
    }
}

DWORD RunPatternSource(ScreenStream* stream)
{
    const int fps = stream->fps;
    // What follows "pattern:" picks the kind; anything but "vod" is live.
    const bool vod = _wcsnicmp(stream->source + 8, L"vod", 3) == 0;
    const wchar_t* kind = vod ? L"vod" : L"live";
    const LONGLONG loopFrames = static_cast<LONGLONG>(kPatternVodSeconds) * fps;
    LONGLONG frame = 0;
    if (vod)
    {
        // Start where the shared timeline says the video is, like a seeking source would.
        stream->seekSeconds = TimelinePosition(stream, kPatternVodSeconds);
        frame = static_cast<LONGLONG>(stream->seekSeconds * fps);
    }
    const bool cropped = stream->crop[0] > 0;
    const int w = cropped ? stream->crop[0] : stream->videoWidth, h = cropped ? stream->crop[1] : stream->videoHeight;
    const DWORD pictureBytes = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    SetStatus(stream, vod ? L"pattern: vod" : L"pattern: live");
    if (stream->audioWanted)
    {
        EnterCriticalSection(&stream->lock);
        stream->audioActive = true; // no pipe to wait for: the tone is written along with the frames
        LeaveCriticalSection(&stream->lock);
        stream->audioRenderThread = CreateThread(nullptr, 0, AudioRenderThread, stream, 0, nullptr);
    }

    BYTE* base = new BYTE[pictureBytes];
    DrawPatternBase(base, w, h);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!dib || !bits)
    {
        if (dc)
            DeleteDC(dc);
        delete[] base;
        SetStatus(stream, L"pattern: no drawing surface");
        return 0;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);

    PatternState st;
    st.labelFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Courier New");

    const UINT32 spf = stream->samplesPerFrame, channels = stream->channels;
    float* tone = (stream->audioWanted && spf && channels) ? new float[static_cast<size_t>(spf) * channels] : nullptr;
    BYTE* frameBits = static_cast<BYTE*>(bits);
    BYTE* window = cropped ? new BYTE[stream->frameBytes] : nullptr;
    for (; !stream->stop; ++frame)
    {
        // The vod wraps at its length, like a video that loops; live counts on.
        const LONGLONG shown = vod ? frame % loopFrames : frame;
        std::memcpy(frameBits, base, pictureBytes);
        DrawPatternFrame(dc, frameBits, w, h, shown, fps, stream->crop, kind, st);
        GdiFlush();

        if (window)
            CopyCropWindow(window, stream->videoWidth, stream->videoHeight, frameBits, w, h, stream->crop[2], stream->crop[3]);
        if (!PushVideoFrame(stream, window ? window : frameBits))
            break;
        if (tone)
        {
            GeneratePatternTone(tone, shown * spf, spf, stream->sampleRate, channels);
            if (!PushAudioSamples(stream, tone, spf))
                break;
        }
    }

    delete[] tone;
    delete[] window;
    SelectObject(dc, oldBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    DeleteObject(st.labelFont);
    delete[] base;
    return 0;
}
} // namespace aisp
