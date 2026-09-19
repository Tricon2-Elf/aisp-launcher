namespace aisp.launch;

internal sealed class RuntimeDependencies
{
    public required string ElectronPath { get; init; }
    public required MediaTools MediaTools { get; init; }
}

internal static class RuntimeDependencyBootstrap
{
    public static RuntimeDependencies? Current { get; private set; }

    public static bool NeedsDownload()
    {
        try
        {
            return !ElectronRuntime.IsPinnedVersionInstalled()
                || MediaToolsResolver.TryFindExisting() is null
                || MediaToolsResolver.TryFindYtdlp() is null
                || DxvkRuntime.NeedsDownload()
                || DgVoodooRuntime.NeedsDownload();
        }
        catch (PlatformNotSupportedException)
        {
            return false;
        }
    }

    public static async Task<RuntimeDependencies> EnsureAsync(
        IProgress<string>? status = null,
        IProgress<double>? downloadProgress = null,
        CancellationToken cancellationToken = default
    )
    {
        Directory.CreateDirectory(RuntimeDataPaths.DataRoot);

        using var http = new RuntimeHttpClient();

        await ElectronRuntime
            .EnsureInstalledAsync(http, status, downloadProgress, cancellationToken)
            .ConfigureAwait(false);

        // Reset progress between large downloads so the UI bar starts fresh.
        downloadProgress?.Report(0);

        var media = await MediaToolsResolver
            .EnsureInstalledAsync(http, status, downloadProgress, cancellationToken)
            .ConfigureAwait(false);

        if (LauncherBootstrap.Settings.UseEnhancements && LauncherBootstrap.Settings.UseDxvk)
        {
            downloadProgress?.Report(0);
            await DxvkRuntime
                .EnsureInstalledAsync(http, status, downloadProgress, cancellationToken)
                .ConfigureAwait(false);
        }
        else if (LauncherBootstrap.Settings.UseEnhancements && LauncherBootstrap.Settings.UseDgVoodoo)
        {
            downloadProgress?.Report(0);
            await DgVoodooRuntime
                .EnsureInstalledAsync(http, status, downloadProgress, cancellationToken)
                .ConfigureAwait(false);
        }

        var result = new RuntimeDependencies
        {
            ElectronPath = ElectronRuntime.ElectronExecutablePath,
            MediaTools = media,
        };
        Current = result;
        status?.Report("Runtime dependencies ready.");
        return result;
    }

    /// <summary>
    /// Sets AISP_* tool variables on this process so a CreateProcess-injected game inherits
    /// absolute paths into aisp.launch.data.
    /// </summary>
    public static void ApplyHookEnvironment()
    {
        try
        {
            // Under Wine the hook execs the native Linux Electron itself (AISP_ELECTRON_NATIVE,
            // a DOS path it converts); on Windows it starts electron.exe (AISP_ELECTRON).
            if (File.Exists(ElectronRuntime.ElectronExecutablePath))
            {
                Environment.SetEnvironmentVariable(
                    WineDetection.IsRunningOnWine ? "AISP_ELECTRON_NATIVE" : "AISP_ELECTRON",
                    Path.GetFullPath(ElectronRuntime.ElectronExecutablePath)
                );
            }

            var media = Current?.MediaTools ?? MediaToolsResolver.TryFindExisting();
            if (media is { } tools)
            {
                Environment.SetEnvironmentVariable(
                    "AISP_STREAMLINK",
                    Path.GetFullPath(tools.Streamlink)
                );
                Environment.SetEnvironmentVariable("AISP_FFMPEG", Path.GetFullPath(tools.Ffmpeg));
            }

            var ytdlp = MediaToolsResolver.TryFindYtdlp();
            if (ytdlp is not null)
                Environment.SetEnvironmentVariable("AISP_YTDLP", Path.GetFullPath(ytdlp));

            Environment.SetEnvironmentVariable(
                "AISP_ELECTRON_HW_ACCEL",
                LauncherBootstrap.Settings.ElectronHardwareAcceleration ? "1" : "0"
            );
        }
        catch (PlatformNotSupportedException)
        {
            // Hook injection is Windows-only; nothing to export.
        }
    }
}
