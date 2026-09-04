// The room TV panel's comment button. The client ships it half wired (see the comment on
// PatchTvCommentButton); these patches complete it in memory, in this client build only.
#include <windows.h>
#include <cstring>
#include <cwchar>
#include <initializer_list>

#include "tv_panel.h"

// The client's own methods are thiscall; spelt for both compilers that build this DLL.
#if defined(_MSC_VER)
#define AISP_THISCALL __thiscall
#else
#define AISP_THISCALL __attribute__((thiscall))
#endif

namespace aisp
{
// The room TV's control panel has a comment on/off button (control id 190), but in this client
// build its click case in the panel's message handler is an empty stub (0x649E57: mov al,1; jmp
// to the epilogue), so pressing it did nothing. Everything else is in place: the button is a
// two-state toggle like mute and play, its state follows the TV's comment byte through the
// panel's refresh, and the TV object's SetCommentVisible (0x48B520, thiscall, one bool) calls the
// page's ext_setCommentVisible when the player is ready and queues it otherwise. Three patches,
// each only applied when the expected bytes are there:
//  - the click: the handler's jump-table slot for id 190 is repointed to a stub that calls
//    SetCommentVisible with the inverse of the current byte, then the inner panel's state
//    refresh (0x647DD0, which sets every toggle from the TV bytes) so the button follows at once.
//    In the handler ebp is the panel, [ebp+0x12C] its inner control panel, edi the panel's TV
//    holder, the TV is [edi+8] and esi is TV+0x10 (0 when there is no TV).
//  - the second image: a toggle holds a layout unit id per state (frame id+0 at rest, +1 hovered,
//    +2 pressed) and the first doubles as the control id, so it must stay 190. The panel's layout
//    (PAS 16200, interface/settings/PAS/myroom05.xml) has two groups for this button, 190-192
//    "comments shown" and 200-202 "comments hidden", the same shape as mute's 140 and 150, but
//    the create call (0x647C07) passes 190 for both; it is patched to pass 200 for the second.
//  - the state: the refresh feeds the toggle the TV's comment byte (1 = shown), which would draw
//    the second image, the hidden glyph, while comments show. Its feed for this one button
//    (0x647DB2) is routed through a stub that inverts the byte, so the button shows the current
//    state the way mute does. AISP_TV_COMMENT_HIDDEN_IMAGE overrides the 200.
//  - the frames: the sheet (texture 343, interface/main/edit_window03.dds) draws this button's
//    mousedown frames as a preview of the other state rather than the shifted glyph every other
//    button uses, and the hidden state's hover frame sits two pixels too high in its cell. The
//    toggle's apply (0x4B6600) hands the frame (image id + visual state: 0 rest, 1 hover, 2
//    pressed) to the shared unit apply (0x77C300); that call site is routed through a wrapper
//    that, for this button only, draws the hover frame for mousedown and nudges the control one
//    pixel right and down, and two pixels down for the hidden state's hover and mousedown.
// There is no network message for this field, so the toggle is local to this client, as the
// panel's mute button is.

using UnitApply_t = int(__cdecl*)(void* control, void* unitHandle, int frame, int a, int b);
UnitApply_t g_originalToggleApplyUnit = reinterpret_cast<UnitApply_t>(0x77C300);
DWORD g_commentHiddenImage = 200;
// The last nudge applied to the comment button, undone before the next apply so the offsets never
// accumulate whatever the unit apply does to the position.
void* g_nudgedControl = nullptr;
int g_nudgeX = 0, g_nudgeY = 0;

void NudgeControl(void* control, int dx, int dy)
{
    auto** vtable = *reinterpret_cast<void***>(control);
    using Get_t = int(AISP_THISCALL* )(void*);
    using Set_t = void(AISP_THISCALL* )(void*, int);
    auto getX = reinterpret_cast<Get_t>(vtable[0x58 / 4]);
    auto getY = reinterpret_cast<Get_t>(vtable[0x60 / 4]);
    auto setX = reinterpret_cast<Set_t>(vtable[0x54 / 4]);
    auto setY = reinterpret_cast<Set_t>(vtable[0x5C / 4]);
    setX(control, getX(control) + dx);
    setY(control, getY(control) + dy);
}

int __cdecl CommentButtonApplyUnit(void* control, void* unitHandle, int frame, int a, int b)
{
    auto* c = static_cast<BYTE*>(control);
    const bool isCommentButton = *reinterpret_cast<DWORD*>(c + 0xA0) == 190 && *reinterpret_cast<DWORD*>(c + 0xA4) == g_commentHiddenImage;
    int dx = 0, dy = 0;
    if (isCommentButton)
    {
        const int image = *reinterpret_cast<int*>(c + 0x98);
        const int visual = frame - image;
        if (visual == 2)
        {
            frame = image + 1;
            dx = 1;
            dy = 1;
        }
        if (image == static_cast<int>(g_commentHiddenImage) && (visual == 1 || visual == 2))
            dy += 2;
    }
    if (g_nudgedControl == control && (g_nudgeX || g_nudgeY))
        NudgeControl(control, -g_nudgeX, -g_nudgeY);
    g_nudgedControl = nullptr;
    const int result = g_originalToggleApplyUnit(control, unitHandle, frame, a, b);
    if (dx || dy)
    {
        NudgeControl(control, dx, dy);
        g_nudgedControl = control;
        g_nudgeX = dx;
        g_nudgeY = dy;
    }
    return result;
}

bool PatchBytes(DWORD address, const BYTE* expected, const BYTE* replacement, size_t size)
{
    auto* target = reinterpret_cast<BYTE*>(address);
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    if (std::memcmp(target, expected, size) != 0)
        return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(target, replacement, size);
    DWORD ignored = 0;
    VirtualProtect(target, size, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

void PatchTvCommentButton()
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return;
    auto* caseBody = reinterpret_cast<BYTE*>(0x649E57);
    auto* tableSlot = reinterpret_cast<DWORD*>(0x649E90);
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(caseBody, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return;
    static const BYTE expectedBody[] = {0xB0, 0x01, 0xEB, 0x02}; // mov al,1; jmp +2
    if (std::memcmp(caseBody, expectedBody, sizeof(expectedBody)) != 0 || *tableSlot != 0x649E57)
    {
        OutputDebugStringW(L"aisp.hook: TV comment button: unexpected client code, not patched\n");
        return;
    }

    DWORD hiddenImage = 200;
    wchar_t setting[16] = {};
    if (GetEnvironmentVariableW(L"AISP_TV_COMMENT_HIDDEN_IMAGE", setting, 16) > 0 && setting[0])
        hiddenImage = static_cast<DWORD>(_wtoi(setting));
    g_commentHiddenImage = hiddenImage;

    BYTE* stubs = static_cast<BYTE*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stubs)
        return;
    const DWORD setCommentVisible = 0x48B520;
    const DWORD refreshPanel = 0x647DD0;
    const DWORD setToggleState = 0x647000;
    const DWORD epilogue = 0x649E5D;
    BYTE code[128];
    size_t n = 0;
    auto emit = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) code[n++] = b; };
    auto emitDword = [&](DWORD value) { std::memcpy(code + n, &value, 4); n += 4; };
    auto emitCall = [&](DWORD target) { emit({0xE8}); emitDword(target - (reinterpret_cast<DWORD>(stubs) + n + 4)); };

    // Click stub, entered from the handler's jump table.
    BYTE* clickStub = stubs + n;
    emit({0x85, 0xF6});                                   // test esi,esi        ; no TV object
    const size_t jzNoTv = n; emit({0x74, 0x00});          // jz   done
    emit({0x8B, 0x4F, 0x08});                             // mov  ecx,[edi+8]    ; TV object (this)
    emit({0x80, 0x7E, 0x02, 0x00});                       // cmp  byte [esi+2],0 ; comment flag
    emit({0x0F, 0x94, 0xC0});                             // sete al             ; the inverse
    emit({0x0F, 0xB6, 0xC0});                             // movzx eax,al
    emit({0x50});                                         // push eax
    emitCall(setCommentVisible);                          // callee pops the argument
    emit({0x8B, 0x8D}); emitDword(0x12C);                 // mov  ecx,[ebp+0x12C] ; inner panel
    emit({0x85, 0xC9});                                   // test ecx,ecx
    const size_t jzNoPanel = n; emit({0x74, 0x00});       // jz   done
    emit({0x6A, 0x00});                                   // push 0              ; unused argument
    emitCall(refreshPanel);                               // callee pops the argument
    const size_t done = n;
    emit({0xB0, 0x01});                                   // done: mov al,1      ; handled
    emit({0xE9}); emitDword(epilogue - (reinterpret_cast<DWORD>(stubs) + n + 4));
    code[jzNoTv + 1] = static_cast<BYTE>(done - (jzNoTv + 2));
    code[jzNoPanel + 1] = static_cast<BYTE>(done - (jzNoPanel + 2));

    // Refresh stub, replacing the 16 bytes that feed the comment button its state: there ebx is
    // TV+0x10 and esi the inner panel, and the toggle's setter takes the state as one argument.
    BYTE* refreshStub = stubs + n;
    emit({0x80, 0x7B, 0x02, 0x00});                       // cmp  byte [ebx+2],0
    emit({0x0F, 0x94, 0xC2});                             // sete dl             ; shown -> state 0
    emit({0x0F, 0xB6, 0xD2});                             // movzx edx,dl
    emit({0x52});                                         // push edx
    emit({0x8D, 0x8E}); emitDword(0xCD4);                 // lea  ecx,[esi+0xCD4] ; the comment button
    emitCall(setToggleState);                             // callee pops the argument
    emit({0xC3});                                         // ret
    std::memcpy(stubs, code, n);
    FlushInstructionCache(GetCurrentProcess(), stubs, n);

    // Second image: push 190 -> push <hidden image> in the panel's create.
    BYTE expectedPush[] = {0x68, 0xBE, 0x00, 0x00, 0x00};
    BYTE newPush[] = {0x68, 0, 0, 0, 0};
    std::memcpy(newPush + 1, &hiddenImage, 4);
    if (!PatchBytes(0x647C07, expectedPush, newPush, sizeof(newPush)))
    {
        OutputDebugStringW(L"aisp.hook: TV comment button: create call differs, not patched\n");
        return;
    }
    // State feed: movzx edx,[ebx+2]; push edx; lea ecx,[esi+0xCD4]; call SetToggleState
    // -> call refreshStub; nop...
    BYTE expectedFeed[] = {0x0F, 0xB6, 0x53, 0x02, 0x52, 0x8D, 0x8E, 0xD4, 0x0C, 0x00, 0x00, 0xE8, 0x3E, 0xF2, 0xFF, 0xFF};
    BYTE newFeed[16];
    std::memset(newFeed, 0x90, sizeof(newFeed));
    newFeed[0] = 0xE8;
    const DWORD feedRel = reinterpret_cast<DWORD>(refreshStub) - (0x647DB2 + 5);
    std::memcpy(newFeed + 1, &feedRel, 4);
    if (!PatchBytes(0x647DB2, expectedFeed, newFeed, sizeof(newFeed)))
    {
        OutputDebugStringW(L"aisp.hook: TV comment button: refresh differs, not patched\n");
        return;
    }

    // Frames: call 0x77C300 inside the toggle's apply -> call CommentButtonApplyUnit.
    BYTE expectedApply[] = {0xE8, 0xA8, 0x5C, 0x2C, 0x00};
    BYTE newApply[] = {0xE8, 0, 0, 0, 0};
    const DWORD applyRel = reinterpret_cast<DWORD>(&CommentButtonApplyUnit) - (0x4B6653 + 5);
    std::memcpy(newApply + 1, &applyRel, 4);
    if (!PatchBytes(0x4B6653, expectedApply, newApply, sizeof(newApply)))
    {
        OutputDebugStringW(L"aisp.hook: TV comment button: toggle apply differs, not patched\n");
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(tableSlot, sizeof(*tableSlot), PAGE_READWRITE, &oldProtect))
        return;
    *tableSlot = reinterpret_cast<DWORD>(clickStub);
    DWORD ignored = 0;
    VirtualProtect(tableSlot, sizeof(*tableSlot), oldProtect, &ignored);
    OutputDebugStringW(L"aisp.hook: TV comment button: toggles comments locally\n");
}

} // namespace aisp
