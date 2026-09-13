// The client's receive switch has one case for notify_nicolive_reload (0xE342): it reads the
// 97-byte live id, and if the map's Nico Live billboard object exists (the Stage only) builds
// that page's URL from the id and navigates the billboard to it; then, on every map, it calls
// one virtual of the session object with the id and returns to the common tail. Any other
// screen on the map gets nothing, and only learns of a new /screen or /channel assignment from
// its page's own poll. The hook detours the case right before that final call, at the point
// both branches reach (0x7EFF5A), reloads the primary browser of every screen whose page is a
// channel-screen on a map that is not a room (room TVs are reloaded by their own set-channel
// notify), narrowed to the screens on one channel number when the packet's live id is
// lv0..lv99, first stopping what they play when it is lv200 (see ScopeOf), and continues
// with the displaced instructions.
#define CINTERFACE
#include <windows.h>
#include <exdisp.h>
#include <strsafe.h>
#include <cstring>
#include <cwchar>
#include <initializer_list>

#include "reload_notify.h"
#include "browser.h"

namespace aisp
{
bool PatchBytes(DWORD address, const BYTE* expected, const BYTE* replacement, size_t size); // tv_panel.cpp
void StopSession(ScreenStream* stream); // aisp.hook.cpp: stops the screen's source, waiting for its threads

namespace
{
// IID_IWebBrowser2, spelt out so no uuid library is needed.
const GUID kWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};

// The value of one numeric query parameter (`key=` with the = included) in a URL, or -1.
int QueryNumber(const wchar_t* url, const wchar_t* key)
{
    const wchar_t* at = std::wcsstr(url, key);
    while (at && at != url && at[-1] != L'?' && at[-1] != L'&')
        at = std::wcsstr(at + 1, key);
    if (!at)
        return -1;
    const wchar_t* digits = at + std::wcslen(key);
    if (*digits < L'0' || *digits > L'9')
        return -1;
    int value = 0;
    while (*digits >= L'0' && *digits <= L'9' && value < 100000000)
        value = value * 10 + (*digits++ - L'0');
    return *digits == L'\0' || *digits == L'&' || *digits == L'#' ? value : -1;
}

// The screen page of a town map's own screen: a channel-screen (the client's route for a
// screen on a channel, the same one a room TV on a channel uses) whose map= is not a room's
// (MyRoom maps are 20000000..20000030, the only map ids starting with 2). With a channel
// filter, only one whose own tvid= (the channel number the client gave the screen) is it.
bool IsTownChannelScreen(const wchar_t* pageUrl, int channelFilter)
{
    if (!pageUrl || !std::wcsstr(pageUrl, L"/channel-screen?"))
        return false;
    const wchar_t* map = std::wcsstr(pageUrl, L"?map=");
    if (!map)
        map = std::wcsstr(pageUrl, L"&map=");
    if (!map)
        return false;
    map += 5;
    if (*map < L'0' || *map > L'9' || *map == L'2')
        return false;
    return channelFilter < 0 || QueryNumber(pageUrl, L"tvid=") == channelFilter;
}

// The packet's live id doubles as the reload's scope: lv0..lv99 means only the town screens
// on that channel number (a /channel change: the screens following their own tvid= are the
// ones that show it), lv100..lv199 every town screen (a /screen change), lv200 and up every
// town screen with whatever it plays torn down first (/screen reload, to unstick a decoder or
// a player as well as the page); anything else counts as lv100. The Stage's billboard takes
// the id as the page to load, which the emulator ignores.
struct ReloadScope
{
    int channelFilter = -1; // -1: every town screen
    bool teardown = false;  // stop the screen's source before its page reloads
};
ReloadScope ScopeOf(const char* liveId)
{
    ReloadScope scope;
    if (!liveId || liveId[0] != 'l' || liveId[1] != 'v' || liveId[2] < '0' || liveId[2] > '9')
        return scope;
    int value = 0;
    const char* p = liveId + 2;
    while (*p >= '0' && *p <= '9' && value < 1000)
        value = value * 10 + (*p++ - '0');
    if (*p != '\0')
        return scope;
    if (value <= 99)
        scope.channelFilter = value;
    else if (value >= 200)
        scope.teardown = true;
    return scope;
}

// Reloads the page of one screen: the primary Electron's page through a script, or the IE
// control's through IWebBrowser2::Refresh when IE is the page.
bool ReloadScreenPage(ScreenStream* stream)
{
    if (UsePrimaryBrowser())
    {
        if (!stream->primaryActive || !stream->primaryControl)
            return false;
        SendPrimaryEval(stream, "location.reload()");
        return true;
    }
    if (!stream->browser)
        return false;
    IWebBrowser2* browser = nullptr;
    if (FAILED(stream->browser->lpVtbl->QueryInterface(stream->browser, kWebBrowser2, reinterpret_cast<void**>(&browser))) || !browser)
        return false;
    const HRESULT hr = browser->lpVtbl->Refresh(browser);
    browser->lpVtbl->Release(browser);
    return SUCCEEDED(hr);
}

// Entered from the detour once the client has read the packet's live id (`liveId`), before its
// own handling continues. Runs on the client's network-dispatch thread; the list lock covers
// the walk, and a reload is one line on a channel or one COM call.
void __cdecl OnNicoliveReloadNotify(const char* liveId)
{
    if (!g_streamsLockReady)
        return;
    const ReloadScope scope = ScopeOf(liveId);
    const int channelFilter = scope.channelFilter;
    int matched = 0, reloaded = 0, stopped = 0;
    EnterCriticalSection(&g_streamsLock);
    for (ScreenStream* stream = g_streams; stream; stream = stream->next)
    {
        if (!IsTownChannelScreen(stream->pageUrl, channelFilter))
            continue;
        matched++;
        // The source goes first (as the watchdog does it, under the list lock); the reloaded
        // page's title then starts it afresh through the draw path.
        if (scope.teardown && stream->sessionActive)
        {
            StopSession(stream);
            stopped++;
        }
        if (ReloadScreenPage(stream))
            reloaded++;
    }
    LeaveCriticalSection(&g_streamsLock);
    char note[256] = {};
    char id[100] = {};
    if (liveId)
        StringCchCopyA(id, sizeof(id), liveId);
    if (channelFilter >= 0)
        StringCchPrintfA(note, sizeof(note), "screen reload notify (%s): %d town screen(s) on channel %d, %d reloaded\r\n", id, matched, channelFilter, reloaded);
    else if (scope.teardown)
        StringCchPrintfA(note, sizeof(note), "screen reload notify (%s): %d town screen(s), %d source(s) stopped, %d reloaded\r\n", id, matched, stopped, reloaded);
    else
        StringCchPrintfA(note, sizeof(note), "screen reload notify (%s): %d town screen(s), %d reloaded\r\n", id, matched, reloaded);
    LogLine(note);
}
} // namespace

void PatchNicoliveReloadNotify()
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return;
    // 0x7EFF5A: mov ecx,[esp+0x20] (the live id buffer); mov eax,[ebx]; mov edx,[eax+0x608]
    // The first two (6 bytes) make room for the jump; the third is left in place at 0x7EFF60.
    const DWORD site = 0x7EFF5A;
    const DWORD resume = 0x7EFF60;
    static const BYTE expected[] = {0x8B, 0x4C, 0x24, 0x20, 0x8B, 0x03};
    auto* target = reinterpret_cast<const BYTE*>(site);
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT || std::memcmp(target, expected, sizeof(expected)) != 0)
    {
        OutputDebugStringW(L"aisp.hook: nicolive reload notify: unexpected client code, not patched\n");
        return;
    }

    BYTE* stub = static_cast<BYTE*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return;
    BYTE code[64];
    size_t n = 0;
    auto emit = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) code[n++] = b; };
    auto emitDword = [&](DWORD value) { std::memcpy(code + n, &value, 4); n += 4; };
    // esp is as at the site (a jmp got here), so the displaced load reads the same slot.
    emit({0x8B, 0x4C, 0x24, 0x20});                       // mov  ecx,[esp+0x20]  ; the live id
    emit({0x60});                                         // pushad
    emit({0x9C});                                         // pushfd
    emit({0x51});                                         // push ecx
    emit({0xE8}); emitDword(reinterpret_cast<DWORD>(&OnNicoliveReloadNotify) - (reinterpret_cast<DWORD>(stub) + n + 4));
    emit({0x83, 0xC4, 0x04});                             // add  esp,4           ; cdecl
    emit({0x9D});                                         // popfd
    emit({0x61});                                         // popad                ; ecx as loaded
    emit({0x8B, 0x03});                                   // mov  eax,[ebx]       ; the other displaced instruction
    emit({0xE9}); emitDword(resume - (reinterpret_cast<DWORD>(stub) + n + 4));
    std::memcpy(stub, code, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);

    BYTE jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const DWORD rel = reinterpret_cast<DWORD>(stub) - (site + 5);
    std::memcpy(jump + 1, &rel, 4);
    if (!PatchBytes(site, expected, jump, sizeof(jump)))
    {
        OutputDebugStringW(L"aisp.hook: nicolive reload notify: patch failed\n");
        return;
    }
    OutputDebugStringW(L"aisp.hook: nicolive reload notify: town screens reload on it\n");
}
} // namespace aisp
