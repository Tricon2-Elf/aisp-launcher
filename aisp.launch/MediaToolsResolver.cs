using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal readonly record struct MediaTools(string Streamlink, string Ffmpeg);

internal static class MediaToolsResolver
{
    private const string WindowsReleaseApi =
        "https://api.github.com/repos/streamlink/windows-builds/releases/latest";
    private const string LinuxReleaseApi =
        "https://api.github.com/repos/streamlink/streamlink-appimage/releases/latest";

    private static readonly Regex Sha256DigestRegex = new(
        @"^sha256:([0-9a-fA-F]{64})$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled
    );

    public static MediaTools? TryFindExisting()
    {
        var streamlink = FindOnPath("streamlink") ?? FindCachedStreamlink();
        var ffmpeg = FindOnPath("ffmpeg") ?? FindCachedFfmpeg();
        if (streamlink is null || ffmpeg is null)
            return null;
        return new MediaTools(streamlink, ffmpeg);
    }

    public static async Task<MediaTools> EnsureInstalledAsync(
        RuntimeHttpClient http,
        IProgress<string>? status = null,
        IProgress<double>? downloadProgress = null,
        CancellationToken cancellationToken = default
    )
    {
        var existing = TryFindExisting();
        if (existing is not null)
            return existing.Value;

        var platform = RuntimeDataPaths.DetectPlatform();
        status?.Report($"Downloading Streamlink + FFmpeg for {platform.CacheKey}…");

        var apiUrl = platform.IsWindows ? WindowsReleaseApi : LinuxReleaseApi;
        var release = await http.GetReleaseAsync(apiUrl, cancellationToken).ConfigureAwait(false);
        var asset = SelectBundleAsset(release, platform);

        var workRoot = Path.Combine(
            RuntimeDataPaths.DataRoot,
            ".download-media-" + Guid.NewGuid().ToString("N")
        );
        var archivePath = Path.Combine(workRoot, asset.Name);

        try
        {
            Directory.CreateDirectory(workRoot);
            await http.DownloadToFileAsync(
                    asset.Url,
                    archivePath,
                    downloadProgress,
                    cancellationToken
                )
                .ConfigureAwait(false);

            status?.Report("Verifying Streamlink bundle…");
            var actual = await RuntimeHttpClient
                .ComputeSha256Async(archivePath, cancellationToken)
                .ConfigureAwait(false);
            if (!actual.Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    $"SHA256 mismatch for {asset.Name}. Expected {asset.Sha256}, got {actual}."
                );
            }

            status?.Report("Installing Streamlink + FFmpeg…");
            string installSource;
            if (platform.IsWindows)
            {
                var extracted = Path.Combine(workRoot, "extracted");
                ExtractZipSafely(archivePath, extracted);
                var streamlink = FindWindowsExecutable(extracted, "streamlink.exe", "bin")
                    ?? throw new InvalidOperationException(
                        "Streamlink archive did not contain bin/streamlink.exe."
                    );
                installSource = Path.GetDirectoryName(Path.GetDirectoryName(streamlink))!;
                var ffmpeg = FindWindowsExecutable(installSource, "ffmpeg.exe", "ffmpeg")
                    ?? throw new InvalidOperationException(
                        "Streamlink archive did not contain ffmpeg/ffmpeg.exe."
                    );
                _ = ffmpeg;
            }
            else
            {
                TryMakeExecutable(archivePath);
                var extractRoot = Path.Combine(workRoot, "squashfs-root");
                await ExtractAppImageAsync(archivePath, workRoot, cancellationToken)
                    .ConfigureAwait(false);
                installSource = extractRoot;
                var streamlink = Path.Combine(installSource, "AppRun");
                var ffmpeg = Path.Combine(installSource, "usr", "bin", "ffmpeg");
                if (!File.Exists(streamlink) || !File.Exists(ffmpeg))
                {
                    throw new InvalidOperationException(
                        "Streamlink AppImage did not contain AppRun and usr/bin/ffmpeg."
                    );
                }

                TryMakeExecutable(streamlink);
                TryMakeExecutable(ffmpeg);
            }

            var metadata = new
            {
                release = asset.Release,
                asset = asset.Name,
                sha256 = asset.Sha256,
                source = asset.Url,
                platform = platform.CacheKey,
            };
            await File.WriteAllTextAsync(
                    Path.Combine(installSource, "aisp-media-bundle.json"),
                    JsonSerializer.Serialize(metadata, new JsonSerializerOptions { WriteIndented = true })
                        + Environment.NewLine,
                    cancellationToken
                )
                .ConfigureAwait(false);

            var target = RuntimeDataPaths.MediaPlatformDirectory;
            ReplaceDirectory(installSource, target);

            var installed =
                TryFindExisting()
                ?? throw new InvalidOperationException(
                    "Local Streamlink/FFmpeg installation is incomplete."
                );

            status?.Report("Streamlink + FFmpeg ready.");
            return installed;
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    private static string? FindCachedStreamlink()
    {
        var platform = RuntimeDataPaths.DetectPlatform();
        var root = RuntimeDataPaths.MediaPlatformDirectory;
        if (platform.IsWindows)
            return FindWindowsExecutable(root, "streamlink.exe", "bin");

        var appRun = Path.Combine(root, "AppRun");
        return File.Exists(appRun) ? appRun : null;
    }

    private static string? FindCachedFfmpeg()
    {
        var platform = RuntimeDataPaths.DetectPlatform();
        var root = RuntimeDataPaths.MediaPlatformDirectory;
        if (platform.IsWindows)
            return FindWindowsExecutable(root, "ffmpeg.exe", "ffmpeg");

        var ffmpeg = Path.Combine(root, "usr", "bin", "ffmpeg");
        return File.Exists(ffmpeg) ? ffmpeg : null;
    }

    private static string? FindOnPath(string command)
    {
        var pathEnv = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(pathEnv))
            return null;

        var extensions = RuntimeDataPaths.DetectPlatform().IsWindows
            ? (Environment.GetEnvironmentVariable("PATHEXT") ?? ".EXE;.CMD;.BAT").Split(
                ';',
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
            )
            : [""];

        foreach (var directory in pathEnv.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (var extension in extensions)
            {
                var candidate = Path.Combine(
                    directory,
                    extension.Length == 0 || command.EndsWith(extension, StringComparison.OrdinalIgnoreCase)
                        ? command
                        : command + extension
                );
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
        }

        return null;
    }

    private static BundleAsset SelectBundleAsset(
        GitHubApiReleaseDocument release,
        RuntimePlatform platform
    )
    {
        var matches = new List<GitHubApiAssetDocument>();
        foreach (var candidate in release.Assets)
        {
            if (platform.IsWindows)
            {
                if (candidate.Name.EndsWith("-x86_64.zip", StringComparison.OrdinalIgnoreCase))
                    matches.Add(candidate);
            }
            else
            {
                if (
                    candidate.Name.StartsWith("streamlink+ffmpeg-", StringComparison.Ordinal)
                    && candidate.Name.EndsWith(
                        $"_{platform.Architecture}.AppImage",
                        StringComparison.Ordinal
                    )
                )
                    matches.Add(candidate);
            }
        }

        if (matches.Count != 1)
        {
            throw new InvalidOperationException(
                $"Expected one Streamlink bundle for {platform.CacheKey}, found {matches.Count}."
            );
        }

        var asset = matches[0];
        if (
            string.IsNullOrWhiteSpace(asset.BrowserDownloadUrl)
            || !asset.BrowserDownloadUrl.StartsWith(
                "https://github.com/streamlink/",
                StringComparison.Ordinal
            )
            || asset.Size <= 0
            || asset.Digest is null
            || Sha256DigestRegex.Match(asset.Digest) is not { Success: true } digestMatch
        )
        {
            throw new InvalidOperationException("Selected Streamlink release asset is invalid.");
        }

        return new BundleAsset(
            asset.Name,
            asset.BrowserDownloadUrl,
            asset.Size,
            release.TagName,
            digestMatch.Groups[1].Value.ToLowerInvariant()
        );
    }

    private static string? FindWindowsExecutable(string root, string fileName, string preferredFolder)
    {
        if (!Directory.Exists(root))
            return null;

        var preferred = Path.Combine(root, preferredFolder, fileName);
        if (File.Exists(preferred))
            return preferred;

        return Directory
            .EnumerateFiles(root, fileName, SearchOption.AllDirectories)
            .FirstOrDefault();
    }

    private static void ExtractZipSafely(string archivePath, string destination)
    {
        Directory.CreateDirectory(destination);
        using var archive = ZipFile.OpenRead(archivePath);
        foreach (var entry in archive.Entries)
        {
            var relative = entry.FullName.Replace('\\', '/');
            if (
                string.IsNullOrEmpty(relative)
                || relative.StartsWith('/')
                || relative.Split('/').Contains("..", StringComparer.Ordinal)
            )
            {
                throw new InvalidOperationException(
                    $"Archive contains an unsafe path: {entry.FullName}"
                );
            }

            var target = Path.GetFullPath(Path.Combine(destination, entry.FullName));
            if (!target.StartsWith(Path.GetFullPath(destination), StringComparison.Ordinal))
            {
                throw new InvalidOperationException(
                    $"Archive contains an unsafe path: {entry.FullName}"
                );
            }

            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(target);
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            entry.ExtractToFile(target, overwrite: true);
        }
    }

    private static async Task ExtractAppImageAsync(
        string appImagePath,
        string workingDirectory,
        CancellationToken cancellationToken
    )
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = appImagePath,
            Arguments = "--appimage-extract",
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        using var process =
            Process.Start(startInfo)
            ?? throw new InvalidOperationException("Could not start Streamlink AppImage extraction.");

        var stdoutTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderrTask = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        var stderr = await stderrTask.ConfigureAwait(false);
        _ = await stdoutTask.ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                "Could not extract the Streamlink AppImage"
                    + (string.IsNullOrWhiteSpace(stderr) ? "." : $": {stderr.Trim()}")
            );
        }
    }

    private static void ReplaceDirectory(string source, string destination)
    {
        var parent = Path.GetDirectoryName(destination)!;
        Directory.CreateDirectory(parent);
        var staging = destination + ".new-" + Guid.NewGuid().ToString("N");
        try
        {
            CopyDirectory(source, staging);
            if (Directory.Exists(destination))
                Directory.Delete(destination, recursive: true);
            Directory.Move(staging, destination);
        }
        catch
        {
            TryDeleteDirectory(staging);
            throw;
        }
    }

    private static void CopyDirectory(string sourceDir, string destinationDir)
    {
        Directory.CreateDirectory(destinationDir);
        foreach (
            var directory in Directory.EnumerateDirectories(
                sourceDir,
                "*",
                SearchOption.AllDirectories
            )
        )
        {
            Directory.CreateDirectory(
                Path.Combine(destinationDir, Path.GetRelativePath(sourceDir, directory))
            );
        }

        foreach (
            var file in Directory.EnumerateFiles(sourceDir, "*", SearchOption.AllDirectories)
        )
        {
            var target = Path.Combine(destinationDir, Path.GetRelativePath(sourceDir, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target, overwrite: true);
        }
    }

    private static void TryMakeExecutable(string path)
    {
        if (!File.Exists(path) || OperatingSystem.IsWindows())
            return;
        try
        {
            File.SetUnixFileMode(
                path,
                UnixFileMode.UserRead
                    | UnixFileMode.UserWrite
                    | UnixFileMode.UserExecute
                    | UnixFileMode.GroupRead
                    | UnixFileMode.GroupExecute
                    | UnixFileMode.OtherRead
                    | UnixFileMode.OtherExecute
            );
        }
        catch
        {
            // Best-effort.
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: true);
        }
        catch
        {
            // Best-effort cleanup.
        }
    }

    private sealed record BundleAsset(
        string Name,
        string Url,
        long Size,
        string Release,
        string Sha256
    );
}
