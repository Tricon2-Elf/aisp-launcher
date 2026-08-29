using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace aisp.launch;

public partial class App : Application
{
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
            mainWindow.Closing += (_, _) => LauncherServices.Stop();
            desktop.Exit += (_, _) =>
            {
                LauncherServices.Stop();
                if (OperatingSystem.IsWindows())
                    Environment.Exit(0);
            };
            _ = LauncherServices.InitializeAsync(mainWindow);
        }

        base.OnFrameworkInitializationCompleted();
    }
}
