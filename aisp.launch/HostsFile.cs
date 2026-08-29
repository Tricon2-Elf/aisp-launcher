using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace aisp.launch;

public static partial class HostsFile
{
    public const string Hostname = "aisp.jp";
    public const string Marker = "# aisp.launch";

    private static readonly (string Address, string Line)[] RequiredEntries =
    [
        ("127.0.0.1", $"127.0.0.1 {Hostname} {Marker}"),
        ("::1", $"::1 {Hostname} {Marker}"),
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

        return "/etc/hosts";
    }

    public static HostsFileResult TryApply()
    {
        try
        {
            var path = GetPath();
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];

            if (!ApplyChanges(lines))
                return HostsFileResult.Success("Hosts file already configured for aisp.jp.");

            File.WriteAllLines(path, lines);
            return HostsFileResult.Success("Added aisp.jp redirect to the hosts file.");
        }
        catch (UnauthorizedAccessException)
        {
            return HostsFileResult.Failure(
                "Could not write to the hosts file.",
                "Run the launcher as administrator (Windows) or with sudo (Linux) once to add the aisp.jp redirect."
            );
        }
        catch (Exception ex)
        {
            return HostsFileResult.Failure("Could not update the hosts file.", ex.Message);
        }
    }

    private static bool ApplyChanges(List<string> lines)
    {
        var foundAddresses = new HashSet<string>(StringComparer.Ordinal);
        var changed = false;

        for (var i = lines.Count - 1; i >= 0; i--)
        {
            if (!TryParseHostname(lines[i], out var address))
                continue;

            if (IsLoopbackAddress(address))
            {
                foundAddresses.Add(address);
                if (!lines[i].Contains(Marker, StringComparison.Ordinal))
                {
                    lines[i] = RequiredEntries.First(entry => entry.Address == address).Line;
                    changed = true;
                }

                continue;
            }

            lines.RemoveAt(i);
            changed = true;
        }

        foreach (var (address, line) in RequiredEntries)
        {
            if (foundAddresses.Contains(address))
                continue;

            lines.Add(line);
            changed = true;
        }

        return changed;
    }

    private static bool TryParseHostname(string line, out string address)
    {
        address = string.Empty;

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
        return parts.Skip(1).Any(part => HostnameToken().IsMatch(part));
    }

    private static bool IsLoopbackAddress(string address) =>
        address is "127.0.0.1" or "::1";

    [GeneratedRegex(@"^aisp\.jp$", RegexOptions.IgnoreCase)]
    private static partial Regex HostnameToken();
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
