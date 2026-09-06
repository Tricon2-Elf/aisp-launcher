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
            return !ElectronRuntime.IsInstalled() || MediaToolsResolver.TryFindExisting() is null;
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
}
