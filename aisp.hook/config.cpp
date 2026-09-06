// aisp.hook.ini, see config.h.
#include <windows.h>
#include <strsafe.h>

#include <cwchar>

#include "config.h"

namespace aisp
{
bool BuildGameFilePath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount); // screen.cpp

namespace
{
// The profile API looks in the Windows directory for a bare file name, so the ini is always
// addressed by its full path next to the game executable.
const wchar_t* IniPath()
{
    static wchar_t path[MAX_PATH] = {};
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        if (!BuildGameFilePath(L"aisp.hook.ini", path, MAX_PATH))
            path[0] = L'\0';
    }
    return path;
}
} // namespace

bool ConfigString(const wchar_t* variable, const wchar_t* section, const wchar_t* key, wchar_t* out, size_t outCount)
{
    if (outCount == 0)
        return false;
    out[0] = L'\0';
    if (variable && GetEnvironmentVariableW(variable, out, static_cast<DWORD>(outCount)) > 0 && out[0])
        return true;
    out[0] = L'\0';
    const wchar_t* path = IniPath();
    if (!path[0])
        return false;
    GetPrivateProfileStringW(section, key, L"", out, static_cast<DWORD>(outCount), path);
    // Trailing spaces are the profile API's to keep; nobody means them.
    size_t length = std::wcslen(out);
    while (length > 0 && (out[length - 1] == L' ' || out[length - 1] == L'\t'))
        out[--length] = L'\0';
    return out[0] != L'\0';
}

Switch ConfigSwitch(const wchar_t* variable, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[16] = {};
    if (!ConfigString(variable, section, key, value, 16))
        return Switch::Auto;
    if (_wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"on") == 0 || _wcsicmp(value, L"yes") == 0 || _wcsicmp(value, L"true") == 0)
        return Switch::On;
    if (_wcsicmp(value, L"0") == 0 || _wcsicmp(value, L"off") == 0 || _wcsicmp(value, L"no") == 0 || _wcsicmp(value, L"false") == 0)
        return Switch::Off;
    return Switch::Auto;
}
} // namespace aisp
