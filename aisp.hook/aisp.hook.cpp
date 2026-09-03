#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <cstring>
#include <cwchar>

namespace
{
constexpr UINT kJapaneseCodePage = 932;
constexpr LCID kJapaneseLcid = 0x0411;
constexpr LANGID kJapaneseLangId = 0x0411;

using GetACP_t = UINT(WINAPI*)();
using GetOEMCP_t = UINT(WINAPI*)();
using GetThreadLocale_t = LCID(WINAPI*)();
using GetUserDefaultLCID_t = LCID(WINAPI*)();
using GetSystemDefaultLCID_t = LCID(WINAPI*)();
using GetUserDefaultLangID_t = LANGID(WINAPI*)();
using GetSystemDefaultLangID_t = LANGID(WINAPI*)();
using MultiByteToWideChar_t = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
using WideCharToMultiByte_t = int(WINAPI*)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using OleDraw_t = HRESULT(WINAPI*)(IUnknown*, DWORD, HDC, LPCRECT);
using StartCefRenderer_t = void(WINAPI*)(LPCWSTR, int, int);
using DrawCefFrame_t = bool(WINAPI*)(HDC, const RECT*);

GetACP_t g_originalGetACP = nullptr;
GetOEMCP_t g_originalGetOEMCP = nullptr;
GetThreadLocale_t g_originalGetThreadLocale = nullptr;
GetUserDefaultLCID_t g_originalGetUserDefaultLCID = nullptr;
GetSystemDefaultLCID_t g_originalGetSystemDefaultLCID = nullptr;
GetUserDefaultLangID_t g_originalGetUserDefaultLangID = nullptr;
GetSystemDefaultLangID_t g_originalGetSystemDefaultLangID = nullptr;
MultiByteToWideChar_t g_originalMultiByteToWideChar = nullptr;
WideCharToMultiByte_t g_originalWideCharToMultiByte = nullptr;
CreateWindowExW_t g_originalCreateWindowExW = nullptr;
OleDraw_t g_originalOleDraw = nullptr;
StartCefRenderer_t g_startCefRenderer = nullptr;
DrawCefFrame_t g_drawCefFrame = nullptr;
HMODULE g_cefRendererModule = nullptr;
bool g_browserReplacementPocEnabled = false;
LONG g_loggedCefDrawFallback = 0;
LONG g_loggedCefDrawSuccess = 0;
HWND g_gameWindow = nullptr;
RECT g_gameWindowRect = {};
wchar_t g_gameWindowTitle[256] = {};
bool g_gameWindowStateCaptured = false;

void PatchModule(HMODULE module);

UINT WINAPI HookGetACP()
{
    return kJapaneseCodePage;
}

UINT WINAPI HookGetOEMCP()
{
    return kJapaneseCodePage;
}

LCID WINAPI HookGetThreadLocale()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetUserDefaultLCID()
{
    return kJapaneseLcid;
}

LCID WINAPI HookGetSystemDefaultLCID()
{
    return kJapaneseLcid;
}

LANGID WINAPI HookGetUserDefaultLangID()
{
    return kJapaneseLangId;
}

LANGID WINAPI HookGetSystemDefaultLangID()
{
    return kJapaneseLangId;
}

int WINAPI HookMultiByteToWideChar(UINT codePage, DWORD dwFlags, LPCCH multiByteStr, int cbMultiByte, LPWSTR wideCharStr, int cchWideChar)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalMultiByteToWideChar ? g_originalMultiByteToWideChar(codePage, dwFlags, multiByteStr, cbMultiByte, wideCharStr, cchWideChar) : 0;
}

int WINAPI HookWideCharToMultiByte(
    UINT codePage,
    DWORD dwFlags,
    LPCWCH wideCharStr,
    int cchWideChar,
    LPSTR multiByteStr,
    int cbMultiByte,
    LPCCH defaultChar,
    LPBOOL usedDefaultChar
)
{
    if (codePage == CP_ACP || codePage == CP_THREAD_ACP)
        codePage = kJapaneseCodePage;
    return g_originalWideCharToMultiByte
               ? g_originalWideCharToMultiByte(codePage, dwFlags, wideCharStr, cchWideChar, multiByteStr, cbMultiByte, defaultChar, usedDefaultChar)
               : 0;
}

bool IsAtlAxHostClass(LPCWSTR className)
{
    return reinterpret_cast<ULONG_PTR>(className) > 0xFFFF && _wcsicmp(className, L"AtlAxWin80") == 0;
}

bool IsLocalNicoPlayerUrl(LPCWSTR source)
{
    constexpr wchar_t kPrefix[] = L"http://127.0.0.1/p?";
    return source && std::wcsncmp(source, kPrefix, _countof(kPrefix) - 1) == 0;
}

void WriteBrowserPocTraceLine(LPCWSTR line)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    wchar_t* fileName = std::wcsrchr(path, L'\\');
    if (!fileName || FAILED(StringCchCopyW(fileName + 1, MAX_PATH - (fileName + 1 - path), L"aisp.browser-poc.log")))
        return;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

void WriteBrowserPocTrace(LPCWSTR source)
{
    wchar_t line[512] = {};
    if (SUCCEEDED(StringCchPrintfW(line, _countof(line), L"AtlAxWin80: %s\r\n", source ? source : L"(null)")))
        WriteBrowserPocTraceLine(line);
}

void CaptureGameWindowState(HWND child)
{
    HWND window = child ? GetAncestor(child, GA_ROOT) : nullptr;
    DWORD processId = 0;
    if (!window || !GetWindowThreadProcessId(window, &processId) || processId != GetCurrentProcessId())
        return;

    RECT rect = {};
    if (!GetWindowRect(window, &rect))
        return;

    g_gameWindow = window;
    g_gameWindowRect = rect;
    GetWindowTextW(window, g_gameWindowTitle, _countof(g_gameWindowTitle));
    g_gameWindowStateCaptured = true;
}

void RestoreGameWindowState()
{
    if (!g_gameWindowStateCaptured || !IsWindow(g_gameWindow))
        return;

    if (g_gameWindowTitle[0])
        SetWindowTextW(g_gameWindow, g_gameWindowTitle);
    SetWindowPos(
        g_gameWindow,
        nullptr,
        g_gameWindowRect.left,
        g_gameWindowRect.top,
        g_gameWindowRect.right - g_gameWindowRect.left,
        g_gameWindowRect.bottom - g_gameWindowRect.top,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED
    );
}

bool EnsureCefRendererLoaded()
{
    if (g_cefRendererModule)
        return g_startCefRenderer && g_drawCefFrame;

    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        WriteBrowserPocTraceLine(L"CEF plugin: unable to resolve game directory\r\n");
        return false;
    }

    wchar_t* fileName = std::wcsrchr(path, L'\\');
    if (!fileName)
    {
        WriteBrowserPocTraceLine(L"CEF plugin: game path has no directory\r\n");
        return false;
    }
    *fileName = L'\0';

    wchar_t runtimeDirectory[MAX_PATH] = {};
    wchar_t rendererPath[MAX_PATH] = {};
    if (
        FAILED(StringCchPrintfW(runtimeDirectory, _countof(runtimeDirectory), L"%s\\aisp.cef", path))
        || FAILED(StringCchPrintfW(rendererPath, _countof(rendererPath), L"%s\\aisp.cef-renderer.dll", runtimeDirectory))
    )
    {
        WriteBrowserPocTraceLine(L"CEF plugin: runtime path is too long\r\n");
        return false;
    }

    SetDllDirectoryW(runtimeDirectory);
    g_cefRendererModule = LoadLibraryW(rendererPath);
    if (!g_cefRendererModule)
    {
        wchar_t line[128] = {};
        StringCchPrintfW(line, _countof(line), L"CEF plugin: LoadLibraryW failed (%lu)\r\n", GetLastError());
        WriteBrowserPocTraceLine(line);
        return false;
    }

    // The CEF renderer is a 32-bit WINAPI DLL. MinGW exports stdcall symbols
    // with their argument-byte suffix, so resolve those exact names rather
    // than the undecorated C++ source identifiers.
    g_startCefRenderer = reinterpret_cast<StartCefRenderer_t>(GetProcAddress(g_cefRendererModule, "StartCefRenderer@12"));
    g_drawCefFrame = reinterpret_cast<DrawCefFrame_t>(GetProcAddress(g_cefRendererModule, "DrawCefFrame@8"));
    if (g_startCefRenderer && g_drawCefFrame)
    {
        WriteBrowserPocTraceLine(L"CEF plugin: loaded\r\n");
        return true;
    }

    WriteBrowserPocTraceLine(L"CEF plugin: required exports are missing\r\n");
    return false;
}

HWND WINAPI HookCreateWindowExW(
    DWORD extendedStyle,
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter
)
{
    // Stage 1 of the browser-replacement POC: verify the exact host creation
    // seam without changing behavior. The next stage replaces this ATL host and
    // answers WM_ATLGETCONTROL with an IWebBrowser2 compatibility shim.
    if (IsAtlAxHostClass(className))
    {
        OutputDebugStringW(L"[aisp browser POC] AtlAxWin80 source: ");
        OutputDebugStringW(windowName ? windowName : L"(null)");
        OutputDebugStringW(L"\n");
        WriteBrowserPocTrace(windowName);
        if (IsLocalNicoPlayerUrl(windowName) && EnsureCefRendererLoaded())
        {
            CaptureGameWindowState(parent);
            g_startCefRenderer(windowName, width, height);
            RestoreGameWindowState();
        }
    }

    return g_originalCreateWindowExW
               ? g_originalCreateWindowExW(
                     extendedStyle,
                     className,
                     windowName,
                     style,
                     x,
                     y,
                     width,
                     height,
                     parent,
                     menu,
                     instance,
                     parameter
                 )
               : nullptr;
}

HRESULT WINAPI HookOleDraw(IUnknown* unknown, DWORD aspect, HDC destination, LPCRECT destinationRect)
{
    // Trident is retained for the document and JavaScript bridge the client
    // already uses. Only its final draw into the game-owned TV bitmap is
    // replaced with CEF's off-screen BGRA frame.
    if (aspect == DVASPECT_CONTENT && g_drawCefFrame)
    {
        if (g_drawCefFrame(destination, destinationRect))
        {
            if (InterlockedCompareExchange(&g_loggedCefDrawSuccess, 1, 0) == 0)
            {
                RestoreGameWindowState();
                WriteBrowserPocTraceLine(L"CEF texture: first frame drawn\r\n");
            }
            return S_OK;
        }

        if (InterlockedCompareExchange(&g_loggedCefDrawFallback, 1, 0) == 0)
            WriteBrowserPocTraceLine(L"CEF texture: no frame available; using Trident fallback\r\n");
    }

    return g_originalOleDraw ? g_originalOleDraw(unknown, aspect, destination, destinationRect) : E_FAIL;
}

template <typename T>
bool PatchSingleImport(HMODULE module, const char* importedModuleName, const char* importName, void* replacement, T* original)
{
    if (!module)
        return false;

    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress || !importDir.Size)
        return false;

    auto importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    while (importDesc->Name)
    {
        auto moduleName = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(moduleName, importedModuleName) == 0)
        {
            auto originalThunk = importDesc->OriginalFirstThunk
                                     ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->OriginalFirstThunk)
                                     : reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);

            while (originalThunk->u1.AddressOfData)
            {
                if ((originalThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) == 0)
                {
                    auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), importName) == 0)
                    {
                        DWORD oldProtect = 0;
                        if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                            return false;

                        if (original && !*original)
                            *original = reinterpret_cast<T>(thunk->u1.Function);
                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

                        FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));

                        DWORD ignored = 0;
                        VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignored);
                        return true;
                    }
                }

                ++originalThunk;
                ++thunk;
            }
        }
        ++importDesc;
    }

    return false;
}

template <typename T>
void PatchImport(HMODULE module, const char* importName, void* replacement, T* original)
{
    PatchSingleImport(module, "KERNEL32.dll", importName, replacement, original);
    PatchSingleImport(module, "KERNELBASE.dll", importName, replacement, original);
}

void PatchModule(HMODULE module)
{
    PatchImport(module, "GetACP", reinterpret_cast<void*>(HookGetACP), &g_originalGetACP);
    PatchImport(module, "GetOEMCP", reinterpret_cast<void*>(HookGetOEMCP), &g_originalGetOEMCP);
    PatchImport(module, "GetThreadLocale", reinterpret_cast<void*>(HookGetThreadLocale), &g_originalGetThreadLocale);
    PatchImport(module, "GetUserDefaultLCID", reinterpret_cast<void*>(HookGetUserDefaultLCID), &g_originalGetUserDefaultLCID);
    PatchImport(module, "GetSystemDefaultLCID", reinterpret_cast<void*>(HookGetSystemDefaultLCID), &g_originalGetSystemDefaultLCID);
    PatchImport(module, "GetUserDefaultLangID", reinterpret_cast<void*>(HookGetUserDefaultLangID), &g_originalGetUserDefaultLangID);
    PatchImport(module, "GetSystemDefaultLangID", reinterpret_cast<void*>(HookGetSystemDefaultLangID), &g_originalGetSystemDefaultLangID);
    PatchImport(module, "MultiByteToWideChar", reinterpret_cast<void*>(HookMultiByteToWideChar), &g_originalMultiByteToWideChar);
    PatchImport(module, "WideCharToMultiByte", reinterpret_cast<void*>(HookWideCharToMultiByte), &g_originalWideCharToMultiByte);

    if (g_browserReplacementPocEnabled && module == GetModuleHandleW(nullptr))
    {
        PatchSingleImport(module, "USER32.dll", "CreateWindowExW", reinterpret_cast<void*>(HookCreateWindowExW), &g_originalCreateWindowExW);
        PatchSingleImport(module, "OLE32.dll", "OleDraw", reinterpret_cast<void*>(HookOleDraw), &g_originalOleDraw);
    }
}

void PatchLoadedModules()
{
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32 moduleEntry = {};
    moduleEntry.dwSize = sizeof(moduleEntry);

    if (Module32First(snapshot, &moduleEntry))
    {
        do
        {
            PatchModule(moduleEntry.hModule);
        } while (Module32Next(snapshot, &moduleEntry));
    }

    CloseHandle(snapshot);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        wchar_t browserPocSetting[2] = {};
        g_browserReplacementPocEnabled = GetEnvironmentVariableW(L"AISP_BROWSER_POC", browserPocSetting, 2) == 1
                                     && browserPocSetting[0] == L'1';
        PatchLoadedModules();
    }
    return TRUE;
}
