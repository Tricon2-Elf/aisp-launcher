// HTTPS for the client's web traffic. Configured by [web] in aisp.hook.ini (config.h): on by
// itself for an endpoint whose host in connection.txt is one of https_hosts (aisp.moe,
// example.com and nbspou.net unless changed), or forced on or off for both; the environment
// overrides either way.
//
// The client reaches its web endpoints in two ways, neither of which can do TLS:
//
//   download.php (and the Nico login)  WinINet, through a generic POST routine in the client
//     that takes a "secure" flag its download caller leaves off: InternetConnectW to port 80,
//     HttpOpenRequestW without INTERNET_FLAG_SECURE. Two import hooks fix that in place:
//     InternetConnectW turns port 80 into 443 for HTTP service, HttpOpenRequestW adds the
//     secure flag. The client also tells WinINet to ignore every certificate problem
//     (SECURITY_FLAG_IGNORE_UNKNOWN_CA and friends via InternetSetOptionW); the third hook
//     drops those bits so the certificate is actually checked, unless [web] insecure=1.
//
//   upload.php (drama discs, AI tunes)  the client's own http::HTTP class writes the request
//     text by hand ("POST /<path> HTTP/1.1", Host, a multipart body) into a VCE stream. The
//     uploader object (CUCCUploaderBase, a vce::Session) asks the global VCE core (ivce::iVCE) to
//     connect it to <upload host>:80; the request bytes go through vce::Session::send into the stream's
//     write slot, and the reply comes back through the Session callbacks the uploader overrides:
//     onRecv (vtable slot 8) parses the HTTP text for the XML and calls the finished callback,
//     onClose (slot 5) marks the session closed. The hook takes the VCE core's connect slot: for
//     an uploader session on port 80 it attaches a stream object of its own instead of a socket.
//     That object collects the request the client writes, performs it over WinHTTP on a worker
//     thread, and hands the reply text to the session's onRecv and onClose on the game's thread,
//     from the session's own state poll or from a PeekMessageW import hook, where VCE's poll would
//     have delivered it. Unless [web] upload_hook=0 this happens for plain-HTTP hosts as well, with
//     the replay on port 80: the reply and the close then always arrive together, which the
//     client's uploader needs (its previous instance's close landing during the next upload wipes
//     that upload's job), whatever the server does with the connection. Nothing in the
//     client's own request writer, parser or state machine changes, and no socket is involved.
//
// The screen base built in aisp.hook.cpp follows [screens] https, auto against the same
// https_hosts list; the IE control does TLS on its own.
//
// Only the game executable's imports and one static vtable slot are patched: the IE control,
// Electron and WinHTTP's own sockets live elsewhere and keep the real functions. wininet.h and
// winhttp.h do not coexist in one translation unit, so the WinINet constants and signatures used
// are spelled out here.

#include <windows.h>
#include <winhttp.h>
#include <strsafe.h>

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "config.h"
#include "https.h"

namespace aisp
{
void DebugLog(const wchar_t* format, const wchar_t* arg);                   // screen.cpp
void LogLine(const char* text);                                              // screen.cpp
bool ReadConnectionValue(char lineNumber, wchar_t* out, size_t outCount); // aisp.hook.cpp

namespace
{
// WinINet, as far as the hooks need it.
using HINTERNET_ = LPVOID;
using INTERNET_PORT_ = WORD;
constexpr INTERNET_PORT_ kInternetDefaultHttpPort = 80;
constexpr INTERNET_PORT_ kInternetDefaultHttpsPort = 443;
constexpr DWORD kInternetServiceHttp = 3;
constexpr DWORD kInternetFlagSecure = 0x00800000;
constexpr DWORD kInternetOptionSecurityFlags = 31;
constexpr DWORD kSecurityFlagIgnoreRevocation = 0x00000080;
constexpr DWORD kSecurityFlagIgnoreUnknownCa = 0x00000100;
constexpr DWORD kSecurityFlagIgnoreWrongUsage = 0x00000200;
constexpr DWORD kSecurityFlagIgnoreCertCnInvalid = 0x00001000;
constexpr DWORD kSecurityFlagIgnoreCertDateInvalid = 0x00002000;
constexpr DWORD kCertificateIgnoreBits = kSecurityFlagIgnoreRevocation | kSecurityFlagIgnoreUnknownCa | kSecurityFlagIgnoreWrongUsage | kSecurityFlagIgnoreCertCnInvalid | kSecurityFlagIgnoreCertDateInvalid;

using InternetConnectW_t = HINTERNET_(WINAPI*)(HINTERNET_, LPCWSTR, INTERNET_PORT_, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
using HttpOpenRequestW_t = HINTERNET_(WINAPI*)(HINTERNET_, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD, DWORD_PTR);
using InternetSetOptionW_t = BOOL(WINAPI*)(HINTERNET_, DWORD, LPVOID, DWORD);
using PeekMessageW_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

InternetConnectW_t g_originalInternetConnectW = nullptr;
HttpOpenRequestW_t g_originalHttpOpenRequestW = nullptr;
InternetSetOptionW_t g_originalInternetSetOptionW = nullptr;
PeekMessageW_t g_originalPeekMessageW = nullptr;

// Per endpoint: download.php goes through WinINet, upload.php through the client's VCE stream.
bool g_downloadTls = false;
bool g_uploadTls = false;
bool g_uploadHook = true;  // [web] upload_hook: the uploader's stream is the hook's even for plain HTTP
bool g_insecure = false;
bool g_flagsRead = false;
Switch g_switch = Switch::Auto;

// [web] base: a full replacement for where upload.php and download.php go, keeping the
// client's own file names. Parsed once; empty host means not set.
struct WebBase
{
    bool secure = false;
    std::wstring host;
    WORD port = 0;
    std::wstring directory; // begins and ends with '/'
};
WebBase g_webBase;

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 64u * 1024 * 1024;

// To the debugger and to aisp.screen.log, so a user can see where the endpoints went.
void Log(const wchar_t* text)
{
    DebugLog(L"%s\n", text);
    char line[1100] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line, 1090, nullptr, nullptr);
    if (n > 1)
    {
        StringCchCatA(line, 1100, "\r\n");
        LogLine(line);
    }
}

void LogF(const wchar_t* format, const wchar_t* arg)
{
    wchar_t line[1024] = {};
    StringCchPrintfW(line, 1024, format, arg);
    Log(line);
}

void LogN(const wchar_t* format, unsigned value)
{
    wchar_t line[1024] = {};
    StringCchPrintfW(line, 1024, format, value);
    Log(line);
}

bool HostMatchesDomain(const wchar_t* host, size_t hostLength, const wchar_t* domain, size_t domainLength)
{
    if (domainLength == 0 || hostLength < domainLength || _wcsnicmp(host + hostLength - domainLength, domain, domainLength) != 0)
        return false;
    return hostLength == domainLength || host[hostLength - domainLength - 1] == L'.';
}

// The hosts that serve their endpoints over TLS, so the hook turns HTTPS on for them without
// being asked: [web] https_hosts, a comma-separated list of domains, subdomains included;
// aisp.moe, example.com and nbspou.net when not set. A host may carry a :port from a hand-edited connection.txt.
bool IsHttpsHost(const wchar_t* host)
{
    size_t hostLength = std::wcslen(host);
    if (const wchar_t* colon = std::wcschr(host, L':'))
        hostLength = static_cast<size_t>(colon - host);
    wchar_t list[1024] = {};
    if (!ConfigString(L"AISP_HTTPS_HOSTS", L"web", L"https_hosts", list, 1024))
        StringCchCopyW(list, 1024, L"aisp.moe,example.com,nbspou.net");
    const wchar_t* entry = list;
    while (*entry)
    {
        while (*entry == L' ' || *entry == L',' || *entry == L';')
            ++entry;
        const wchar_t* end = entry;
        while (*end && *end != L',' && *end != L';' && *end != L' ')
            ++end;
        if (HostMatchesDomain(host, hostLength, entry, static_cast<size_t>(end - entry)))
            return true;
        entry = end;
    }
    return false;
}

bool ConnectionHostIsHttps(char lineNumber)
{
    wchar_t host[512] = {};
    return ReadConnectionValue(lineNumber, host, 512) && IsHttpsHost(host);
}

void ReadFlags()
{
    if (g_flagsRead)
        return;
    g_flagsRead = true;
    g_switch = ConfigSwitch(L"AISP_HTTPS", L"web", L"https");
    switch (g_switch)
    {
    case Switch::On: g_downloadTls = g_uploadTls = true; break;
    case Switch::Off: g_downloadTls = g_uploadTls = false; break;
    case Switch::Auto:
        g_downloadTls = ConnectionHostIsHttps('4');
        g_uploadTls = ConnectionHostIsHttps('6');
        break;
    }
    g_insecure = ConfigSwitch(L"AISP_HTTPS_INSECURE", L"web", L"insecure") == Switch::On;
    g_uploadHook = ConfigSwitch(L"AISP_UPLOAD_HOOK", L"web", L"upload_hook") != Switch::Off;

    wchar_t base[1024] = {};
    if (ConfigString(L"AISP_WEB_BASE", L"web", L"base", base, 1024))
    {
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {}, path[512] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 512;
        if (WinHttpCrackUrl(base, 0, 0, &parts) && host[0] && (parts.nScheme == INTERNET_SCHEME_HTTP || parts.nScheme == INTERNET_SCHEME_HTTPS))
        {
            g_webBase.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
            g_webBase.host = host;
            g_webBase.port = parts.nPort;
            g_webBase.directory = path[0] ? path : L"/";
            if (size_t query = g_webBase.directory.find_first_of(L"?#"); query != std::wstring::npos)
                g_webBase.directory.resize(query);
            if (g_webBase.directory.back() != L'/')
                g_webBase.directory += L'/';
            g_downloadTls = g_uploadTls = true; // the base decides everything, the https switch included
        }
        else
            LogF(L"aisp.hook: https: [web] base is not an http(s) URL, ignored: %s", base);
    }
}

bool WebBaseSet()
{
    return !g_webBase.host.empty();
}

// The part after the last '/', the client's own file name (upload.php, download.php).
std::wstring Leaf(const wchar_t* path)
{
    const wchar_t* slash = path ? std::wcsrchr(path, L'/') : nullptr;
    return slash ? slash + 1 : (path ? path : L"");
}

// --- import patching (by name or ordinal) -----------------------------------------------------

template <typename T>
bool PatchImport(HMODULE module, const char* dllName, const char* importName, WORD ordinal, void* replacement, T* original)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress || !importDir.Size)
        return false;

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dllName) != 0)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++thunks)
        {
            bool match = false;
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                match = ordinal != 0 && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal;
            else if (importName)
                match = std::strcmp(reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name), importName) == 0;
            if (!match)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;
            if (original && !*original)
                *original = reinterpret_cast<T>(thunks->u1.Function);
            thunks->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            FlushInstructionCache(GetCurrentProcess(), &thunks->u1.Function, sizeof(void*));
            DWORD ignored = 0;
            VirtualProtect(&thunks->u1.Function, sizeof(void*), oldProtect, &ignored);
            return true;
        }
    }
    return false;
}

// --- WinINet hooks: download.php --------------------------------------------------------------

HINTERNET_ WINAPI HookInternetConnectW(HINTERNET_ session, LPCWSTR server, INTERNET_PORT_ port, LPCWSTR user, LPCWSTR password, DWORD service, DWORD flags, DWORD_PTR context)
{
    if (service == kInternetServiceHttp && port == kInternetDefaultHttpPort)
    {
        if (WebBaseSet())
        {
            server = g_webBase.host.c_str();
            port = g_webBase.port;
            LogF(L"aisp.hook: https: WinINet connection moved to the [web] base host %s", server);
        }
        else
        {
            port = kInternetDefaultHttpsPort;
            LogF(L"aisp.hook: https: WinINet connection to %s moved to port 443", server ? server : L"?");
        }
    }
    return g_originalInternetConnectW(session, server, port, user, password, service, flags, context);
}

HINTERNET_ WINAPI HookHttpOpenRequestW(HINTERNET_ connection, LPCWSTR verb, LPCWSTR object, LPCWSTR version, LPCWSTR referrer, LPCWSTR* acceptTypes, DWORD flags, DWORD_PTR context)
{
    if (WebBaseSet())
    {
        std::wstring path = g_webBase.directory + Leaf(object);
        return g_originalHttpOpenRequestW(connection, verb, path.c_str(), version, referrer, acceptTypes, g_webBase.secure ? flags | kInternetFlagSecure : flags & ~kInternetFlagSecure, context);
    }
    return g_originalHttpOpenRequestW(connection, verb, object, version, referrer, acceptTypes, flags | kInternetFlagSecure, context);
}

BOOL WINAPI HookInternetSetOptionW(HINTERNET_ handle, DWORD option, LPVOID buffer, DWORD length)
{
    if (option == kInternetOptionSecurityFlags && buffer && length == sizeof(DWORD) && !g_insecure)
    {
        DWORD value = *static_cast<DWORD*>(buffer) & ~kCertificateIgnoreBits;
        return g_originalInternetSetOptionW(handle, option, &value, sizeof(value));
    }
    return g_originalInternetSetOptionW(handle, option, buffer, length);
}

// --- the request the client wrote, replayed over WinHTTP: upload.php -------------------------

struct Request
{
    std::string method;
    std::string target;
    std::string host;
    std::string contentType;
    size_t contentLength = 0;
    bool hasContentLength = false;
};

bool EqualsNoCase(const std::string& a, const char* b)
{
    return _stricmp(a.c_str(), b) == 0;
}

std::string Trim(const std::string& text)
{
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    size_t end = text.find_last_not_of(" \t\r");
    return text.substr(start, end - start + 1);
}

// Parses the request line and the headers the replay needs. Everything else the client sends
// (Keep-Alive, its own Content-Type parameters) is either irrelevant or passed on.
bool ParseRequest(const std::string& head, Request& request)
{
    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos)
        return false;
    std::string requestLine = head.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return false;
    request.method = requestLine.substr(0, sp1);
    request.target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    if (request.target.empty())
        return false;
    if (request.target[0] != '/')
        request.target.insert(request.target.begin(), '/');

    size_t pos = lineEnd + 2;
    while (pos < head.size())
    {
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos)
            next = head.size();
        std::string line = head.substr(pos, next - pos);
        pos = next + 2;
        if (line.empty())
            break;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = Trim(line.substr(0, colon));
        std::string value = Trim(line.substr(colon + 1));
        if (EqualsNoCase(name, "Host"))
            request.host = value;
        else if (EqualsNoCase(name, "Content-Type"))
            request.contentType = value;
        else if (EqualsNoCase(name, "Content-Length"))
        {
            request.contentLength = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
            request.hasContentLength = true;
        }
    }
    return !request.host.empty();
}

std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count > 0 ? count : 0), L'\0');
    if (count > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], count);
    return wide;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
        return "";
    int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(count > 0 ? count : 0), '\0');
    if (count > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &narrow[0], count, nullptr, nullptr);
    return narrow;
}

std::wstring QueryHeader(HINTERNET request, DWORD info)
{
    DWORD size = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return L"";
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, &value[0], &size, WINHTTP_NO_HEADER_INDEX))
        return L"";
    value.resize(size / sizeof(wchar_t));
    return value;
}

struct Response
{
    DWORD status = 502;
    std::string statusText = "Bad Gateway";
    std::string contentType = "text/xml; charset=utf-8";
    std::string body;
};

// One request to <host>/<target>, over TLS on 443 or plain on 80 as `tls` says. The host may
// carry a :port from a hand-edited connection.txt; it is ignored, the way the client always used 80.
bool Forward(const Request& request, const std::string& body, bool tls, Response& response)
{
    std::string hostOnly = request.host;
    if (size_t colon = hostOnly.find(':'); colon != std::string::npos)
        hostOnly.resize(colon);
    std::wstring host = Widen(hostOnly), target = Widen(request.target), method = Widen(request.method);
    WORD port = tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    bool secure = tls;
    if (WebBaseSet())
    {
        host = g_webBase.host;
        port = g_webBase.port;
        secure = g_webBase.secure;
        target = g_webBase.directory + Leaf(target.c_str());
    }

    HINTERNET session = WinHttpOpen(L"aispace", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 120000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    HINTERNET handle = connection ? WinHttpOpenRequest(connection, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (handle)
    {
        if (g_insecure)
        {
            DWORD ignore = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(handle, WINHTTP_OPTION_SECURITY_FLAGS, &ignore, sizeof(ignore));
        }
        std::wstring headers;
        if (!request.contentType.empty())
            headers = L"Content-Type: " + Widen(request.contentType) + L"\r\n";
        ok = WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
             && WinHttpReceiveResponse(handle, nullptr);
        if (ok)
        {
            DWORD status = 0, size = sizeof(status);
            if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
                response.status = status;
            std::wstring text = QueryHeader(handle, WINHTTP_QUERY_STATUS_TEXT);
            if (!text.empty())
                response.statusText = Narrow(text);
            std::wstring type = QueryHeader(handle, WINHTTP_QUERY_CONTENT_TYPE);
            if (!type.empty())
                response.contentType = Narrow(type);
            response.body.clear();
            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(handle, &available) || available == 0)
                    break;
                size_t offset = response.body.size();
                response.body.resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(handle, &response.body[offset], available, &read))
                {
                    ok = false;
                    break;
                }
                response.body.resize(offset + read);
                if (response.body.size() > kMaxBodyBytes)
                    break;
            }
        }
        else
            LogN(L"aisp.hook: https: forward failed, WinHTTP error %u", GetLastError());
    }
    else
        LogN(L"aisp.hook: https: could not open the request, WinHTTP error %u", GetLastError());
    if (handle)
        WinHttpCloseHandle(handle);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}


// --- the uploader's VCE stream: upload.php -----------------------------------------------------
//
// vce::Session (the uploader derives from it): +0x0 vtable, +0x4 the ivce stream, +0xc the
// pending connector, +0x20 the state (1 connected, 2 closed). The vtable slots the hook uses:
//   5  onClose(int reason)             the uploader records an error if it has no result yet
//   6  onConnect()                     the base sets state 1
//   8  onRecv(const char*, int) -> int the uploader parses the reply and finishes the upload
// ivce::iTcpStream, as far as vce::Session calls it on the uploader's behalf:
//   2  state() -> int                  3 or 4 is connected, what the upload job waits for
//   7  shutdown()                      before a new connect, on the previous stream
//   8  close()
//   12 write(const void*, int) -> bool  vce::Session::send
// Nothing else reaches the stream: VCE never sees this object, and the uploader uses none of
// the Session's other pass-throughs. The remaining slots answer 0.
constexpr uintptr_t kVceVtable = 0x9a700c;                                       // ivce::iVCE, the core at ds:0xa5edd8; slot 6 = connect
constexpr uintptr_t kVceConnect = 0x8addd0;                                      // what that slot holds in this client build
constexpr uintptr_t kUploaderVtables[] = {0x94379c, 0x94355c, 0x9435c4};          // CUCCUploaderBase, CDramaUploader, CAITuneUploader
constexpr size_t kStreamSlots = 30;

using VceConnect_t = int(__thiscall*)(void* self, void* session, const char* host, int port, int timeout, int flags);
using SessionOnClose_t = void(__thiscall*)(void* self, int reason);
using SessionOnConnect_t = void(__thiscall*)(void* self);
using SessionOnRecv_t = int(__thiscall*)(void* self, const char* data, int length);

VceConnect_t g_originalVceConnect = nullptr;

struct UploadStream
{
    void** vtable = nullptr;
    BYTE* session = nullptr;   // the uploader, a vce::Session
    std::string host;          // from connect, without a port the client never passes anyway
    std::string request;       // what the client's http::HTTP wrote
    std::string reply;         // the HTTP/1.1 text to hand back
    volatile LONG state = 0;   // 0 collecting the request, 1 request in flight, 2 reply ready, 3 done or closed
    DWORD connectThread = 0;   // the thread that asked for the connection
    ULONGLONG readyAt = 0;
    bool logged = false;       // the delivery line, once
    UploadStream* next = nullptr;
};

CRITICAL_SECTION g_uploadLock;
bool g_uploadLockReady = false;
UploadStream* g_uploads = nullptr; // every stream handed to a session, newest first

void** SessionVtable(BYTE* session) { return *reinterpret_cast<void***>(session); }

bool IsUploaderSession(void* session)
{
    if (!session)
        return false;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(SessionVtable(static_cast<BYTE*>(session)));
    for (uintptr_t known : kUploaderVtables)
        if (vtable == known)
            return true;
    return false;
}

// The request is complete once its head and Content-Length bytes are in (or the head alone
// for a request without a body).
bool RequestComplete(const std::string& request)
{
    const size_t headEnd = request.find("\r\n\r\n");
    if (headEnd == std::string::npos)
        return request.size() >= kMaxHeaderBytes;
    Request parsed;
    if (!ParseRequest(request.substr(0, headEnd + 2), parsed))
        return true; // unparseable: replay what there is, the reply says 400
    if (!parsed.hasContentLength)
        return true;
    return request.size() >= headEnd + 4 + parsed.contentLength;
}

std::string ResponseText(const Response& response)
{
    char head[1024] = {};
    StringCchPrintfA(head, sizeof(head), "HTTP/1.1 %lu %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                     static_cast<unsigned long>(response.status), response.statusText.c_str(), response.contentType.c_str(), static_cast<unsigned long>(response.body.size()));
    return std::string(head) + response.body;
}

DWORD WINAPI UploadWorker(LPVOID parameter)
{
    UploadStream* stream = static_cast<UploadStream*>(parameter);
    Request request;
    Response response;
    std::string body;
    const size_t headEnd = stream->request.find("\r\n\r\n");
    bool parsed = headEnd != std::string::npos && ParseRequest(stream->request.substr(0, headEnd + 2), request);
    if (parsed)
    {
        if (request.host.empty())
            request.host = stream->host;
        body = stream->request.substr(headEnd + 4, request.hasContentLength ? request.contentLength : std::string::npos);
        if (request.hasContentLength && request.contentLength > kMaxBodyBytes)
            parsed = false;
    }
    if (parsed)
    {
        if (WebBaseSet())
            LogF(L"aisp.hook: https: upload.php: sending to the [web] base, %s", (g_webBase.directory + Leaf(Widen(request.target).c_str())).c_str());
        else
            LogF(g_uploadTls ? L"aisp.hook: https: upload.php: sending to https://%s" : L"aisp.hook: https: upload.php: sending to http://%s", Widen(request.host + request.target).c_str());
        if (!Forward(request, body, g_uploadTls, response))
            response.body = g_uploadTls
                ? "<result status=\"fail\"><error><code>502</code><description>aisp.hook could not reach the server over HTTPS</description></error></result>"
                : "<result status=\"fail\"><error><code>502</code><description>aisp.hook could not reach the server</description></error></result>";
        LogN(L"aisp.hook: https: upload.php: server answered %u", response.status);
    }
    else
    {
        Log(L"aisp.hook: https: upload.php: could not parse the client's request");
        response.status = 400;
        response.statusText = "Bad Request";
        response.body = "<result status=\"fail\"><error><code>400</code><description>aisp.hook could not parse the request</description></error></result>";
    }
    stream->reply = ResponseText(response);
    stream->readyAt = GetTickCount64();
    InterlockedExchange(&stream->state, 2);
    LogN(L"aisp.hook: https: upload.php: reply ready, %u bytes, waiting for the game to poll", static_cast<unsigned>(stream->reply.size()));
    return 0;
}

void DeliverUploadReplies(const wchar_t* from);

// --- the stream object's slots (thiscall: this in ecx, the callee pops its arguments) --------

void __thiscall Stream_Destroy(UploadStream*, int) {}
int __thiscall Stream_Nothing(UploadStream*) { return 0; }

int __thiscall Stream_State(UploadStream* self)
{
    // The client polls this while it waits; a ready reply goes to it right here.
    if (self->state == 2)
        DeliverUploadReplies(L"state poll");
    return self->state < 3 ? 3 : 0;
}

void __thiscall Stream_Close(UploadStream* self)
{
    InterlockedExchange(&self->state, 3);
}

bool __thiscall Stream_Write(UploadStream* self, const void* data, int length)
{
    if (self->state != 0 || !data || length <= 0)
        return false;
    self->request.append(static_cast<const char*>(data), static_cast<size_t>(length));
    if (RequestComplete(self->request))
    {
        InterlockedExchange(&self->state, 1);
        wchar_t line[200] = {};
        StringCchPrintfW(line, 200, L"aisp.hook: https: upload.php: request complete, %u bytes, written on thread %u", static_cast<unsigned>(self->request.size()), GetCurrentThreadId());
        Log(line);
        HANDLE thread = CreateThread(nullptr, 0, UploadWorker, self, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
        {
            Log(L"aisp.hook: https: upload.php: worker thread could not start");
            InterlockedExchange(&self->state, 3);
            return false;
        }
    }
    return true;
}

void** StreamVtable()
{
    static void* slots[kStreamSlots] = {};
    if (!slots[2])
    {
        for (size_t i = 0; i < kStreamSlots; ++i)
            slots[i] = reinterpret_cast<void*>(Stream_Nothing);
        slots[0] = reinterpret_cast<void*>(Stream_Destroy);
        slots[2] = reinterpret_cast<void*>(Stream_State);
        slots[7] = reinterpret_cast<void*>(Stream_Close);
        slots[8] = reinterpret_cast<void*>(Stream_Close);
        slots[12] = reinterpret_cast<void*>(Stream_Write);
    }
    return slots;
}

// Ready replies go to their session the way VCE's poll would have delivered them: on the
// thread that asked for the connection, from the client's own state poll or its message pump.
// A reply nobody picked up within a second goes to whichever thread pumps messages.
void DeliverUploadReplies(const wchar_t* from)
{
    if (!g_uploadLockReady)
        return;
    const DWORD thread = GetCurrentThreadId();
    for (;;)
    {
        UploadStream* ready = nullptr;
        EnterCriticalSection(&g_uploadLock);
        for (UploadStream* stream = g_uploads; stream; stream = stream->next)
        {
            if (stream->state != 2)
                continue;
            if (stream->connectThread == thread || GetTickCount64() - stream->readyAt > 1000)
            {
                ready = stream;
                break;
            }
        }
        if (ready)
            InterlockedExchange(&ready->state, 3);
        LeaveCriticalSection(&g_uploadLock);
        if (!ready)
            return;
        BYTE* session = ready->session;
        if (*reinterpret_cast<void**>(session + 0x4) != ready)
        {
            Log(L"aisp.hook: https: upload.php: the session moved on before the reply arrived");
            continue;
        }
        wchar_t line[256] = {};
        StringCchPrintfW(line, 256, L"aisp.hook: https: upload.php: reply delivered from the %s on thread %u%s", from, thread, ready->connectThread == thread ? L"" : L" (not the one that connected)");
        Log(line);
        void** vtable = SessionVtable(session);
        auto onRecv = reinterpret_cast<SessionOnRecv_t>(vtable[8]);
        auto onClose = reinterpret_cast<SessionOnClose_t>(vtable[5]);
        onRecv(session, ready->reply.data(), static_cast<int>(ready->reply.size()));
        onClose(session, 0);
    }
}

BOOL WINAPI HookPeekMessageW(LPMSG message, HWND window, UINT filterMin, UINT filterMax, UINT remove)
{
    DeliverUploadReplies(L"message pump");
    return g_originalPeekMessageW(message, window, filterMin, filterMax, remove);
}

// ivce::iVCE::connect(session, host, port, timeout, flags), what the upload job calls on the
// global VCE core. For the uploader on port 80 the session gets the hook's stream and is told it
// is connected; every other session (the game servers) is the client's.
int __thiscall HookVceConnect(void* self, void* sessionPointer, const char* host, int port, int timeout, int flags)
{
    if (port != 80 || !host || !IsUploaderSession(sessionPointer))
        return g_originalVceConnect(self, sessionPointer, host, port, timeout, flags);
    BYTE* session = static_cast<BYTE*>(sessionPointer);
    UploadStream* stream = new UploadStream();
    stream->vtable = StreamVtable();
    stream->session = session;
    stream->host = host;
    if (size_t colon = stream->host.find(':'); colon != std::string::npos)
        stream->host.resize(colon);
    stream->connectThread = GetCurrentThreadId();
    EnterCriticalSection(&g_uploadLock);
    // This session's previous stream, if it was ours and is done with, is not needed any more.
    for (UploadStream** link = &g_uploads; *link;)
    {
        UploadStream* old = *link;
        if (old->session == session && old->state == 3 && *reinterpret_cast<void**>(session + 0x4) == old)
        {
            *link = old->next;
            delete old;
        }
        else
            link = &old->next;
    }
    stream->next = g_uploads;
    g_uploads = stream;
    LeaveCriticalSection(&g_uploadLock);
    *reinterpret_cast<void**>(session + 0x4) = stream;
    *reinterpret_cast<void**>(session + 0xc) = nullptr; // no connector to pump
    reinterpret_cast<SessionOnConnect_t>(SessionVtable(session)[6])(session);
    wchar_t line[600] = {};
    StringCchPrintfW(line, 600, L"aisp.hook: https: upload.php: the client's connection to %s is answered by the hook (thread %u)", Widen(stream->host).c_str(), stream->connectThread);
    Log(line);
    return 1;
}

bool PatchVtableSlot(uintptr_t vtable, size_t slot, void* replacement, void** original)
{
    void** entry = reinterpret_cast<void**>(vtable) + slot;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    if (original)
        *original = *entry;
    *entry = replacement;
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
    return true;
}

bool PatchUpload()
{
    if (!g_uploadLockReady)
    {
        InitializeCriticalSection(&g_uploadLock);
        g_uploadLockReady = true;
    }
    // The addresses are this client build's: the slot must hold the connect we expect there,
    // or another build is running and gets no upload hook.
    if (IsBadReadPtr(reinterpret_cast<void*>(kVceVtable), 7 * sizeof(void*)) || reinterpret_cast<uintptr_t>(*(reinterpret_cast<void**>(kVceVtable) + 6)) != kVceConnect)
        return false;
    HMODULE game = GetModuleHandleW(nullptr);
    if (!PatchImport(game, "USER32.dll", "PeekMessageW", 0, reinterpret_cast<void*>(HookPeekMessageW), &g_originalPeekMessageW))
        return false;
    return PatchVtableSlot(kVceVtable, 6, reinterpret_cast<void*>(HookVceConnect), reinterpret_cast<void**>(&g_originalVceConnect));
}

// One line per endpoint in the log: what was decided, why, and where the requests go.
void LogEndpoint(const wchar_t* name, char hostLine, char pathLine, bool tls, const wchar_t* file)
{
    wchar_t host[512] = {}, path[512] = {};
    if (!ReadConnectionValue(hostLine, host, 512))
        StringCchCopyW(host, 512, L"?");
    if (!ReadConnectionValue(pathLine, path, 512))
        StringCchCopyW(path, 512, L"?");
    while (path[0] == L'/')
        std::wmemmove(path, path + 1, std::wcslen(path));
    wchar_t list[1024] = {};
    if (!ConfigString(L"AISP_HTTPS_HOSTS", L"web", L"https_hosts", list, 1024))
        StringCchCopyW(list, 1024, L"aisp.moe,example.com,nbspou.net");
    const bool automatic = g_switch == Switch::Auto && !WebBaseSet();
    const wchar_t* why = WebBaseSet() ? L"[web] base" : g_switch == Switch::On ? L"[web] https=1" : g_switch == Switch::Off ? L"[web] https=0" : (IsHttpsHost(host) ? L"host in https_hosts" : L"host not in https_hosts");
    wchar_t target[1024] = {};
    if (WebBaseSet())
        StringCchPrintfW(target, 1024, L"%s://%s:%u%s%s", g_webBase.secure ? L"https" : L"http", g_webBase.host.c_str(), g_webBase.port, g_webBase.directory.c_str(), file);
    else
        StringCchPrintfW(target, 1024, L"%s://%s/%s", tls ? L"https" : L"http", host, path);
    wchar_t line[2048] = {};
    const bool hooked = tls || (std::wcscmp(name, L"upload.php") == 0 && g_uploadHook);
    StringCchPrintfW(line, 2048, L"aisp.hook: https: %s: %s (%s%s%s) -> %s", name, tls ? L"TLS" : hooked ? L"plain HTTP through the hook" : L"plain HTTP, as the client does it", why, automatic ? L", https_hosts=" : L"", automatic ? list : L"", target);
    Log(line);
}
} // namespace

bool HttpsEnabled()
{
    ReadFlags();
    return g_downloadTls || g_uploadTls;
}

bool ScreensHttpsEnabled(const wchar_t* host)
{
    switch (ConfigSwitch(L"AISP_SCREEN_HTTPS", L"screens", L"https"))
    {
    case Switch::On: return true;
    case Switch::Off: return false;
    case Switch::Auto: break;
    }
    return host && IsHttpsHost(host);
}

void PatchHttps()
{
    ReadFlags();
    LogEndpoint(L"download.php", '4', '5', g_downloadTls, L"download.php");
    LogEndpoint(L"upload.php", '6', '7', g_uploadTls, L"upload.php");
    if (g_downloadTls)
    {
        HMODULE game = GetModuleHandleW(nullptr);
        bool connectPatched = PatchImport(game, "WININET.dll", "InternetConnectW", 0, reinterpret_cast<void*>(HookInternetConnectW), &g_originalInternetConnectW);
        bool openPatched = PatchImport(game, "WININET.dll", "HttpOpenRequestW", 0, reinterpret_cast<void*>(HookHttpOpenRequestW), &g_originalHttpOpenRequestW);
        bool optionPatched = PatchImport(game, "WININET.dll", "InternetSetOptionW", 0, reinterpret_cast<void*>(HookInternetSetOptionW), &g_originalInternetSetOptionW);
        if (!connectPatched || !openPatched || !optionPatched)
            Log(L"aisp.hook: https: WinINet imports not all found; download.php stays as the client does it");
    }
    if ((g_uploadTls || g_uploadHook) && !PatchUpload())
        Log(L"aisp.hook: https: the VCE connect slot or PeekMessageW was not where this client build has them; upload.php stays as the client does it");
    if ((g_downloadTls || g_uploadTls) && g_insecure)
        Log(L"aisp.hook: https: [web] insecure=1, certificates are not checked");
}
} // namespace aisp
