#define WIN32_LEAN_AND_MEAN
// C-style COM vtables: the screen hook patches IWebBrowser2's vtable in place and needs the
// slot layout from the SDK header rather than hand-counted offsets.
#define CINTERFACE
#include <windows.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <exdisp.h>
#include <oleauto.h>
#include <cstring>
#include <cwchar>

namespace aisp
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
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);

GetACP_t g_originalGetACP = nullptr;
GetOEMCP_t g_originalGetOEMCP = nullptr;
GetThreadLocale_t g_originalGetThreadLocale = nullptr;
GetUserDefaultLCID_t g_originalGetUserDefaultLCID = nullptr;
GetSystemDefaultLCID_t g_originalGetSystemDefaultLCID = nullptr;
GetUserDefaultLangID_t g_originalGetUserDefaultLangID = nullptr;
GetSystemDefaultLangID_t g_originalGetSystemDefaultLangID = nullptr;
MultiByteToWideChar_t g_originalMultiByteToWideChar = nullptr;
WideCharToMultiByte_t g_originalWideCharToMultiByte = nullptr;
LoadLibraryW_t g_originalLoadLibraryW = nullptr;

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

bool IsD3d9LibraryPath(LPCWSTR path)
{
    if (!path || !*path)
        return false;

    const wchar_t* fileName = path;
    if (const wchar_t* slash = std::wcsrchr(path, L'\\'))
        fileName = slash + 1;
    if (const wchar_t* slash = std::wcsrchr(fileName, L'/'))
        fileName = slash + 1;

    return _wcsicmp(fileName, L"d3d9.dll") == 0 || _wcsicmp(fileName, L"d3d9") == 0;
}

bool BuildLocalD3d9Path(wchar_t* outPath, size_t outPathCount)
{
    if (!outPath || outPathCount == 0)
        return false;

    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    if (FAILED(StringCchCopyW(outPath, outPathCount, processPath)))
        return false;
    if (FAILED(StringCchCatW(outPath, outPathCount, L"d3d9.dll")))
        return false;

    return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!g_originalLoadLibraryW)
        return nullptr;

    if (IsD3d9LibraryPath(lpLibFileName))
    {
        wchar_t localD3d9Path[MAX_PATH] = {};
        if (BuildLocalD3d9Path(localD3d9Path, sizeof(localD3d9Path) / sizeof(localD3d9Path[0])))
            return g_originalLoadLibraryW(localD3d9Path);
    }

    return g_originalLoadLibraryW(lpLibFileName);
}

// ---------------------------------------------------------------------------------------------
// In-game screens.
//
// Every in-game display (room TVs, channel screens, the Nico Live billboard) is an IE WebBrowser
// control hosted by the client's statically linked ATL AxHost. The host creates it with
// CoCreateInstance(CLSID_WebBrowser) and calls IWebBrowser2::Navigate exactly once with a URL
// built from the client's own templates:
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=<id>       a TV given a movie id
//   http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?tvid=<n>&chid=<n>  a channel screen
//   http://live.nicovideo.jp/watch/<id>?npwarn=false#player               the live billboard
// Neither host serves the game any more, so Navigate is patched to send them to the emulator,
// keeping the distinction:
//   <base>room-tv?movieid=<id>         <base>channel-screen?tvid=<n>&chid=<n>
//   <base>live-watch?liveid=<id>       <base>screen?url=<anything else on aisp.jp>
// <base> is http://<download host>/<directory of the download path>/ from connection.txt
// (lines 4 and 5, written by the launcher), e.g. http://host/ai-sp/ or http://host/ai-sp/dev/,
// or the AISP_SCREEN_BASE environment variable verbatim. javascript: navigations pass through.
// ---------------------------------------------------------------------------------------------

using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using Navigate_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, BSTR, VARIANT*, VARIANT*, VARIANT*, VARIANT*);
using Navigate2_t = HRESULT(STDMETHODCALLTYPE*)(IWebBrowser2*, VARIANT*, VARIANT*, VARIANT*, VARIANT*, VARIANT*);

CoCreateInstance_t g_originalCoCreateInstance = nullptr;
Navigate_t g_originalNavigate = nullptr;
Navigate2_t g_originalNavigate2 = nullptr;
bool g_webBrowserPatched = false;
wchar_t g_screenBase[1024] = {};

const GUID kClsidWebBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
const GUID kIidWebBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};

void DebugLog(const wchar_t* format, const wchar_t* arg)
{
    wchar_t line[2048] = {};
    if (SUCCEEDED(StringCchPrintfW(line, 2048, format, arg)))
        OutputDebugStringW(line);
}

bool BuildGameFilePath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount)
{
    wchar_t processPath[MAX_PATH] = {};
    const DWORD processPathLen = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (processPathLen == 0 || processPathLen >= MAX_PATH)
        return false;

    wchar_t* lastSlash = std::wcsrchr(processPath, L'\\');
    if (!lastSlash)
        return false;
    *(lastSlash + 1) = L'\0';

    return SUCCEEDED(StringCchCopyW(outPath, outPathCount, processPath)) && SUCCEEDED(StringCchCatW(outPath, outPathCount, fileName));
}

constexpr wchar_t kTvHost[] = L"http://aisp.jp";
constexpr wchar_t kLiveHost[] = L"http://live.nicovideo.jp/watch/";

bool ReadConnectionValue(char lineNumber, wchar_t* out, size_t outCount)
{
    wchar_t path[MAX_PATH] = {};
    if (!BuildGameFilePath(L"connection.txt", path, MAX_PATH))
        return false;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    char buffer[4096] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;
    buffer[read] = '\0';

    const char* line = buffer;
    while (line && *line)
    {
        const char* end = std::strchr(line, '\n');
        if (line[0] == lineNumber && line[1] == ',')
        {
            const char* value = line + 2;
            const char* comma = std::strchr(value, ',');
            const char* stop = comma ? comma : (end ? end : value + std::strlen(value));
            while (stop > value && (stop[-1] == '\r' || stop[-1] == ' '))
                --stop;
            const int length = static_cast<int>(stop - value);
            if (length <= 0 || static_cast<size_t>(length) >= outCount)
                return false;
            const int converted = MultiByteToWideChar(CP_ACP, 0, value, length, out, static_cast<int>(outCount) - 1);
            if (converted <= 0)
                return false;
            out[converted] = L'\0';
            return true;
        }
        line = end ? end + 1 : nullptr;
    }
    return false;
}

void InitScreenBase()
{
    if (GetEnvironmentVariableW(L"AISP_SCREEN_BASE", g_screenBase, 1024) > 0 && g_screenBase[0])
    {
        DebugLog(L"aisp.hook: screen base from environment: %s\n", g_screenBase);
        return;
    }

    wchar_t host[512] = {}, downloadPath[512] = {};
    if (!ReadConnectionValue('4', host, 512))
    {
        g_screenBase[0] = L'\0';
        OutputDebugStringW(L"aisp.hook: no download host in connection.txt; screens are not redirected\n");
        return;
    }
    // The download path names the environment's endpoint directory: ai-sp/download.php or
    // ai-sp/dev/download.php. The screen pages live next to it.
    if (!ReadConnectionValue('5', downloadPath, 512))
        StringCchCopyW(downloadPath, 512, L"ai-sp/download.php");
    if (wchar_t* slash = std::wcsrchr(downloadPath, L'/'))
        *slash = L'\0';
    else
        StringCchCopyW(downloadPath, 512, L"ai-sp");
    while (downloadPath[0] == L'/')
        std::wmemmove(downloadPath, downloadPath + 1, std::wcslen(downloadPath));

    if (FAILED(StringCchPrintfW(g_screenBase, 1024, L"http://%s/%s/", host, downloadPath)))
        g_screenBase[0] = L'\0';
    DebugLog(L"aisp.hook: screen base: %s\n", g_screenBase);
}

bool AppendW(wchar_t* buffer, size_t count, const wchar_t* text)
{
    return SUCCEEDED(StringCchCatW(buffer, count, text));
}

bool AppendPercentEncoded(wchar_t* buffer, size_t count, const wchar_t* text)
{
    for (const wchar_t* p = text; *p; ++p)
    {
        const wchar_t c = *p;
        const bool safe = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' || c == L'_' || c == L'~';
        wchar_t piece[16] = {};
        if (safe)
        {
            piece[0] = c;
        }
        else if (c < 0x80)
        {
            StringCchPrintfW(piece, 16, L"%%%02X", static_cast<unsigned>(c));
        }
        else
        {
            char utf8[8] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, 8, nullptr, nullptr);
            wchar_t* out = piece;
            for (int i = 0; i < n; ++i)
                out += swprintf(out, 4, L"%%%02X", static_cast<unsigned char>(utf8[i]));
        }
        if (!AppendW(buffer, count, piece))
            return false;
    }
    return true;
}

// The client keeps the current channel and map in two globals written by its notify_change_map
// handler ([0xA5DAE0] channel, [0xA5DAE4] map; verified in the binary, which loads unrelocated
// at 0x400000). The URL a screen asks for only carries the screen's own ids, so these tell the
// server which map's screens a page is for.
bool ReadClientMapContext(DWORD* mapId, DWORD* channelId)
{
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000))
        return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(0xA5DAE0), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS)
        return false;
    *channelId = *reinterpret_cast<volatile DWORD*>(0xA5DAE0);
    *mapId = *reinterpret_cast<volatile DWORD*>(0xA5DAE4);
    return true;
}

// Appends map=<id>&ch=<id> to a screen URL, ahead of any #fragment.
void AppendClientContext(wchar_t* url, size_t count)
{
    DWORD mapId = 0, channelId = 0;
    if (!ReadClientMapContext(&mapId, &channelId))
        return;
    wchar_t fragment[512] = {};
    if (wchar_t* hash = std::wcschr(url, L'#'))
    {
        StringCchCopyW(fragment, 512, hash);
        *hash = L'\0';
    }
    wchar_t context[64] = {};
    StringCchPrintfW(context, 64, L"%smap=%lu&ch=%lu", std::wcschr(url, L'?') ? L"&" : L"?", mapId, channelId);
    AppendW(url, count, context);
    AppendW(url, count, fragment);
}

// Returns a new BSTR with the emulator URL for a screen URL, or nullptr for anything else.
BSTR RewriteScreenUrl(const wchar_t* url)
{
    if (!url || !g_screenBase[0])
        return nullptr;

    wchar_t rewritten[4096] = {};
    StringCchCopyW(rewritten, 4096, g_screenBase);

    const size_t tvHostLength = std::wcslen(kTvHost);
    const size_t liveHostLength = std::wcslen(kLiveHost);
    if (_wcsnicmp(url, kTvHost, tvHostLength) == 0 && (url[tvHostLength] == L'/' || url[tvHostLength] == L'?' || url[tvHostLength] == L'#' || url[tvHostLength] == L'\0'))
    {
        const wchar_t* query = std::wcschr(url, L'?');
        const wchar_t* fragment = std::wcschr(url, L'#');
        const wchar_t* tail = query ? query + 1 : (fragment ? fragment : L"");
        if (query && std::wcsstr(tail, L"movieid="))
        {
            AppendW(rewritten, 4096, L"room-tv?");
            AppendW(rewritten, 4096, tail);
        }
        else if (query && std::wcsstr(tail, L"tvid="))
        {
            AppendW(rewritten, 4096, L"channel-screen?");
            AppendW(rewritten, 4096, tail);
        }
        else
        {
            AppendW(rewritten, 4096, L"screen?url=");
            AppendPercentEncoded(rewritten, 4096, url);
        }
    }
    else if (_wcsnicmp(url, kLiveHost, liveHostLength) == 0)
    {
        const wchar_t* id = url + liveHostLength;
        size_t idLength = 0;
        while (id[idLength] && id[idLength] != L'?' && id[idLength] != L'#' && id[idLength] != L'/')
            ++idLength;
        AppendW(rewritten, 4096, L"live-watch?liveid=");
        wchar_t idCopy[256] = {};
        StringCchCopyNW(idCopy, 256, id, idLength);
        AppendPercentEncoded(rewritten, 4096, idCopy);
        const wchar_t* rest = id + idLength;
        if (*rest == L'?')
        {
            AppendW(rewritten, 4096, L"&");
            AppendW(rewritten, 4096, rest + 1);
        }
        else if (*rest)
        {
            AppendW(rewritten, 4096, rest);
        }
    }
    else
    {
        return nullptr;
    }

    AppendClientContext(rewritten, 4096);
    DebugLog(L"aisp.hook: screen navigate -> %s\n", rewritten);
    return SysAllocString(rewritten);
}

// --- browser patch -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookNavigate(IWebBrowser2* self, BSTR url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    BSTR rewritten = RewriteScreenUrl(url);
    const HRESULT hr = g_originalNavigate(self, rewritten ? rewritten : url, flags, targetFrameName, postData, headers);
    if (rewritten)
        SysFreeString(rewritten);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookNavigate2(IWebBrowser2* self, VARIANT* url, VARIANT* flags, VARIANT* targetFrameName, VARIANT* postData, VARIANT* headers)
{
    if (url && V_VT(url) == VT_BSTR)
    {
        BSTR rewritten = RewriteScreenUrl(V_BSTR(url));
        if (rewritten)
        {
            VARIANT replaced;
            VariantInit(&replaced);
            V_VT(&replaced) = VT_BSTR;
            V_BSTR(&replaced) = rewritten;
            const HRESULT hr = g_originalNavigate2(self, &replaced, flags, targetFrameName, postData, headers);
            SysFreeString(rewritten);
            return hr;
        }
    }
    return g_originalNavigate2(self, url, flags, targetFrameName, postData, headers);
}

// All WebBrowser instances share ieframe's vtable, so one patch covers every screen.
void PatchWebBrowserVtable(IUnknown* unknown)
{
    if (g_webBrowserPatched || !unknown)
        return;

    IWebBrowser2* browser = nullptr;
    if (FAILED(unknown->lpVtbl->QueryInterface(unknown, kIidWebBrowser2, reinterpret_cast<void**>(&browser))) || !browser)
        return;

    IWebBrowser2Vtbl* vtable = browser->lpVtbl;
    DWORD oldProtect = 0;
    if (VirtualProtect(vtable, sizeof(*vtable), PAGE_READWRITE, &oldProtect))
    {
        g_originalNavigate = vtable->Navigate;
        g_originalNavigate2 = vtable->Navigate2;
        vtable->Navigate = HookNavigate;
        vtable->Navigate2 = HookNavigate2;
        DWORD ignored = 0;
        VirtualProtect(vtable, sizeof(*vtable), oldProtect, &ignored);
        g_webBrowserPatched = true;
        OutputDebugStringW(L"aisp.hook: IWebBrowser2::Navigate patched\n");
    }

    browser->lpVtbl->Release(browser);
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out)
{
    if (!g_originalCoCreateInstance)
        return E_FAIL;

    const HRESULT hr = g_originalCoCreateInstance(clsid, outer, context, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(clsid, kClsidWebBrowser))
        PatchWebBrowserVtable(static_cast<IUnknown*>(*out));
    return hr;
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
    PatchImport(module, "LoadLibraryW", reinterpret_cast<void*>(HookLoadLibraryW), &g_originalLoadLibraryW);
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

} // namespace aisp

using namespace aisp;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        PatchLoadedModules();
        // The screen hook only concerns the game executable's own import (the ATL host is
        // linked into it); other modules keep the real function.
        InitScreenBase();
        PatchSingleImport(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(HookCoCreateInstance), &g_originalCoCreateInstance);
    }
    return TRUE;
}
