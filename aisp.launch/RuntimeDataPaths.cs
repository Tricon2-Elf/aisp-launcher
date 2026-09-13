using System.Runtime.InteropServices;

namespace aisp.launch;

internal static class RuntimeDataPaths
{
    public const string DataDirectoryName = "aisp.launch.data";
    public const string ElectronDirectoryName = "electron";
    public const string MediaDirectoryName = "media";

    public static string InstallDirectory =>
        Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;

    public static string DataRoot => Path.Combine(InstallDirectory, DataDirectoryName);

    public static string ElectronDirectory => Path.Combine(DataRoot, ElectronDirectoryName);

    public static string MediaPlatformDirectory =>
        Path.Combine(DataRoot, MediaDirectoryName, DetectPlatform().CacheKey);

    public static RuntimePlatform DetectPlatform()
    {
        var arch = RuntimeInformation.OSArchitecture;
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            // Streamlink Windows portable builds are x64; ARM64 Windows can run them via emulation.
            if (
                arch
                is Architecture.X64
                    or Architecture.Arm64
                    or Architecture.X86
            )
                return new RuntimePlatform("windows", "x86_64");

            throw new PlatformNotSupportedException(
                $"Automatic runtime downloads do not support Windows architecture {arch}."
            );
        }

        if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
        {
            return arch switch
            {
                Architecture.X64 => new RuntimePlatform("linux", "x86_64"),
                Architecture.Arm64 => new RuntimePlatform("linux", "aarch64"),
                _ => throw new PlatformNotSupportedException(
                    $"Automatic runtime downloads do not support Linux architecture {arch}."
                ),
            };
        }

        throw new PlatformNotSupportedException(
            "Automatic runtime downloads support Windows, WSL, and Linux only."
        );
    }
}

internal readonly record struct RuntimePlatform(string OperatingSystem, string Architecture)
{
    public string CacheKey => $"{OperatingSystem}-{Architecture}";

    public bool IsWindows =>
        OperatingSystem.Equals("windows", StringComparison.OrdinalIgnoreCase);

    public bool IsLinux => OperatingSystem.Equals("linux", StringComparison.OrdinalIgnoreCase);
}
