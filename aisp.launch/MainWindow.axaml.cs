using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Layout;

namespace aisp.launch;

public partial class MainWindow : Window
{
    private readonly GameLauncher _gameLauncher;
    private readonly LauncherUpdater _updater;
    private bool _updateInProgress;

    public MainWindow()
    {
        InitializeComponent();

        Title = LaunchVersion.WindowTitle;

        _gameLauncher = new GameLauncher(LauncherBootstrap.Settings);
        _updater = new LauncherUpdater(LauncherBootstrap.Settings);

        EnvironmentComboBox.SelectedIndex = Math.Clamp(
            (int)LauncherBootstrap.Settings.SelectedEnvironment,
            0,
            2
        );
        BindOptions();
        AttachWebsitePane(_gameLauncher.Settings.WebsiteUrl);

        Opened += OnOpened;
    }

    private void BindOptions()
    {
        var settings = LauncherBootstrap.Settings;
        EnhancementsCheckBox.IsChecked = settings.UseEnhancements;
        ElectronHwAccelCheckBox.IsChecked = settings.ElectronHardwareAcceleration;
        UseDxvkCheckBox.IsChecked = settings.UseDxvk;
        UseDgVoodooCheckBox.IsChecked = settings.UseDgVoodoo;
        CheckUpdatesOnStartupCheckBox.IsChecked = settings.CheckForUpdatesOnStartup;

        EnhancementsCheckBox.IsCheckedChanged += OnOptionsChanged;
        ElectronHwAccelCheckBox.IsCheckedChanged += OnOptionsChanged;
        UseDxvkCheckBox.IsCheckedChanged += OnOptionsChanged;
        UseDgVoodooCheckBox.IsCheckedChanged += OnOptionsChanged;
        CheckUpdatesOnStartupCheckBox.IsCheckedChanged += OnOptionsChanged;
        UpdateEnhancementDependentOptions();
    }

    private async void OnOptionsChanged(object? sender, RoutedEventArgs e)
    {
        var settings = LauncherBootstrap.Settings;
        var wasDxvk = settings.UseDxvk;
        var wasDgVoodoo = settings.UseDgVoodoo;

        if (
            sender == UseDxvkCheckBox
            && UseDxvkCheckBox.IsChecked is true
            && UseDgVoodooCheckBox.IsChecked is true
        )
            SetCheckBoxWithoutNotify(UseDgVoodooCheckBox, false);
        if (
            sender == UseDgVoodooCheckBox
            && UseDgVoodooCheckBox.IsChecked is true
            && UseDxvkCheckBox.IsChecked is true
        )
            SetCheckBoxWithoutNotify(UseDxvkCheckBox, false);

        UpdateEnhancementDependentOptions();
        ApplyOptionsToSettings();
        settings.Save();

        if (!settings.UseEnhancements || (wasDxvk && !settings.UseDxvk))
            DxvkRuntime.RemoveInstalled();
        if (!settings.UseEnhancements || (wasDgVoodoo && !settings.UseDgVoodoo))
            DgVoodooRuntime.RemoveInstalled();

        if (settings.UseDxvk && !wasDxvk)
        {
            try
            {
                await EnsureDxvkWithUiAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                SetCheckBoxWithoutNotify(UseDxvkCheckBox, false);
                settings.UseDxvk = false;
                settings.Save();
                await ShowMessageAsync("DXVK setup failed", ex.Message).ConfigureAwait(true);
            }
        }
        else if (settings.UseDgVoodoo && !wasDgVoodoo)
        {
            try
            {
                await EnsureDgVoodooWithUiAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                SetCheckBoxWithoutNotify(UseDgVoodooCheckBox, false);
                settings.UseDgVoodoo = false;
                settings.Save();
                await ShowMessageAsync("dgVoodoo setup failed", ex.Message).ConfigureAwait(true);
            }
        }
    }

    private void SetCheckBoxWithoutNotify(CheckBox box, bool value)
    {
        box.IsCheckedChanged -= OnOptionsChanged;
        box.IsChecked = value;
        box.IsCheckedChanged += OnOptionsChanged;
    }

    private void UpdateEnhancementDependentOptions()
    {
        var enabled = EnhancementsCheckBox.IsChecked is true;
        ElectronHwAccelCheckBox.IsEnabled = enabled;
        UseDxvkCheckBox.IsEnabled = enabled;
        UseDgVoodooCheckBox.IsEnabled = enabled;
    }

    private void ApplyOptionsToSettings()
    {
        var settings = LauncherBootstrap.Settings;
        settings.UseEnhancements = EnhancementsCheckBox.IsChecked is true;
        settings.ElectronHardwareAcceleration = ElectronHwAccelCheckBox.IsChecked is true;
        settings.UseDxvk = UseDxvkCheckBox.IsChecked is true;
        settings.UseDgVoodoo = UseDgVoodooCheckBox.IsChecked is true;
        settings.CheckForUpdatesOnStartup = CheckUpdatesOnStartupCheckBox.IsChecked is true;
    }

    private void AttachWebsitePane(string websiteUrl)
    {
        // NativeWebView on this WinExe is WebView2. Wine has no usable host, and
        // constructing the control can take the process down; leave the pane empty.
        if (WineDetection.IsRunningOnWine)
            return;

        try
        {
            var webView = new NativeWebView();
            LauncherBootstrap.ConfigureWebViewEnvironment(webView);
            WebsiteHost.Content = webView;
            webView.Source = new Uri(websiteUrl);
        }
        catch
        {
            // Missing or broken WebView2: keep the empty pane.
        }
    }

    private async void OnOpened(object? sender, EventArgs e)
    {
        Opened -= OnOpened;

        try
        {
            await EnsureRuntimeDependenciesOnStartupAsync().ConfigureAwait(true);
        }
        catch (Exception ex)
        {
            await ShowMessageAsync(
                    "Runtime setup failed",
                    "Could not download Electron / Streamlink / FFmpeg.\n\n" + ex.Message
                )
                .ConfigureAwait(true);
        }

        await PromptDirectXIfMissingAsync(AppContext.BaseDirectory).ConfigureAwait(true);

        if (!LauncherBootstrap.Settings.CheckForUpdatesOnStartup)
            return;
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return;

        try
        {
            var result = await _updater.CheckForUpdateAsync().ConfigureAwait(true);
            if (result.Availability != UpdateAvailability.UpdateAvailable)
                return;

            await PromptAndApplyUpdateAsync(result, silentIfDeclined: true)
                .ConfigureAwait(true);
        }
        catch
        {
            // Startup update checks fail silently so offline use is not blocked.
        }
    }

    private async Task EnsureRuntimeDependenciesOnStartupAsync()
    {
        try
        {
            _ = RuntimeDataPaths.DetectPlatform();
        }
        catch (PlatformNotSupportedException)
        {
            return;
        }

        if (!RuntimeDependencyBootstrap.NeedsDownload())
        {
            // Still resolve paths for later use when everything is already present.
            await RuntimeDependencyBootstrap.EnsureAsync().ConfigureAwait(true);
            return;
        }

        var statusText = new TextBlock
        {
            Text = "Checking runtime dependencies…",
            TextWrapping = Avalonia.Media.TextWrapping.Wrap,
        };
        var progressBar = new ProgressBar
        {
            Minimum = 0,
            Maximum = 100,
            Value = 0,
            Height = 8,
        };
        var dialog = new Window
        {
            Title = "Downloading runtime",
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children = { statusText, progressBar },
            },
        };

        var status = new Progress<string>(message => statusText.Text = message);
        var downloadProgress = new Progress<double>(fraction =>
        {
            progressBar.Value = Math.Clamp(fraction * 100, 0, 100);
        });

        var ensureTask = RuntimeDependencyBootstrap.EnsureAsync(status, downloadProgress);
        var dialogTask = dialog.ShowDialog(this);

        try
        {
            await ensureTask.ConfigureAwait(true);
            dialog.Close();
            await dialogTask.ConfigureAwait(true);
        }
        catch
        {
            dialog.Close();
            try
            {
                await dialogTask.ConfigureAwait(true);
            }
            catch
            {
                // Dialog may already be closed.
            }

            throw;
        }
    }

    private async Task EnsureDxvkWithUiAsync()
    {
        if (DxvkRuntime.IsInstalled())
            return;

        var statusText = new TextBlock
        {
            Text = "Checking latest DXVK release…",
            TextWrapping = Avalonia.Media.TextWrapping.Wrap,
        };
        var progressBar = new ProgressBar
        {
            Minimum = 0,
            Maximum = 100,
            Value = 0,
            Height = 8,
        };
        var dialog = new Window
        {
            Title = "Downloading DXVK",
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children = { statusText, progressBar },
            },
        };

        var status = new Progress<string>(message => statusText.Text = message);
        var downloadProgress = new Progress<double>(fraction =>
        {
            progressBar.Value = Math.Clamp(fraction * 100, 0, 100);
        });

        StartGameButton.IsEnabled = false;
        OptionsButton.IsEnabled = false;
        using var http = new RuntimeHttpClient();
        var ensureTask = DxvkRuntime.EnsureInstalledAsync(http, status, downloadProgress);
        var dialogTask = dialog.ShowDialog(this);

        try
        {
            await ensureTask.ConfigureAwait(true);
            dialog.Close();
            await dialogTask.ConfigureAwait(true);
        }
        catch
        {
            dialog.Close();
            try
            {
                await dialogTask.ConfigureAwait(true);
            }
            catch
            {
                // Dialog may already be closed.
            }

            throw;
        }
        finally
        {
            StartGameButton.IsEnabled = !_updateInProgress;
            OptionsButton.IsEnabled = true;
        }
    }

    private async Task EnsureDgVoodooWithUiAsync()
    {
        if (DgVoodooRuntime.IsInstalled())
            return;

        var statusText = new TextBlock
        {
            Text = $"Checking dgVoodoo {DgVoodooRuntime.Version}…",
            TextWrapping = Avalonia.Media.TextWrapping.Wrap,
        };
        var progressBar = new ProgressBar
        {
            Minimum = 0,
            Maximum = 100,
            Value = 0,
            Height = 8,
        };
        var dialog = new Window
        {
            Title = "Downloading dgVoodoo",
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children = { statusText, progressBar },
            },
        };

        var status = new Progress<string>(message => statusText.Text = message);
        var downloadProgress = new Progress<double>(fraction =>
        {
            progressBar.Value = Math.Clamp(fraction * 100, 0, 100);
        });

        StartGameButton.IsEnabled = false;
        OptionsButton.IsEnabled = false;
        using var http = new RuntimeHttpClient();
        var ensureTask = DgVoodooRuntime.EnsureInstalledAsync(http, status, downloadProgress);
        var dialogTask = dialog.ShowDialog(this);

        try
        {
            await ensureTask.ConfigureAwait(true);
            dialog.Close();
            await dialogTask.ConfigureAwait(true);
        }
        catch
        {
            dialog.Close();
            try
            {
                await dialogTask.ConfigureAwait(true);
            }
            catch
            {
                // Dialog may already be closed.
            }

            throw;
        }
        finally
        {
            StartGameButton.IsEnabled = !_updateInProgress;
            OptionsButton.IsEnabled = true;
        }
    }

    private async void OnCheckUpdatesClick(object? sender, RoutedEventArgs e)
    {
        if (_updateInProgress)
            return;

        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            await ShowMessageAsync(
                    "Updates",
                    "Self-update is only available on the Windows win-x86 release."
                )
                .ConfigureAwait(true);
            return;
        }

        CheckUpdatesButton.IsEnabled = false;
        try
        {
            var result = await _updater.CheckForUpdateAsync().ConfigureAwait(true);
            if (result.Availability == UpdateAvailability.UpToDate)
            {
                await ShowMessageAsync("Updates", result.Message).ConfigureAwait(true);
                return;
            }

            await PromptAndApplyUpdateAsync(result, silentIfDeclined: false)
                .ConfigureAwait(true);
        }
        catch (Exception ex)
        {
            await ShowMessageAsync("Update check failed", ex.Message).ConfigureAwait(true);
        }
        finally
        {
            CheckUpdatesButton.IsEnabled = !_updateInProgress;
        }
    }

    private async Task PromptAndApplyUpdateAsync(
        UpdateCheckResult result,
        bool silentIfDeclined
    )
    {
        var accept = await ShowConfirmAsync(
                "Update available",
                $"{result.Message}\n\nDownload and install the update now?\n"
                    + "The launcher will restart after the update."
            )
            .ConfigureAwait(true);

        if (!accept)
        {
            if (!silentIfDeclined)
                await ShowMessageAsync("Updates", "Update skipped.").ConfigureAwait(true);
            return;
        }

        await DownloadAndApplyAsync(result.Release).ConfigureAwait(true);
    }

    private async Task DownloadAndApplyAsync(GitHubReleaseInfo release)
    {
        _updateInProgress = true;
        CheckUpdatesButton.IsEnabled = false;
        StartGameButton.IsEnabled = false;

        var statusText = new TextBlock
        {
            Text = "Preparing…",
            TextWrapping = Avalonia.Media.TextWrapping.Wrap,
        };
        var progressBar = new ProgressBar
        {
            Minimum = 0,
            Maximum = 100,
            Value = 0,
            IsIndeterminate = false,
            Height = 8,
        };
        var dialog = new Window
        {
            Title = "Updating",
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children = { statusText, progressBar },
            },
        };

        var status = new Progress<string>(message =>
        {
            statusText.Text = message;
        });
        var downloadProgress = new Progress<double>(fraction =>
        {
            progressBar.Value = Math.Clamp(fraction * 100, 0, 100);
        });

        var applyTask = _updater.ApplyUpdateAsync(release, status, downloadProgress);
        var dialogTask = dialog.ShowDialog(this);

        try
        {
            await applyTask.ConfigureAwait(true);
            dialog.Close();
            await dialogTask.ConfigureAwait(true);
            ShutdownLauncher();
        }
        catch (Exception ex)
        {
            dialog.Close();
            try
            {
                await dialogTask.ConfigureAwait(true);
            }
            catch
            {
                // Dialog may already be closed.
            }

            _updateInProgress = false;
            CheckUpdatesButton.IsEnabled = true;
            StartGameButton.IsEnabled = true;
            await ShowMessageAsync("Update failed", ex.Message).ConfigureAwait(true);
        }
    }

    private static void ShutdownLauncher()
    {
        if (
            Application.Current?.ApplicationLifetime
            is IClassicDesktopStyleApplicationLifetime desktop
        )
            desktop.Shutdown();
    }

    private async void OnStartGameClick(object? sender, RoutedEventArgs e)
    {
        if (_updateInProgress)
            return;

        var environment = (GameEnvironment)EnvironmentComboBox.SelectedIndex;
        LauncherBootstrap.Settings.SelectedEnvironment = environment;
        ApplyOptionsToSettings();
        LauncherBootstrap.Settings.Save();

        if (
            LauncherBootstrap.Settings.UseEnhancements
            && LauncherBootstrap.Settings.UseDxvk
            && !DxvkRuntime.IsInstalled()
        )
        {
            try
            {
                await EnsureDxvkWithUiAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                await ShowMessageAsync("DXVK setup failed", ex.Message).ConfigureAwait(true);
                return;
            }
        }
        else if (
            LauncherBootstrap.Settings.UseEnhancements
            && LauncherBootstrap.Settings.UseDgVoodoo
            && !DgVoodooRuntime.IsInstalled()
        )
        {
            try
            {
                await EnsureDgVoodooWithUiAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                await ShowMessageAsync("dgVoodoo setup failed", ex.Message).ConfigureAwait(true);
                return;
            }
        }

        await PromptDirectXIfMissingAsync(AppContext.BaseDirectory).ConfigureAwait(true);

        var result = _gameLauncher.TryLaunch(environment);
        if (result.Succeeded)
        {
            ShutdownLauncher();
            return;
        }
        await ShowMessageAsync("Unable to start game", $"{result.Message}\n\n{result.Details}");
    }

    private async Task PromptDirectXIfMissingAsync(string? gameDirectory)
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            return;
        if (DirectXDecember2006.IsInstalled(gameDirectory))
            return;

        if (
            await ShowOkIgnoreAsync(
                    DirectXDecember2006.DisplayName,
                    DirectXDecember2006.MissingMessage
                )
                .ConfigureAwait(true)
        )
            DirectXDecember2006.OpenDownloadPage();
    }

    private async Task ShowMessageAsync(string title, string message)
    {
        var okButton = new Button
        {
            Content = "OK",
            HorizontalAlignment = HorizontalAlignment.Right,
            MinWidth = 80,
        };
        var dialog = new Window
        {
            Title = title,
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children =
                {
                    new TextBlock
                    {
                        Text = message,
                        TextWrapping = Avalonia.Media.TextWrapping.Wrap,
                    },
                    okButton,
                },
            },
        };

        okButton.Click += (_, _) => dialog.Close();
        await dialog.ShowDialog(this);
    }

    private Task<bool> ShowConfirmAsync(string title, string message) =>
        ShowChoiceAsync(title, message, acceptLabel: "Yes", cancelLabel: "No");

    private Task<bool> ShowOkIgnoreAsync(string title, string message) =>
        ShowChoiceAsync(title, message, acceptLabel: "OK", cancelLabel: "Ignore");

    private async Task<bool> ShowChoiceAsync(
        string title,
        string message,
        string acceptLabel,
        string cancelLabel
    )
    {
        var yesButton = new Button
        {
            Content = acceptLabel,
            MinWidth = 80,
            IsDefault = true,
        };
        var noButton = new Button
        {
            Content = cancelLabel,
            MinWidth = 80,
            IsCancel = true,
        };
        var accepted = false;
        var dialog = new Window
        {
            Title = title,
            Width = 420,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Spacing = 16,
                Children =
                {
                    new TextBlock
                    {
                        Text = message,
                        TextWrapping = Avalonia.Media.TextWrapping.Wrap,
                    },
                    new StackPanel
                    {
                        Orientation = Orientation.Horizontal,
                        Spacing = 8,
                        HorizontalAlignment = HorizontalAlignment.Right,
                        Children = { noButton, yesButton },
                    },
                },
            },
        };

        yesButton.Click += (_, _) =>
        {
            accepted = true;
            dialog.Close();
        };
        noButton.Click += (_, _) => dialog.Close();
        await dialog.ShowDialog(this);
        return accepted;
    }
}
