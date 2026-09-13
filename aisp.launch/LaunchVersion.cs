using System.Reflection;

namespace aisp.launch;

internal static class LaunchVersion
{
    public static string Display { get; } =
        Assembly
            .GetExecutingAssembly()
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion
        ?? "dev";

    public static string WindowTitle => $"aisp.launch - {Display}";

    /// <summary>
    /// Parses CalVer <c>YYYY.MM.DD.N</c>. Returns null for non-release builds such as <c>dev</c>.
    /// </summary>
    public static Version? TryParse(string? version)
    {
        if (string.IsNullOrWhiteSpace(version))
            return null;

        var trimmed = version.Trim();
        if (trimmed.Equals("dev", StringComparison.OrdinalIgnoreCase))
            return null;

        return Version.TryParse(trimmed, out var parsed) ? parsed : null;
    }

    /// <summary>
    /// Returns true when <paramref name="remote"/> is newer than the running build.
    /// Local <c>dev</c> builds are treated as older than any release.
    /// </summary>
    public static bool IsNewerThanCurrent(string remoteVersion)
    {
        var remote = TryParse(remoteVersion);
        if (remote is null)
            return false;

        var current = TryParse(Display);
        if (current is null)
            return true;

        return remote > current;
    }
}
