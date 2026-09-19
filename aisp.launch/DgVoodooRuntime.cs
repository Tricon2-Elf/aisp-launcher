using System.IO.Compression;
using System.Reflection;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal static class DgVoodooRuntime
{
    public const string Version = "v2.87.5";
    public const string DllFileName = "d3d9.dll";
    public const string ConfigFileName = "dgVoodoo.conf";
    public const string CplFileName = "dgVoodooCpl.exe";

    private const string ReleaseApi =
        "https://api.github.com/repos/dege-diosg/dgVoodoo2/releases/tags/v2.87.5";
    private const string ZipAssetName = "dgVoodoo2_87_5.zip";
    private const string EmbeddedConfig = "aisp.hook.dgVoodoo.conf";
    private const string VersionStampName = "dgvoodoo-version";

    private static readonly Regex Sha256DigestRegex = new(
        @"^sha256:([0-9a-fA-F]{64})$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled
    );

    public static string DllPath =>
        Path.Combine(RuntimeDataPaths.InstallDirectory, DllFileName);

    public static string ManagedConfigPath =>
        Path.Combine(RuntimeDataPaths.DataRoot, ConfigFileName);

    public static string CplPath =>
        Path.Combine(RuntimeDataPaths.DataRoot, CplFileName);

    public static string VersionStampPath =>
        Path.Combine(RuntimeDataPaths.DataRoot, VersionStampName);

    public static bool IsInstalled() =>
        File.Exists(DllPath)
        && File.Exists(CplPath)
        && File.Exists(VersionStampPath)
        && string.Equals(
            TryReadInstalledVersion(),
            Version,
            StringComparison.OrdinalIgnoreCase
        );

    public static bool NeedsDownload() =>
        LauncherBootstrap.Settings.UseDgVoodoo && !IsInstalled();

    public static void RemoveInstalled()
    {
        if (!File.Exists(VersionStampPath))
            return;

        TryDelete(DllPath);
        TryDelete(Path.Combine(RuntimeDataPaths.InstallDirectory, ConfigFileName));
        TryDelete(VersionStampPath);
    }

    /// <summary>
    /// Copies the player-edited config from aisp.launch.data into the game directory
    /// so D3D9.dll picks it up. Writes the embedded default only if the managed file
    /// is missing (an existing game-directory config is migrated first).
    /// </summary>
    public static void ApplyGameConfig(string gameDirectory)
    {
        EnsureManagedConfig();
        if (!File.Exists(ManagedConfigPath))
        {
            throw new InvalidOperationException(
                $"dgVoodoo config was not found at '{ManagedConfigPath}'."
            );
        }

        Directory.CreateDirectory(gameDirectory);
        File.Copy(
            ManagedConfigPath,
            Path.Combine(gameDirectory, ConfigFileName),
            overwrite: true
        );
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
            EnsureManagedConfig();
            status?.Report("dgVoodoo already installed.");
            return;
        }

        DxvkRuntime.RemoveInstalled();

        status?.Report($"Checking dgVoodoo {Version}…");
        var release = await http.GetReleaseAsync(ReleaseApi, cancellationToken)
            .ConfigureAwait(false);
        var asset = SelectZip(release);

        var workRoot = Path.Combine(
            RuntimeDataPaths.DataRoot,
            ".download-dgvoodoo-" + Guid.NewGuid().ToString("N")
        );
        var archivePath = Path.Combine(workRoot, asset.Name);
        var extractedDll = Path.Combine(workRoot, DllFileName);
        var extractedCpl = Path.Combine(workRoot, CplFileName);

        try
        {
            Directory.CreateDirectory(workRoot);

            status?.Report($"Downloading dgVoodoo {Version}…");
            await http.DownloadToFileAsync(
                    asset.Url,
                    archivePath,
                    downloadProgress,
                    cancellationToken
                )
                .ConfigureAwait(false);

            if (!string.IsNullOrEmpty(asset.Sha256))
            {
                status?.Report("Verifying dgVoodoo download…");
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

            status?.Report("Installing dgVoodoo D3D9.dll…");
            ExtractZipFiles(archivePath, extractedDll, extractedCpl);

            Directory.CreateDirectory(RuntimeDataPaths.DataRoot);
            File.Copy(extractedDll, DllPath, overwrite: true);
            File.Copy(extractedCpl, CplPath, overwrite: true);
            EnsureManagedConfig();
            File.WriteAllText(VersionStampPath, Version);

            if (!IsInstalled())
            {
                throw new InvalidOperationException(
                    $"dgVoodoo installation incomplete: expected {DllPath} and {CplPath}."
                );
            }

            status?.Report($"dgVoodoo {Version} ready.");
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    private static DgVoodooAsset SelectZip(GitHubApiReleaseDocument release)
    {
        var asset =
            release.Assets.FirstOrDefault(candidate =>
                candidate.Name.Equals(ZipAssetName, StringComparison.OrdinalIgnoreCase)
            )
            ?? throw new InvalidOperationException(
                $"dgVoodoo release {release.TagName} has no {ZipAssetName} asset."
            );

        if (
            string.IsNullOrWhiteSpace(asset.BrowserDownloadUrl)
            || !asset.BrowserDownloadUrl.StartsWith(
                "https://github.com/dege-diosg/dgVoodoo2/",
                StringComparison.Ordinal
            )
            || asset.Size <= 0
        )
        {
            throw new InvalidOperationException("Selected dgVoodoo release asset is invalid.");
        }

        string? sha256 = null;
        if (
            asset.Digest is not null
            && Sha256DigestRegex.Match(asset.Digest) is { Success: true } digestMatch
        )
            sha256 = digestMatch.Groups[1].Value.ToLowerInvariant();

        return new DgVoodooAsset(asset.Name, asset.BrowserDownloadUrl, sha256);
    }

    private static void ExtractZipFiles(string zipPath, string destinationDll, string destinationCpl)
    {
        using var archive = ZipFile.OpenRead(zipPath);
        var dllEntry =
            archive.Entries.FirstOrDefault(candidate => IsMsX86D3d9(candidate.FullName))
            ?? throw new InvalidOperationException(
                "dgVoodoo archive did not contain MS/x86/D3D9.dll."
            );
        var cplEntry =
            archive.Entries.FirstOrDefault(candidate => IsRootCpl(candidate.FullName))
            ?? throw new InvalidOperationException(
                "dgVoodoo archive did not contain dgVoodooCpl.exe."
            );

        ExtractEntry(dllEntry, destinationDll, "MS/x86/D3D9.dll");
        ExtractEntry(cplEntry, destinationCpl, CplFileName);
    }

    private static void ExtractEntry(ZipArchiveEntry entry, string destination, string label)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        entry.ExtractToFile(destination, overwrite: true);
        if (!File.Exists(destination) || new FileInfo(destination).Length == 0)
            throw new InvalidOperationException($"dgVoodoo archive entry {label} was empty.");
    }

    private static bool IsRootCpl(string entryName)
    {
        var parts = entryName
            .Replace('\\', '/')
            .Split('/', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length == 1
            && parts[0].Equals(CplFileName, StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsMsX86D3d9(string entryName)
    {
        var parts = entryName
            .Replace('\\', '/')
            .Split('/', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length >= 3
            && parts[^3].Equals("MS", StringComparison.OrdinalIgnoreCase)
            && parts[^2].Equals("x86", StringComparison.OrdinalIgnoreCase)
            && parts[^1].Equals("D3D9.dll", StringComparison.OrdinalIgnoreCase);
    }

    private static void EnsureManagedConfig()
    {
        if (File.Exists(ManagedConfigPath))
            return;

        Directory.CreateDirectory(RuntimeDataPaths.DataRoot);
        var gameConfig = Path.Combine(RuntimeDataPaths.InstallDirectory, ConfigFileName);
        if (File.Exists(gameConfig))
        {
            File.Copy(gameConfig, ManagedConfigPath);
            return;
        }

        WriteEmbeddedConfigIfMissing(ManagedConfigPath);
    }

    private static void WriteEmbeddedConfigIfMissing(string destinationPath)
    {
        if (File.Exists(destinationPath))
            return;

        var assembly = Assembly.GetExecutingAssembly();
        using var stream =
            assembly.GetManifestResourceStream(EmbeddedConfig)
            ?? throw new InvalidOperationException(
                $"Embedded dgVoodoo config '{EmbeddedConfig}' was not found."
            );
        using var output = new FileStream(
            destinationPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None
        );
        stream.CopyTo(output);
    }

    private static string? TryReadInstalledVersion()
    {
        if (!File.Exists(VersionStampPath))
            return null;
        try
        {
            var text = File.ReadAllText(VersionStampPath).Trim();
            return text.Length > 0 ? text : null;
        }
        catch
        {
            return null;
        }
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

    private readonly record struct DgVoodooAsset(string Name, string Url, string? Sha256);
}
