namespace aisp.launch;

internal static class LauncherServices
{
    private static LocalHttpServer? _httpServer;
    private static readonly Lock Sync = new();
    private static int _stopped;

    public static async Task InitializeAsync(MainWindow mainWindow)
    {
        var hostsResult = HostsFile.TryApply();
        if (!hostsResult.Succeeded)
        {
            await mainWindow.ShowMessageAsync(
                "Hosts file",
                $"{hostsResult.Message}\n\n{hostsResult.Details}"
            );
        }

        TridentCache.TryClearLegacyPlayerCache();

        LocalHttpServer httpServer;
        lock (Sync)
        {
            _httpServer ??= new LocalHttpServer();
            httpServer = _httpServer;
        }

        var serverResult = await httpServer.StartAsync();
        if (!serverResult.Succeeded)
        {
            await mainWindow.ShowMessageAsync(
                "Local HTTP server",
                $"{serverResult.Message}\n\n{serverResult.Details}"
            );
        }
    }

    public static void Stop()
    {
        if (Interlocked.Exchange(ref _stopped, 1) == 1)
            return;

        LocalHttpServer? httpServer;
        lock (Sync)
        {
            httpServer = _httpServer;
            _httpServer = null;
        }

        if (httpServer is null)
            return;

        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            httpServer.StopAsync(timeout.Token).GetAwaiter().GetResult();
        }
        catch
        {
            // Best-effort shutdown during app exit.
        }
    }
}
