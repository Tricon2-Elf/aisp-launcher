using System.IO.Compression;
using System.Reflection;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal static partial class ElectronRuntime
{
    public const string Version = "44.1.1";

    private const string EmbeddedMainJs = "aisp.electron.app.main.js";
    private const string EmbeddedPackageJson = "aisp.electron.app.package.json";
    private const string EmbeddedHostJs = "aisp.electron.host.js";

    [GeneratedRegex(@"^([0-9a-fA-F]{64})\s+\*?(\S+)$", RegexOptions.CultureInvariant)]
    private static partial Regex ShaLineRegex();

    public static bool UseWindowsElectronBinary =>
        RuntimeDataPaths.DetectPlatform().IsWindows && !WineDetection.IsRunningOnWine;

    public static string ElectronExecutablePath =>
        Path.Combine(
            RuntimeDataPaths.ElectronDirectory,
            UseWindowsElectronBinary ? "electron.exe" : "electron"
        );

    public static string AppDirectory =>
        WineDetection.IsRunningOnWine
            ? Path.Combine(RuntimeDataPaths.ElectronDirectory, "app")
            : Path.Combine(RuntimeDataPaths.ElectronDirectory, "resources", "app");

    public static string HostJsPath =>
        Path.Combine(RuntimeDataPaths.ElectronDirectory, "host.js");

    public static bool IsInstalled() => File.Exists(ElectronExecutablePath);

    public static bool IsAppInstalled() =>
        File.Exists(Path.Combine(AppDirectory, "main.js"))
        && File.Exists(Path.Combine(AppDirectory, "package.json"));

    public static async Task EnsureInstalledAsync(
        RuntimeHttpClient http,
        IProgress<string>? status = null,
        IProgress<double>? downloadProgress = null,
        CancellationToken cancellationToken = default
    )
    {
        if (!IsInstalled())
            await DownloadAndExtractAsync(http, status, downloadProgress, cancellationToken)
                .ConfigureAwait(false);

        status?.Report("Installing Electron app…");
        EnsureAppFiles();
        status?.Report("Electron ready.");
    }

    public static void EnsureAppFiles()
    {
        if (!IsInstalled())
        {
            throw new InvalidOperationException(
                "Electron runtime is not installed; cannot write the app into resources/app."
            );
        }

        var appDir = AppDirectory;
        Directory.CreateDirectory(appDir);

        WriteEmbeddedResource(EmbeddedMainJs, Path.Combine(appDir, "main.js"));
        WriteEmbeddedResource(EmbeddedPackageJson, Path.Combine(appDir, "package.json"));
        if (WineDetection.IsRunningOnWine)
            WriteEmbeddedResource(EmbeddedHostJs, HostJsPath);

        // Prefer our app folder over Electron's stock default_app.asar.
        TryDeleteFile(
            Path.Combine(RuntimeDataPaths.ElectronDirectory, "resources", "default_app.asar")
        );
    }

    private static async Task DownloadAndExtractAsync(
        RuntimeHttpClient http,
        IProgress<string>? status,
        IProgress<double>? downloadProgress,
        CancellationToken cancellationToken
    )
    {
        var platform = RuntimeDataPaths.DetectPlatform();
        var windowsBinary = UseWindowsElectronBinary;
        if (!windowsBinary && platform.IsLinux && platform.Architecture != "x86_64")
        {
            throw new PlatformNotSupportedException(
                $"Electron v{Version} auto-download is only configured for linux-x64 "
                    + $"(current: {platform.CacheKey})."
            );
        }

        var zipName = windowsBinary
            ? $"electron-v{Version}-win32-x64.zip"
            : $"electron-v{Version}-linux-x64.zip";
        var downloadUrl =
            $"https://github.com/electron/electron/releases/download/v{Version}/{zipName}";

        var workRoot = Path.Combine(
            RuntimeDataPaths.DataRoot,
            ".download-electron-" + Guid.NewGuid().ToString("N")
        );
        var zipPath = Path.Combine(workRoot, zipName);
        var extractDir = Path.Combine(workRoot, "extract");

        try
        {
            Directory.CreateDirectory(extractDir);

            status?.Report($"Downloading Electron v{Version}…");
            await http.DownloadToFileAsync(downloadUrl, zipPath, downloadProgress, cancellationToken)
                .ConfigureAwait(false);

            status?.Report("Verifying Electron download…");
            var expectedSha = await TryFetchElectronSha256Async(http, zipName, cancellationToken)
                .ConfigureAwait(false);
            if (!string.IsNullOrEmpty(expectedSha))
            {
                var actual = await RuntimeHttpClient
                    .ComputeSha256Async(zipPath, cancellationToken)
                    .ConfigureAwait(false);
                if (!actual.Equals(expectedSha, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        $"SHA256 mismatch for {zipName}. Expected {expectedSha}, got {actual}."
                    );
                }
            }

            status?.Report("Extracting Electron…");
            ExtractZipSafely(zipPath, extractDir);

            var extractedElectron = FindElectronBinary(extractDir, windowsBinary);
            if (extractedElectron is null)
            {
                throw new InvalidOperationException(
                    $"Electron archive {zipName} did not contain the electron binary."
                );
            }

            var targetDir = RuntimeDataPaths.ElectronDirectory;
            if (Directory.Exists(targetDir))
                Directory.Delete(targetDir, recursive: true);

            var sourceRoot = Path.GetDirectoryName(extractedElectron)!;
            CopyDirectory(sourceRoot, targetDir);

            if (!windowsBinary && !WineDetection.IsRunningOnWine)
            {
                TryMakeExecutable(ElectronExecutablePath);
                TryMakeExecutable(Path.Combine(targetDir, "chrome_crashpad_handler"));
            }

            if (!IsInstalled())
            {
                throw new InvalidOperationException(
                    $"Electron installation incomplete at '{targetDir}'."
                );
            }
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    private static void WriteEmbeddedResource(string resourceName, string destinationPath)
    {
        var assembly = Assembly.GetExecutingAssembly();
        using var stream =
            assembly.GetManifestResourceStream(resourceName)
            ?? throw new InvalidOperationException(
                $"Embedded Electron app resource '{resourceName}' was not found."
            );
        using var output = new FileStream(
            destinationPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None
        );
        stream.CopyTo(output);
    }

    private static async Task<string?> TryFetchElectronSha256Async(
        RuntimeHttpClient http,
        string zipName,
        CancellationToken cancellationToken
    )
    {
        try
        {
            var sumsUrl =
                $"https://github.com/electron/electron/releases/download/v{Version}/SHASUMS256.txt";
            var sumsPath = Path.Combine(
                Path.GetTempPath(),
                $"electron-SHASUMS256-{Version}-{Guid.NewGuid():N}.txt"
            );
            await http.DownloadToFileAsync(sumsUrl, sumsPath, progress: null, cancellationToken)
                .ConfigureAwait(false);
            try
            {
                foreach (
                    var line in await File.ReadAllLinesAsync(sumsPath, cancellationToken)
                        .ConfigureAwait(false)
                )
                {
                    var match = ShaLineRegex().Match(line.Trim());
                    if (!match.Success)
                        continue;
                    if (match.Groups[2].Value.Equals(zipName, StringComparison.OrdinalIgnoreCase))
                        return match.Groups[1].Value.ToLowerInvariant();
                }
            }
            finally
            {
                try
                {
                    File.Delete(sumsPath);
                }
                catch
                {
                    // ignore
                }
            }
        }
        catch
        {
            // Verification is best-effort if SHASUMS cannot be fetched.
        }

        return null;
    }

    private static string? FindElectronBinary(string root, bool windows)
    {
        var name = windows ? "electron.exe" : "electron";
        var direct = Path.Combine(root, name);
        if (File.Exists(direct))
            return direct;

        return Directory
            .EnumerateFiles(root, name, SearchOption.AllDirectories)
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
                    $"Electron archive contains an unsafe path: {entry.FullName}"
                );
            }

            var target = Path.GetFullPath(Path.Combine(destination, entry.FullName));
            if (!target.StartsWith(Path.GetFullPath(destination), StringComparison.Ordinal))
            {
                throw new InvalidOperationException(
                    $"Electron archive contains an unsafe path: {entry.FullName}"
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
            // Best-effort on restricted filesystems.
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // Best-effort; resources/app still takes precedence when present.
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
}
