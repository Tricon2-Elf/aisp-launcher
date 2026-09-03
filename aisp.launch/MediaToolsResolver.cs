using System.Diagnostics;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal readonly record struct RuntimePlatform(string OperatingSystem, string Architecture)
{
    public string CacheKey => $"{OperatingSystem}-{Architecture}";
}

internal readonly record struct BundleAsset(
    string Name,
    string Url,
    long Size,
    string Release,
    string Sha256
);

public readonly record struct MediaTools(string Streamlink, string Ffmpeg);

internal static partial class MediaToolsResolver
{
    private const string UserAgent = "aisp.launch/1.0";
    private const long MaxDownloadBytes = 512L * 1024 * 1024;
    private const long MaxExtractedBytes = 1024L * 1024 * 1024;

    private const string WindowsReleaseApi =
        "https://api.github.com/repos/streamlink/windows-builds/releases/latest";

    private static readonly HttpClient HttpClient = new()
    {
        Timeout = TimeSpan.FromMinutes(5),
    };

    public static string GetCacheDirectory(string? configuredDirectory = null)
    {
        var directory = string.IsNullOrWhiteSpace(configuredDirectory)
            ? NicoTvConstants.DefaultToolsDirectory
            : configuredDirectory;
        return Path.IsPathRooted(directory)
            ? directory
            : Path.Combine(AppContext.BaseDirectory, directory);
    }

    public static async Task<MediaTools> ResolveAsync(
        string? toolsDirectory = null,
        CancellationToken cancellationToken = default
    )
    {
        var systemStreamlink = FindOnPath("streamlink");
        var systemFfmpeg = FindOnPath("ffmpeg");
        if (systemStreamlink is not null && systemFfmpeg is not null)
        {
            Trace.WriteLine($"Using Streamlink: {systemStreamlink}");
            Trace.WriteLine($"Using FFmpeg: {systemFfmpeg}");
            return new MediaTools(systemStreamlink, systemFfmpeg);
        }

        var cacheDirectory = GetCacheDirectory(toolsDirectory);
        var platform = DetectRuntimePlatform();
        var cached = FindCachedBundle(cacheDirectory, platform);
        if (cached is not null)
        {
            Trace.WriteLine($"Using Streamlink: {cached.Value.Streamlink}");
            Trace.WriteLine($"Using FFmpeg: {cached.Value.Ffmpeg}");
            return cached.Value;
        }

        var downloaded = await DownloadStreamlinkBundleAsync(
            cacheDirectory,
            platform,
            cancellationToken
        );
        Trace.WriteLine($"Using Streamlink: {downloaded.Streamlink}");
        Trace.WriteLine($"Using FFmpeg: {downloaded.Ffmpeg}");
        return downloaded;
    }

    private static string? FindOnPath(string command)
    {
        var pathValue = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(pathValue))
            return null;

        var extensions = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? Environment.GetEnvironmentVariable("PATHEXT")?.Split(';') ?? [".EXE", ".CMD", ".BAT"]
            : [string.Empty];

        foreach (var directory in pathValue.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (var extension in extensions)
            {
                var candidate = Path.Combine(directory, command + extension);
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
        }

        return null;
    }

    private static RuntimePlatform DetectRuntimePlatform()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return RuntimeInformation.OSArchitecture switch
            {
                Architecture.X64 or Architecture.X86 => new RuntimePlatform("windows", "x86_64"),
                Architecture.Arm64 => new RuntimePlatform("windows", "x86_64"),
                _ => throw new NotSupportedException(
                    $"Automatic Streamlink setup does not support Windows architecture {RuntimeInformation.OSArchitecture}."
                ),
            };
        }

        if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
        {
            return RuntimeInformation.OSArchitecture switch
            {
                Architecture.X64 => new RuntimePlatform("linux", "x86_64"),
                Architecture.Arm64 => new RuntimePlatform("linux", "aarch64"),
                _ => throw new NotSupportedException(
                    $"Automatic Streamlink setup does not support Linux architecture {RuntimeInformation.OSArchitecture}."
                ),
            };
        }

        throw new NotSupportedException(
            "Automatic Streamlink setup supports Windows and Linux only."
        );
    }

    private static MediaTools? FindCachedBundle(string cacheDirectory, RuntimePlatform platform)
    {
        var bundleDirectory = Path.Combine(cacheDirectory, platform.CacheKey);
        if (!Directory.Exists(bundleDirectory))
            return null;

        if (platform.OperatingSystem == "windows")
        {
            var streamlink = FindWindowsExecutable(bundleDirectory, "streamlink.exe", "bin");
            var ffmpeg = FindWindowsExecutable(bundleDirectory, "ffmpeg.exe", "ffmpeg");
            if (streamlink is null || ffmpeg is null)
                return null;
            return new MediaTools(streamlink, ffmpeg);
        }

        var appRun = Path.Combine(bundleDirectory, "AppRun");
        var linuxFfmpeg = Path.Combine(bundleDirectory, "usr", "bin", "ffmpeg");
        if (!File.Exists(appRun) || !File.Exists(linuxFfmpeg))
            return null;

        return new MediaTools(appRun, linuxFfmpeg);
    }

    private static async Task<MediaTools> DownloadStreamlinkBundleAsync(
        string cacheDirectory,
        RuntimePlatform platform,
        CancellationToken cancellationToken
    )
    {
        if (platform.OperatingSystem != "windows")
        {
            throw new NotSupportedException(
                "Automatic Streamlink download is currently supported on Windows only. "
                    + "Install Streamlink and FFmpeg on PATH, or run the launcher on Windows."
            );
        }

        Directory.CreateDirectory(cacheDirectory);
        var asset = SelectWindowsBundleAsset(
            await FetchLatestReleaseAsync(WindowsReleaseApi, cancellationToken)
        );

        Trace.WriteLine(
            $"Downloading Streamlink and FFmpeg {asset.Release} for {platform.CacheKey} "
                + $"({asset.Size / 1024.0 / 1024.0:F1} MiB)..."
        );

        var stagingDirectory = Path.Combine(
            cacheDirectory,
            $".download-{Guid.NewGuid():N}"
        );
        Directory.CreateDirectory(stagingDirectory);

        try
        {
            var archivePath = Path.Combine(stagingDirectory, asset.Name);
            await DownloadAssetAsync(asset, archivePath, cancellationToken);

            var extractedDirectory = Path.Combine(stagingDirectory, "extracted");
            ExtractZipSafely(archivePath, extractedDirectory);

            var streamlink = FindWindowsExecutable(extractedDirectory, "streamlink.exe", "bin")
                ?? throw new InvalidOperationException(
                    "The downloaded Streamlink archive did not contain bin/streamlink.exe."
                );
            var installSource = Directory.GetParent(streamlink)!.Parent!.FullName;
            var ffmpeg = FindWindowsExecutable(installSource, "ffmpeg.exe", "ffmpeg")
                ?? throw new InvalidOperationException(
                    "The downloaded Streamlink archive did not contain ffmpeg/ffmpeg.exe."
                );

            var metadata = JsonSerializer.Serialize(
                new
                {
                    release = asset.Release,
                    asset = asset.Name,
                    sha256 = asset.Sha256,
                    source = asset.Url,
                    platform = platform.CacheKey,
                },
                new JsonSerializerOptions { WriteIndented = true }
            );
            await File.WriteAllTextAsync(
                Path.Combine(installSource, "aispace-nicotv-bundle.json"),
                metadata + Environment.NewLine,
                cancellationToken
            );

            var targetDirectory = Path.Combine(cacheDirectory, platform.CacheKey);
            ReplaceDirectory(installSource, targetDirectory);

            return FindCachedBundle(cacheDirectory, platform)
                ?? throw new InvalidOperationException(
                    "The local Streamlink/FFmpeg installation is incomplete."
                );
        }
        finally
        {
            try
            {
                Directory.Delete(stagingDirectory, recursive: true);
            }
            catch
            {
                // Best-effort cleanup of the staging directory.
            }
        }
    }

    private static async Task<JsonElement> FetchLatestReleaseAsync(
        string apiUrl,
        CancellationToken cancellationToken
    )
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, apiUrl);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        request.Headers.UserAgent.ParseAdd(UserAgent);

        using var response = await HttpClient.SendAsync(request, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        return document.RootElement.Clone();
    }

    private static BundleAsset SelectWindowsBundleAsset(JsonElement release)
    {
        var releaseName = release.TryGetProperty("tag_name", out var tagName)
            ? tagName.GetString() ?? "latest"
            : "latest";

        JsonElement? match = null;
        var matchCount = 0;
        if (release.TryGetProperty("assets", out var assets))
        {
            foreach (var candidate in assets.EnumerateArray())
            {
                if (!candidate.TryGetProperty("name", out var nameElement))
                    continue;

                var name = nameElement.GetString();
                if (name is null || !name.EndsWith("-x86_64.zip", StringComparison.Ordinal))
                    continue;

                match = candidate;
                matchCount++;
            }
        }

        if (matchCount != 1 || match is null)
        {
            throw new InvalidOperationException(
                $"Expected one Streamlink Windows bundle, but the release contained {matchCount} matches."
            );
        }

        var asset = match.Value;
        var assetName = asset.GetProperty("name").GetString()
            ?? throw new InvalidOperationException("The selected Streamlink release asset is invalid.");
        var url = asset.GetProperty("browser_download_url").GetString()
            ?? throw new InvalidOperationException("The selected Streamlink release asset is invalid.");
        var size = asset.GetProperty("size").GetInt64();
        var digest = asset.GetProperty("digest").GetString()
            ?? throw new InvalidOperationException("The selected Streamlink release asset is invalid.");

        if (
            !url.StartsWith("https://github.com/streamlink/", StringComparison.Ordinal)
            || size <= 0
            || size > MaxDownloadBytes
            || !Sha256DigestPattern().IsMatch(digest)
        )
        {
            throw new InvalidOperationException("The selected Streamlink release asset is invalid.");
        }

        return new BundleAsset(
            assetName,
            url,
            size,
            releaseName,
            digest["sha256:".Length..].ToLowerInvariant()
        );
    }

    private static async Task DownloadAssetAsync(
        BundleAsset asset,
        string destination,
        CancellationToken cancellationToken
    )
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, asset.Url);
        request.Headers.UserAgent.ParseAdd(UserAgent);

        using var response = await HttpClient.SendAsync(
            request,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken
        );
        response.EnsureSuccessStatusCode();

        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using var output = File.Create(destination);

        var buffer = new byte[1024 * 1024];
        long downloaded = 0;
        var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);

        while (true)
        {
            var read = await input.ReadAsync(buffer, cancellationToken);
            if (read == 0)
                break;

            downloaded += read;
            if (downloaded > MaxDownloadBytes)
                throw new InvalidOperationException("The Streamlink bundle exceeded the size limit.");

            hash.AppendData(buffer, 0, read);
            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
        }

        if (downloaded != asset.Size)
        {
            throw new InvalidOperationException(
                $"Downloaded {downloaded} bytes for {asset.Name}, expected {asset.Size}."
            );
        }

        var actualDigest = Convert.ToHexStringLower(hash.GetHashAndReset());
        if (actualDigest != asset.Sha256)
        {
            throw new InvalidOperationException(
                $"SHA-256 mismatch for {asset.Name}: expected {asset.Sha256}, received {actualDigest}."
            );
        }
    }

    private static void ExtractZipSafely(string archivePath, string destination)
    {
        Directory.CreateDirectory(destination);
        long extractedBytes = 0;

        using var archive = System.IO.Compression.ZipFile.OpenRead(archivePath);
        foreach (var entry in archive.Entries)
        {
            extractedBytes += entry.Length;
            if (extractedBytes > MaxExtractedBytes)
            {
                throw new InvalidOperationException(
                    "The Streamlink archive exceeds the extraction size limit."
                );
            }

            var relative = entry.FullName.Replace('\\', '/');
            if (
                relative.StartsWith("/", StringComparison.Ordinal)
                || relative.Split('/').Contains("..", StringComparer.Ordinal)
            )
            {
                throw new InvalidOperationException(
                    $"The Streamlink archive contains an unsafe path: {entry.FullName}"
                );
            }

            var target = Path.Combine(
                [destination, .. relative.Split('/', StringSplitOptions.RemoveEmptyEntries)]
            );
            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(target);
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using (var source = entry.Open())
            using (var output = File.Create(target))
            {
                source.CopyTo(output);
            }
        }
    }

    private static string? FindWindowsExecutable(
        string directory,
        string fileName,
        string parentName
    )
    {
        if (!Directory.Exists(directory))
            return null;

        string? match = null;
        foreach (var path in Directory.EnumerateFiles(directory, fileName, SearchOption.AllDirectories))
        {
            if (
                !string.Equals(
                    Path.GetFileName(Path.GetDirectoryName(path)),
                    parentName,
                    StringComparison.OrdinalIgnoreCase
                )
            )
            {
                continue;
            }

            if (match is not null)
                return null;

            match = path;
        }

        return match;
    }

    private static void ReplaceDirectory(string source, string target)
    {
        var backup = target + ".previous";
        if (Directory.Exists(backup))
            Directory.Delete(backup, recursive: true);
        if (Directory.Exists(target))
            Directory.Move(target, backup);

        try
        {
            Directory.Move(source, target);
        }
        catch
        {
            if (Directory.Exists(backup) && !Directory.Exists(target))
                Directory.Move(backup, target);
            throw;
        }

        if (Directory.Exists(backup))
            Directory.Delete(backup, recursive: true);
    }

    [GeneratedRegex("^sha256:[0-9a-fA-F]{64}$", RegexOptions.CultureInvariant)]
    private static partial Regex Sha256DigestPattern();
}
