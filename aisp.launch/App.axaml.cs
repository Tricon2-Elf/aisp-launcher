using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace aisp.launch;

public partial class App : Application
{
    private LocalHttpServer? _localHttpServer;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var mainWindow = new MainWindow();
            desktop.MainWindow = mainWindow;
            desktop.Exit += OnDesktopExit;
            _ = InitializeLocalServicesAsync(mainWindow);
        }

        base.OnFrameworkInitializationCompleted();
    }

    private async Task InitializeLocalServicesAsync(MainWindow mainWindow)
    {
        var hostsResult = HostsFile.TryApply();
        if (!hostsResult.Succeeded)
        {
            await mainWindow.ShowMessageAsync(
                "Hosts file",
                $"{hostsResult.Message}\n\n{hostsResult.Details}"
            );
        }

        _localHttpServer = new LocalHttpServer();
        var serverResult = await _localHttpServer.StartAsync();
        if (!serverResult.Succeeded)
        {
            await mainWindow.ShowMessageAsync(
                "Local HTTP server",
                $"{serverResult.Message}\n\n{serverResult.Details}"
            );
        }
    }

    private void OnDesktopExit(object? sender, ControlledApplicationLifetimeExitEventArgs e)
    {
        if (_localHttpServer is null)
            return;

        _localHttpServer.StopAsync().GetAwaiter().GetResult();
        _localHttpServer = null;
    }
}
