using System.Diagnostics;

namespace aisp.launch;

internal static class TempCleanup
{
    public const string SessionDirectoryPrefix = "aisp-nicotv-twitch-";
    public const string DownloadDirectoryPrefix = ".download-";

    private static readonly string[] StrayExtensions =
    [
        ".ts",
        ".m4s",
        ".m3u8",
        ".tmp",
    ];

    public static string CreateSessionDirectory()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            SessionDirectoryPrefix + Guid.NewGuid().ToString("N")
        );
        Directory.CreateDirectory(path);
        return path;
    }

    public static void Run()
    {
        TryDeleteMatchingDirectories(Path.GetTempPath(), SessionDirectoryPrefix);
        TryDeleteMatchingDirectories(
            MediaToolsResolver.GetCacheDirectory(LauncherBootstrap.Settings.ToolsDirectory),
            DownloadDirectoryPrefix
        );
        SweepStrayMedia(AppContext.BaseDirectory, recursive: false);
        SweepStrayMedia(
            MediaToolsResolver.GetCacheDirectory(LauncherBootstrap.Settings.ToolsDirectory),
            recursive: true
        );
    }

    public static void TryDeleteDirectory(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || !Directory.Exists(path))
            return;

        try
        {
            Directory.Delete(path, recursive: true);
        }
        catch (Exception ex)
        {
            Trace.WriteLine($"Could not delete temp directory {path}: {ex.Message}");
            TryDeleteFilesInDirectory(path);
        }
    }

    private static void TryDeleteMatchingDirectories(string parent, string prefix)
    {
        if (!Directory.Exists(parent))
            return;

        try
        {
            foreach (var directory in Directory.EnumerateDirectories(parent, prefix + "*"))
                TryDeleteDirectory(directory);
        }
        catch (Exception ex)
        {
            Trace.WriteLine($"Could not scan {parent} for {prefix}*: {ex.Message}");
        }
    }

    private static void SweepStrayMedia(string directory, bool recursive)
    {
        if (!Directory.Exists(directory))
            return;

        var option = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;

        try
        {
            foreach (var file in Directory.EnumerateFiles(directory, "*", option))
            {
                if (!IsStrayMediaFile(file))
                    continue;

                TryDeleteFile(file);
            }
        }
        catch (Exception ex)
        {
            Trace.WriteLine($"Could not sweep stray media in {directory}: {ex.Message}");
        }
    }

    private static bool IsStrayMediaFile(string path)
    {
        var name = Path.GetFileName(path);
        var extension = Path.GetExtension(path);

        if (string.Equals(name, "input.mp4", StringComparison.OrdinalIgnoreCase))
            return false;

        if (StrayExtensions.Any(ext => extension.Equals(ext, StringComparison.OrdinalIgnoreCase)))
            return true;

        if (
            name.StartsWith("segment-", StringComparison.OrdinalIgnoreCase)
            && (
                extension.Equals(".mp4", StringComparison.OrdinalIgnoreCase)
                || name.Contains(".tmp", StringComparison.OrdinalIgnoreCase)
            )
        )
        {
            return true;
        }

        return name.Equals("stream.m3u8", StringComparison.OrdinalIgnoreCase)
            || name.Equals("init.mp4", StringComparison.OrdinalIgnoreCase);
    }

    private static void TryDeleteFilesInDirectory(string directory)
    {
        try
        {
            foreach (var file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
                TryDeleteFile(file);
        }
        catch
        {
            // Best-effort cleanup of a locked directory.
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            File.SetAttributes(path, FileAttributes.Normal);
            File.Delete(path);
        }
        catch
        {
            // File may still be locked by a dying process.
        }
    }
}
