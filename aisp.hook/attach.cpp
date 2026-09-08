// 32-bit helper: start the game suspended, attach aisp.hook.dll from this directory to it,
// resume (or, with --into-running, attach to the game already running). Wine's loader
// deadlocks if that DLL does real work in DllMain; the hook defers its patches, so LoadLibraryW
// here is expected to return. Used by the local Wine runner when the Avalonia launcher is not
// the process creating the game; on Windows the launcher loads the DLL itself.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstring>
#include <cwchar>

namespace
{
bool BuildHookPath(wchar_t* out, size_t count)
{
    wchar_t self[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    wchar_t* slash = std::wcsrchr(self, L'\\');
    if (!slash)
        return false;
    slash[1] = 0;
    if (wcslen(self) + 13 >= count)
        return false;
    wcscpy(out, self);
    wcscat(out, L"aisp.hook.dll");
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

bool AttachDll(HANDLE process, const wchar_t* dllPath)
{
    const SIZE_T bytes = (std::wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return false;
    bool ok = false;
    if (WriteProcessMemory(process, remote, dllPath, bytes, nullptr))
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto loadLibraryW = kernel ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW")) : nullptr;
        if (loadLibraryW)
        {
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibraryW, remote, 0, nullptr);
            if (thread)
            {
                WaitForSingleObject(thread, 15000);
                DWORD code = 0;
                GetExitCodeThread(thread, &code);
                CloseHandle(thread);
                ok = code != 0 && code != STILL_ACTIVE;
            }
        }
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}
} // namespace

DWORD FindGamePid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"ai sp@ce.exe") == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

int AttachToPid(DWORD pid, const wchar_t* hook)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process)
        return 6;
    const bool ok = AttachDll(process, hook);
    CloseHandle(process);
    return ok ? 0 : 4;
}

int main()
{
    wchar_t hook[MAX_PATH] = {};
    if (!BuildHookPath(hook, MAX_PATH))
        return 2;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && _wcsicmp(argv[1], L"--into-running") == 0)
    {
        if (argv)
            LocalFree(argv);
        const DWORD pid = FindGamePid();
        if (!pid)
            return 7;
        return AttachToPid(pid, hook);
    }
    wchar_t command[4096] = {};
    if (argc >= 2)
    {
        command[0] = 0;
        for (int i = 1; i < argc; ++i)
        {
            if (command[0])
                wcscat(command, L" ");
            const bool quote = std::wcschr(argv[i], L' ') != nullptr;
            if (quote)
                wcscat(command, L"\"");
            wcscat(command, argv[i]);
            if (quote)
                wcscat(command, L"\"");
        }
    }
    else
    {
        wcscpy(command, L"\"ai sp@ce.exe\" ./data");
    }
    if (argv)
        LocalFree(argv);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &info))
        return 3;
    if (!AttachDll(info.hProcess, hook))
    {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return 4;
    }
    if (ResumeThread(info.hThread) == static_cast<DWORD>(-1))
    {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return 5;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return 0;
}
