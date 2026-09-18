using System.Runtime.InteropServices;

namespace aisp.launch;

internal static class WineDetection
{
    public static bool IsRunningOnWine { get; } = Detect();

    private static bool Detect()
    {
        if (!OperatingSystem.IsWindows())
            return false;

        var ntdll = GetModuleHandleW("ntdll.dll");
        return ntdll != IntPtr.Zero && GetProcAddress(ntdll, "wine_get_version") != IntPtr.Zero;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string lpModuleName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);
}
