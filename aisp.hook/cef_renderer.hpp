#pragma once

#include <windows.h>

// Starts (or navigates) the private CEF off-screen browser for one AISP TV
// surface. The source URL and dimensions are taken directly from the ATL host
// creation that the game would otherwise give to Trident.
#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllexport) void WINAPI StartCefRenderer(LPCWSTR source, int width, int height);

// Copies the newest CEF BGRA frame to the HDC supplied by the game's OleDraw
// call. Returns false until CEF has produced its first frame.
__declspec(dllexport) bool WINAPI DrawCefFrame(HDC destination, const RECT* destinationRect);

#ifdef __cplusplus
}
#endif
