using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace aisp.launch;

public static partial class HostsFile
{
    public static readonly string[] Hostnames = ["aisp.jp", "live.nicovideo.jp"];

    public const string Marker = "# aisp.launch";

    private static readonly (string Address, string Hostname)[] RequiredEntries =
    [
        ("127.0.0.1", "aisp.jp"),
        ("::1", "aisp.jp"),
        ("127.0.0.1", "live.nicovideo.jp"),
        ("::1", "live.nicovideo.jp"),
    ];

    public static string GetPath()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.System),
                "drivers",
                "etc",
                "hosts"
            );
        }

        if (IsWsl())
        {
            const string windowsHosts = "/mnt/c/Windows/System32/drivers/etc/hosts";
            if (File.Exists(windowsHosts))
                return windowsHosts;
        }

        return "/etc/hosts";
    }

    public static HostsFileResult TryApply()
    {
        try
        {
            var path = GetPath();
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];

            if (!ApplyChanges(lines))
                return HostsFileResult.Success("Hosts file already configured for Nico TV proxy hosts.");

            File.WriteAllLines(path, lines);
            return HostsFileResult.Success(
                "Added aisp.jp and live.nicovideo.jp redirects to the hosts file."
            );
        }
        catch (UnauthorizedAccessException)
        {
            return HostsFileResult.Failure(
                "Could not write to the hosts file.",
                "Run the launcher as administrator (Windows) or with sudo (Linux) once to add the Nico TV proxy hosts entries."
            );
        }
        catch (Exception ex)
        {
            return HostsFileResult.Failure("Could not update the hosts file.", ex.Message);
        }
    }

    private static bool ApplyChanges(List<string> lines)
    {
        var foundEntries = new HashSet<string>(StringComparer.Ordinal);
        var changed = false;

        for (var i = lines.Count - 1; i >= 0; i--)
        {
            if (!TryParseManagedHostname(lines[i], out var address, out var hostname))
                continue;

            var entryKey = $"{address}|{hostname}";
            if (IsLoopbackAddress(address))
            {
                foundEntries.Add(entryKey);
                var expectedLine = FormatEntry(address, hostname);
                if (!string.Equals(lines[i], expectedLine, StringComparison.Ordinal))
                {
                    lines[i] = expectedLine;
                    changed = true;
                }

                continue;
            }

            lines.RemoveAt(i);
            changed = true;
        }

        foreach (var (address, hostname) in RequiredEntries)
        {
            var entryKey = $"{address}|{hostname}";
            if (foundEntries.Contains(entryKey))
                continue;

            lines.Add(FormatEntry(address, hostname));
            changed = true;
        }

        return changed;
    }

    private static string FormatEntry(string address, string hostname) =>
        $"{address} {hostname} {Marker}";

    private static bool TryParseManagedHostname(
        string line,
        out string address,
        out string hostname
    )
    {
        address = string.Empty;
        hostname = string.Empty;

        var trimmed = line.Trim();
        if (trimmed.Length == 0 || trimmed.StartsWith('#'))
            return false;

        var commentIndex = trimmed.IndexOf('#');
        if (commentIndex >= 0)
            trimmed = trimmed[..commentIndex].Trim();

        var parts = trimmed.Split([' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 2)
            return false;

        address = parts[0];
        foreach (var candidate in parts.Skip(1))
        {
            foreach (var managedHostname in Hostnames)
            {
                if (HostnameToken(managedHostname).IsMatch(candidate))
                {
                    hostname = managedHostname;
                    return true;
                }
            }
        }

        return false;
    }

    private static bool IsLoopbackAddress(string address) =>
        address is "127.0.0.1" or "::1";

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

    [GeneratedRegex(@"^aisp\.jp$", RegexOptions.IgnoreCase)]
    private static partial Regex AispHostnameToken();

    [GeneratedRegex(@"^live\.nicovideo\.jp$", RegexOptions.IgnoreCase)]
    private static partial Regex LiveNicoHostnameToken();

    private static Regex HostnameToken(string hostname) =>
        hostname switch
        {
            "aisp.jp" => AispHostnameToken(),
            "live.nicovideo.jp" => LiveNicoHostnameToken(),
            _ => throw new ArgumentOutOfRangeException(nameof(hostname)),
        };
}

public readonly record struct HostsFileResult(
    bool Succeeded,
    string Message,
    string? Details = null
)
{
    public static HostsFileResult Success(string message) => new(true, message);

    public static HostsFileResult Failure(string message, string? details = null) =>
        new(false, message, details);
}
