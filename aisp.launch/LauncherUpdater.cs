using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace aisp.launch;

internal sealed class LauncherUpdater(LauncherSettings settings, GitHubReleaseClient? client = null)
{
    private const string LauncherExeName = "aisp.launch.exe";
    private const string HookDllName = "aisp.hook.dll";

    private readonly GitHubReleaseClient _client = client ?? new GitHubReleaseClient();

    public async Task<UpdateCheckResult> CheckForUpdateAsync(
        CancellationToken cancellationToken = default
    )
    {
        var release = await _client
            .GetLatestReleaseAsync(settings.GitHubRepo, cancellationToken)
            .ConfigureAwait(false);

        if (!LaunchVersion.IsNewerThanCurrent(release.Version))
        {
            return new UpdateCheckResult(
                UpdateAvailability.UpToDate,
                release,
                $"Already on the latest version ({LaunchVersion.Display})."
            );
        }

        return new UpdateCheckResult(
            UpdateAvailability.UpdateAvailable,
            release,
            $"Update available: {LaunchVersion.Display} → {release.Version}"
        );
    }

    public async Task ApplyUpdateAsync(
        GitHubReleaseInfo release,
        IProgress<string>? status = null,
        IProgress<double>? downloadProgress = null,
        CancellationToken cancellationToken = default
    )
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            throw new PlatformNotSupportedException(
                "Self-update is only supported on Windows."
            );

        var installDir =
            Path.GetDirectoryName(Environment.ProcessPath)
            ?? throw new InvalidOperationException("Could not determine the launcher install directory.");

        EnsureGameNotRunning(settings.GameExecutable);

        var workRoot = Path.Combine(Path.GetTempPath(), "aisp-update", Guid.NewGuid().ToString("N"));
        var zipPath = Path.Combine(workRoot, release.ZipFileName);
        var extractDir = Path.Combine(workRoot, "extract");
        Directory.CreateDirectory(workRoot);
        Directory.CreateDirectory(extractDir);

        try
        {
            status?.Report("Downloading update…");
            await _client
                .DownloadToFileAsync(
                    release.DownloadUrl,
                    zipPath,
                    downloadProgress,
                    cancellationToken
                )
                .ConfigureAwait(false);

            if (!string.IsNullOrEmpty(release.Sha256))
            {
                status?.Report("Verifying download…");
                var actual = await ComputeSha256Async(zipPath, cancellationToken)
                    .ConfigureAwait(false);
                if (!actual.Equals(release.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        $"SHA256 mismatch. Expected {release.Sha256}, got {actual}."
                    );
                }
            }

            status?.Report("Extracting update…");
            ZipFile.ExtractToDirectory(zipPath, extractDir);

            var newExe = FindExtractedFile(extractDir, LauncherExeName);
            var newDll = FindExtractedFile(extractDir, HookDllName);
            if (newExe is null || newDll is null)
            {
                throw new InvalidOperationException(
                    $"Update zip must contain '{LauncherExeName}' and '{HookDllName}'."
                );
            }

            status?.Report("Preparing to restart…");
            var scriptPath = Path.Combine(workRoot, "apply-update.cmd");
            WriteApplyScript(
                scriptPath,
                Environment.ProcessId,
                newExe,
                newDll,
                Path.Combine(installDir, LauncherExeName),
                Path.Combine(installDir, HookDllName),
                workRoot
            );

            var startInfo = new ProcessStartInfo
            {
                FileName = "cmd.exe",
                Arguments = $"/C \"\"{scriptPath}\"\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = workRoot,
            };
            Process.Start(startInfo);
        }
        catch
        {
            TryDeleteDirectory(workRoot);
            throw;
        }
    }

    private static void EnsureGameNotRunning(string gameExecutable)
    {
        var processName = Path.GetFileNameWithoutExtension(gameExecutable);
        if (string.IsNullOrWhiteSpace(processName))
            return;

        var running = Process.GetProcessesByName(processName);
        try
        {
            if (running.Length > 0)
            {
                throw new InvalidOperationException(
                    $"Close the game ({Path.GetFileName(gameExecutable)}) before updating. "
                        + "The locale hook DLL may be locked while the game is running."
                );
            }
        }
        finally
        {
            foreach (var process in running)
                process.Dispose();
        }
    }

    private static string? FindExtractedFile(string extractDir, string fileName)
    {
        var direct = Path.Combine(extractDir, fileName);
        if (File.Exists(direct))
            return direct;

        return Directory
            .EnumerateFiles(extractDir, fileName, SearchOption.AllDirectories)
            .FirstOrDefault();
    }

    private static async Task<string> ComputeSha256Async(
        string path,
        CancellationToken cancellationToken
    )
    {
        await using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 81920,
            useAsync: true
        );
        var hash = await SHA256.HashDataAsync(stream, cancellationToken).ConfigureAwait(false);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static void WriteApplyScript(
        string scriptPath,
        int pid,
        string sourceExe,
        string sourceDll,
        string destExe,
        string destDll,
        string workRoot
    )
    {
        // Wait for the launcher to exit, copy both files, relaunch, then clean up.
        var sb = new StringBuilder();
        sb.AppendLine("@echo off");
        sb.AppendLine("setlocal");
        sb.AppendLine($":wait");
        sb.AppendLine($"tasklist /FI \"PID eq {pid}\" 2>NUL | find \"{pid}\" >NUL");
        sb.AppendLine("if not errorlevel 1 (");
        sb.AppendLine("  timeout /T 1 /NOBREAK >NUL");
        sb.AppendLine("  goto wait");
        sb.AppendLine(")");
        sb.AppendLine($"copy /Y \"{sourceExe}\" \"{destExe}\" >NUL");
        sb.AppendLine("if errorlevel 1 exit /B 1");
        sb.AppendLine($"copy /Y \"{sourceDll}\" \"{destDll}\" >NUL");
        sb.AppendLine("if errorlevel 1 exit /B 1");
        sb.AppendLine($"start \"\" \"{destExe}\"");
        sb.AppendLine($"rmdir /S /Q \"{workRoot}\"");
        File.WriteAllText(scriptPath, sb.ToString(), Encoding.ASCII);
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
            // Best-effort cleanup after a failed download/extract.
        }
    }
}

internal enum UpdateAvailability
{
    UpToDate,
    UpdateAvailable,
}

internal sealed record UpdateCheckResult(
    UpdateAvailability Availability,
    GitHubReleaseInfo Release,
    string Message
);
