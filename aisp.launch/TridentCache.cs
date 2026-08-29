using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace aisp.launch;

internal static class TridentCache
{
    public const string LegacyPlayerUrl =
        "http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php";

    private static readonly int[] ScreenIds = [0, 1, 2, 3, 4, 5, 9];

    public static void TryClearLegacyPlayerCache()
    {
        var urls = LegacyPlayerCacheUrls();
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            ClearOnWindows(urls);
            return;
        }

        if (IsWsl())
            ClearOnWsl(urls);
    }

    private static IEnumerable<string> LegacyPlayerCacheUrls()
    {
        for (var channelId = 0; channelId < 16; channelId++)
        {
            foreach (var screenId in ScreenIds)
            {
                yield return $"{LegacyPlayerUrl}?tvid={screenId}&chid={channelId}";
            }
        }
    }

    [SupportedOSPlatform("windows")]
    private static void ClearOnWindows(IEnumerable<string> urls)
    {
        var removed = urls.Count(url => DeleteUrlCacheEntry(url));
        Trace.WriteLine($"Removed {removed} cached ai sp@ce Trident player URL(s).");
    }

    [SupportedOSPlatform("windows")]
    [DllImport("wininet.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool DeleteUrlCacheEntry(string url);

    private static void ClearOnWsl(IEnumerable<string> urls)
    {
        var powershell = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
        if (!File.Exists(powershell))
        {
            Trace.WriteLine("Could not find Windows PowerShell to clear the Trident cache.");
            return;
        }

        var quotedUrls = string.Join(",", urls.Select(url => $"'{url}'"));
        var command =
            "Add-Type -Namespace AispLaunch -Name WinInet -MemberDefinition "
            + "'[System.Runtime.InteropServices.DllImport(\"wininet.dll\", "
            + "CharSet=System.Runtime.InteropServices.CharSet.Unicode, SetLastError=true)] "
            + "public static extern bool DeleteUrlCacheEntry(string url);';"
            + $"$urls=@({quotedUrls});"
            + "$removed=0;"
            + "foreach($url in $urls){"
            + "if([AispLaunch.WinInet]::DeleteUrlCacheEntry($url)){$removed++}"
            + "};"
            + "Write-Output $removed";

        try
        {
            using var process = Process.Start(
                new ProcessStartInfo
                {
                    FileName = powershell,
                    Arguments =
                        $"-NoLogo -NoProfile -NonInteractive -Command \"{command.Replace("\"", "\\\"")}\"",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true,
                }
            );

            if (process is null)
                return;

            if (!process.WaitForExit(30_000))
            {
                process.Kill(entireProcessTree: true);
                Trace.WriteLine("Timed out clearing the Windows Trident cache.");
                return;
            }

            if (process.ExitCode != 0)
            {
                var detail = process.StandardError.ReadToEnd().Trim();
                Trace.WriteLine(
                    string.IsNullOrEmpty(detail)
                        ? $"Could not clear the Windows Trident cache (exit code {process.ExitCode})."
                        : $"Could not clear the Windows Trident cache: {detail}"
                );
                return;
            }

            var removed = process.StandardOutput.ReadToEnd().Trim();
            Trace.WriteLine($"Removed {removed} cached ai sp@ce Trident player URL(s).");
        }
        catch (Exception ex)
        {
            Trace.WriteLine($"Could not clear the Windows Trident cache: {ex.Message}");
        }
    }

    private static bool IsWsl()
    {
        try
        {
            if (File.Exists("/proc/sys/kernel/osrelease"))
            {
                var release = File.ReadAllText("/proc/sys/kernel/osrelease");
                return release.Contains("microsoft", StringComparison.OrdinalIgnoreCase);
            }
        }
        catch
        {
            // Ignore and treat as non-WSL.
        }

        return false;
    }
}
