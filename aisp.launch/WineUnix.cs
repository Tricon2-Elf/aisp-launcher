using System.Diagnostics;
using System.Runtime.InteropServices;

namespace aisp.launch;

internal static class WineUnix
{
    public static string GetUnixPath(string windowsPath)
    {
        var full = Path.GetFullPath(windowsPath);
        if (TryWineGetUnixFileName(full, out var mapped) && mapped.Length > 0)
            return mapped;

        if (full.Length >= 3 && full[1] == ':')
        {
            var slash = full[2..].Replace('\\', '/');
            if (char.ToUpperInvariant(full[0]) == 'Z')
                return slash.Length == 0 ? "/" : slash;

            var prefix = Environment.GetEnvironmentVariable("WINEPREFIX");
            if (string.IsNullOrEmpty(prefix))
            {
                var unixHome = Environment.GetEnvironmentVariable("HOME");
                if (!string.IsNullOrEmpty(unixHome))
                    prefix = Path.Combine(unixHome, ".wine").Replace('\\', '/');
            }

            if (!string.IsNullOrEmpty(prefix))
            {
                var drive = char.ToLowerInvariant(full[0]);
                return $"{prefix.Replace('\\', '/')}/dosdevices/{drive}:{slash}";
            }
        }

        throw new InvalidOperationException(
            $"Could not map '{full}' to a Unix path for the native Electron broker."
        );
    }

    public static void RunWait(string unixProgram, params string[] unixArgs) =>
        Start(wait: true, unixProgram, unixArgs);

    public static void RunDetached(string unixProgram, params string[] unixArgs) =>
        Start(wait: false, unixProgram, unixArgs);

    private static void Start(bool wait, string unixProgram, string[] unixArgs)
    {
        var startExe = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System),
            "start.exe"
        );
        if (!File.Exists(startExe))
            startExe = @"C:\windows\system32\start.exe";
        if (!File.Exists(startExe))
        {
            throw new InvalidOperationException(
                "Wine start.exe was not found; cannot launch the native Electron broker."
            );
        }

        var arguments = wait ? "/wait /unix " : "/unix ";
        arguments += Quote(unixProgram);
        foreach (var arg in unixArgs)
            arguments += " " + Quote(arg);

        var info = new ProcessStartInfo
        {
            FileName = startExe,
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        using var process =
            Process.Start(info)
            ?? throw new InvalidOperationException(
                $"Failed to start Wine unix process: {unixProgram}"
            );
        if (!wait)
            return;
        if (!process.WaitForExit(30_000))
        {
            throw new TimeoutException($"Wine unix process timed out: {unixProgram}");
        }
    }

    private static string Quote(string value) =>
        value.IndexOfAny([' ', '\t', '"']) >= 0
            ? "\"" + value.Replace("\"", "\\\"") + "\""
            : value;

    private static bool TryWineGetUnixFileName(string windowsPath, out string unixPath)
    {
        unixPath = "";
        try
        {
            var ntdll = GetModuleHandleW("ntdll.dll");
            if (ntdll == IntPtr.Zero)
                return false;
            var proc = GetProcAddress(ntdll, "wine_get_unix_file_name");
            if (proc == IntPtr.Zero)
                return false;
            var fn = Marshal.GetDelegateForFunctionPointer<WineGetUnixFileName>(proc);
            var ptr = fn(windowsPath);
            if (ptr == IntPtr.Zero)
                return false;
            unixPath = Marshal.PtrToStringAnsi(ptr) ?? "";
            return unixPath.Length > 0;
        }
        catch
        {
            return false;
        }
    }

    private delegate IntPtr WineGetUnixFileName(
        [MarshalAs(UnmanagedType.LPWStr)] string dosPath
    );

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string lpModuleName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);
}
