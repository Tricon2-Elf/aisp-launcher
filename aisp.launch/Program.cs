using System;
using Avalonia;

namespace aisp.launch;

class Program
{
    // Initialization code. Don't use any Avalonia, third-party APIs or any
    // SynchronizationContext-reliant code before AppMain is called: things aren't initialized
    // yet and stuff might break.
    [STAThread]
    public static void Main(string[] args)
    {
        LauncherBootstrap.Initialize();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    // Avalonia configuration, don't remove; also used by visual designer.
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder
            .Configure<App>()
            .UsePlatformDetect()
            // Wine has no working ANGLE/D3D11 for Avalonia's default Win32 renderer: every
            // surface paints black. Software rendering draws the same UI.
            .With(
                new Win32PlatformOptions
                {
                    RenderingMode = WineDetection.IsRunningOnWine
                        ? [Win32RenderingMode.Software]
                        : [Win32RenderingMode.AngleEgl, Win32RenderingMode.Software],
                }
            )
#if DEBUG
            .WithDeveloperTools()
#endif
            .WithInterFont()
            .LogToTrace();
}
