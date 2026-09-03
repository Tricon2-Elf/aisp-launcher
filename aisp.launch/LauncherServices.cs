namespace aisp.launch;

internal static class LauncherServices
{
    private static LocalHttpServer? _httpServer;
    private static TwitchBridge? _twitchBridge;
    private static readonly Lock Sync = new();
    private static int _stopped;

    public static async Task InitializeAsync(MainWindow mainWindow)
    {
        TempCleanup.Run();

        TridentCache.TryClearLegacyPlayerCache();

        var settings = LauncherBootstrap.Settings;
        TwitchBridge? twitchBridge = null;
        if (!string.IsNullOrWhiteSpace(settings.TwitchChannel))
        {
            try
            {
                var tools = await MediaToolsResolver.ResolveAsync(settings.ToolsDirectory);
                twitchBridge = new TwitchBridge(
                    settings.TwitchChannel.Trim(),
                    tools,
                    settings.TwitchQuality,
                    settings.TwitchSegmentSeconds,
                    settings.TwitchRetainedSegments,
                    settings.TwitchBufferSegments
                );
                twitchBridge.Start();
                _twitchBridge = twitchBridge;
            }
            catch (Exception ex)
            {
                await mainWindow.ShowMessageAsync(
                    "Twitch bridge",
                    ex.Message
                );
            }
        }

        LocalHttpServer httpServer;
        lock (Sync)
        {
            _httpServer ??= new LocalHttpServer(twitchBridge);
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
        TwitchBridge? twitchBridge;
        lock (Sync)
        {
            httpServer = _httpServer;
            twitchBridge = _twitchBridge;
            _httpServer = null;
            _twitchBridge = null;
        }

        try
        {
            twitchBridge?.Dispose();
        }
        catch
        {
            // Best-effort shutdown.
        }

        TempCleanup.Run();

        // Do not wait for Kestrel. StopAsync/DisposeAsync can deadlock the
        // Avalonia UI thread via SynchronizationContext.
        _ = httpServer;
    }

    public static void ExitProcess()
    {
        Stop();
        Environment.Exit(0);
    }
}
