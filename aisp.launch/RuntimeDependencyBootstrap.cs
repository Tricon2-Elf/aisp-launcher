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
            return !ElectronRuntime.IsInstalled()
                || MediaToolsResolver.TryFindExisting() is null
                || MediaToolsResolver.TryFindYtdlp() is null;
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
            if (File.Exists(ElectronRuntime.ElectronExecutablePath))
            {
                Environment.SetEnvironmentVariable(
                    "AISP_ELECTRON",
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
        }
        catch (PlatformNotSupportedException)
        {
            // Hook injection is Windows-only; nothing to export.
        }
    }
}
