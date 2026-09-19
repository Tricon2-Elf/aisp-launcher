using System.Diagnostics;

namespace aisp.launch;

internal static class DirectXDecember2006
{
    public const string DisplayName = "December 2006 DirectX";
    public const string DllName = "d3dx9_32.dll";

    // June 2010 End-User Runtime is cumulative and still ships d3dx9_32.dll.
    public const string DownloadUrl =
        "https://www.microsoft.com/en-us/download/details.aspx?id=35";

    public static bool IsInstalled(string? gameDirectory = null)
    {
        foreach (var directory in CandidateDirectories(gameDirectory))
        {
            if (File.Exists(Path.Combine(directory, DllName)))
                return true;
        }

        return false;
    }

    public static string MissingMessage =>
        WineDetection.IsRunningOnWine
            ? $"{DisplayName} is not installed. The game needs {DllName}. OK opens Microsoft's DirectX End-User Runtime page (June 2010, includes December 2006). Under Wine you can also run: winetricks d3dx9_32"
            : $"{DisplayName} is not installed. The game needs {DllName}. OK opens Microsoft's DirectX End-User Runtime page (June 2010, includes December 2006).";

    public static void OpenDownloadPage()
    {
        Process.Start(new ProcessStartInfo { FileName = DownloadUrl, UseShellExecute = true });
    }

    private static IEnumerable<string> CandidateDirectories(string? gameDirectory)
    {
        yield return Environment.SystemDirectory;

        var systemX86 = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
        if (!string.IsNullOrEmpty(systemX86))
            yield return systemX86;

        if (!string.IsNullOrEmpty(gameDirectory))
            yield return gameDirectory;
    }
}
