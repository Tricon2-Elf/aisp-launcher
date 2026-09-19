using System.Formats.Tar;
using System.IO.Compression;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal static class DxvkRuntime
{
    public const string DllFileName = "d3d9.dll";

    private const string LatestReleaseApi =
        "https://api.github.com/repos/doitsujin/dxvk/releases/latest";
    private const string VersionStampName = "dxvk-version";

    private static readonly Regex TarballNameRegex = new(
        @"^dxvk-\d+(?:\.\d+)*\.tar\.gz$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled
    );

    private static readonly Regex Sha256DigestRegex = new(
        @"^sha256:([0-9a-fA-F]{64})$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled
    );

    public static string DllPath =>
        Path.Combine(RuntimeDataPaths.InstallDirectory, DllFileName);

    public static string VersionStampPath =>
        Path.Combine(RuntimeDataPaths.DataRoot, VersionStampName);

    public static bool IsInstalled() => File.Exists(DllPath) && File.Exists(VersionStampPath);

    public static bool NeedsDownload() =>
        LauncherBootstrap.Settings.UseDxvk && !IsInstalled();

    public static void RemoveInstalled()
    {
        if (!File.Exists(VersionStampPath))
            return;

        TryDelete(DllPath);
        TryDelete(VersionStampPath);
    }

    public static async Task EnsureInstalledAsync(
        RuntimeHttpClient http,
        IProgress<string>? status = null,
        IProgress<double>? downloadProgress = null,
        CancellationToken cancellationToken = default
    )
    {
        if (IsInstalled())
        {
            status?.Report("DXVK already installed.");
            return;
        }

        status?.Report("Checking latest DXVK release…");
        var release = await http.GetReleaseAsync(LatestReleaseApi, cancellationToken)
            .ConfigureAwait(false);
        var tag = string.IsNullOrWhiteSpace(release.TagName)
            ? throw new InvalidOperationException("DXVK release has no tag.")
            : release.TagName.Trim();
        var asset = SelectTarball(release);

        var workRoot = Path.Combine(
            RuntimeDataPaths.DataRoot,
            ".download-dxvk-" + Guid.NewGuid().ToString("N")
        );
        var archivePath = Path.Combine(workRoot, asset.Name);
        var extractedDll = Path.Combine(workRoot, DllFileName);

        try
        {
            Directory.CreateDirectory(workRoot);

            status?.Report($"Downloading DXVK {tag}…");
            await http.DownloadToFileAsync(
                    asset.Url,
                    archivePath,
                    downloadProgress,
                    cancellationToken
                )
                .ConfigureAwait(false);

            if (!string.IsNullOrEmpty(asset.Sha256))
            {
                status?.Report("Verifying DXVK download…");
                var actual = await RuntimeHttpClient
                    .ComputeSha256Async(archivePath, cancellationToken)
                    .ConfigureAwait(false);
                if (!actual.Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        $"SHA256 mismatch for {asset.Name}. Expected {asset.Sha256}, got {actual}."
                    );
                }
            }

            status?.Report("Installing DXVK d3d9.dll…");
            await ExtractX32D3d9Async(archivePath, extractedDll, cancellationToken)
                .ConfigureAwait(false);

            Directory.CreateDirectory(RuntimeDataPaths.DataRoot);
            File.Copy(extractedDll, DllPath, overwrite: true);
            File.WriteAllText(VersionStampPath, tag);

            if (!IsInstalled())
            {
                throw new InvalidOperationException(
                    $"DXVK installation incomplete: expected {DllPath}."
                );
            }

            status?.Report($"DXVK {tag} ready.");
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    private static DxvkAsset SelectTarball(GitHubApiReleaseDocument release)
    {
        var asset =
            release.Assets.FirstOrDefault(candidate => TarballNameRegex.IsMatch(candidate.Name))
            ?? throw new InvalidOperationException(
                $"DXVK release {release.TagName} has no dxvk-*.tar.gz asset."
            );

        if (
            string.IsNullOrWhiteSpace(asset.BrowserDownloadUrl)
            || !asset.BrowserDownloadUrl.StartsWith(
                "https://github.com/doitsujin/dxvk/",
                StringComparison.Ordinal
            )
            || asset.Size <= 0
        )
        {
            throw new InvalidOperationException("Selected DXVK release asset is invalid.");
        }

        string? sha256 = null;
        if (
            asset.Digest is not null
            && Sha256DigestRegex.Match(asset.Digest) is { Success: true } digestMatch
        )
            sha256 = digestMatch.Groups[1].Value.ToLowerInvariant();

        return new DxvkAsset(asset.Name, asset.BrowserDownloadUrl, sha256);
    }

    private static async Task ExtractX32D3d9Async(
        string tarGzPath,
        string destinationDll,
        CancellationToken cancellationToken
    )
    {
        await using var file = File.OpenRead(tarGzPath);
        await using var gzip = new GZipStream(file, CompressionMode.Decompress);
        await using var reader = new TarReader(gzip);

        while (
            await reader.GetNextEntryAsync(copyData: false, cancellationToken).ConfigureAwait(false)
                is { } entry
        )
        {
            if (entry.EntryType is not TarEntryType.RegularFile and not TarEntryType.V7RegularFile)
                continue;
            if (!IsX32D3d9(entry.Name))
                continue;

            Directory.CreateDirectory(Path.GetDirectoryName(destinationDll)!);
            if (entry.DataStream is { } data)
            {
                await using var output = new FileStream(
                    destinationDll,
                    FileMode.Create,
                    FileAccess.Write,
                    FileShare.None,
                    bufferSize: 81920,
                    useAsync: true
                );
                await data.CopyToAsync(output, cancellationToken).ConfigureAwait(false);
            }
            else
            {
                entry.ExtractToFile(destinationDll, overwrite: true);
            }

            if (!File.Exists(destinationDll) || new FileInfo(destinationDll).Length == 0)
            {
                throw new InvalidOperationException(
                    "DXVK archive entry x32/d3d9.dll was empty."
                );
            }

            return;
        }

        throw new InvalidOperationException("DXVK archive did not contain x32/d3d9.dll.");
    }

    private static bool IsX32D3d9(string entryName)
    {
        var parts = entryName
            .Replace('\\', '/')
            .Split('/', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length >= 2
            && parts[^2].Equals("x32", StringComparison.OrdinalIgnoreCase)
            && parts[^1].Equals(DllFileName, StringComparison.OrdinalIgnoreCase);
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // Best-effort; the next enable will overwrite.
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

    private readonly record struct DxvkAsset(string Name, string Url, string? Sha256);
}
