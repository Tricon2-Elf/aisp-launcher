using System.Text.Json;
using System.Text.Json.Serialization;

namespace aisp.launch;

public sealed class LauncherSettings
{
    public static string DefaultWebsiteUrl => LaunchDefaults.WebsiteUrl;

    public const string DefaultGameExecutable = "ai sp@ce.exe";

    public static string DefaultGitHubRepo => LaunchDefaults.GitHubRepo;

    /// <summary>
    /// CalVer release number of the launcher that last wrote this file (YYYY.MM.DD.N, or dev).
    /// </summary>
    public string Version { get; set; } = LaunchVersion.Display;

    public string WebsiteUrl { get; set; } = DefaultWebsiteUrl;

    public string GameExecutable { get; set; } = DefaultGameExecutable;

    /// <summary>
    /// When true on Windows, launcher injects aisp.hook.dll into the game process.
    /// </summary>
    public bool UseEnhancements { get; set; } = true;

    /// <summary>
    /// Reads the old launcher.settings.json key so existing files keep their choice.
    /// </summary>
    [JsonPropertyName("useLocaleReplacer")]
    public bool? UseLocaleReplacer
    {
        get => null;
        set
        {
            if (value is bool flag)
                UseEnhancements = flag;
        }
    }

    /// <summary>
    /// GitHub owner/repo that hosts launcher Releases (e.g. Tricon2-Elf/aisp-launcher).
    /// </summary>
    public string GitHubRepo { get; set; } = DefaultGitHubRepo;

    /// <summary>
    /// When true, check GitHub Releases for a newer launcher on startup.
    /// </summary>
    public bool CheckForUpdatesOnStartup { get; set; } = true;

    /// <summary>
    /// When true, the in-game Electron host uses GPU acceleration. The hook reads
    /// AISP_ELECTRON_HW_ACCEL (set from this value at launch). Defaults on for
    /// native Windows and off under Wine.
    /// </summary>
    public bool ElectronHardwareAcceleration { get; set; } = !WineDetection.IsRunningOnWine;

    /// <summary>
    /// When true, the launcher keeps the latest DXVK 32-bit d3d9.dll under
    /// aisp.launch.data/dxvk and copies it next to the game on launch.
    /// </summary>
    public bool UseDxvk { get; set; }

    /// <summary>
    /// When true, the launcher keeps pinned dgVoodoo2 (v2.87.5) under
    /// aisp.launch.data/dgVoodoo and copies D3D9.dll plus dgVoodoo.conf next to
    /// the game on each launch. Mutually exclusive with UseDxvk.
    /// </summary>
    public bool UseDgVoodoo { get; set; }

    public GameEnvironment SelectedEnvironment { get; set; } = GameEnvironment.Stable;

    public Dictionary<string, EnvironmentSettings> Environments { get; set; } =
        EnvironmentCatalog.CreateDefaults();

    public static string GetPath() =>
        Path.Combine(AppContext.BaseDirectory, "launcher.settings.json");

    public static LauncherSettings LoadOrCreate()
    {
        var path = GetPath();
        if (!File.Exists(path) || !FileHasVersion(File.ReadAllText(path)))
        {
            var defaults = new LauncherSettings();
            defaults.Save(path);
            return defaults;
        }

        var json = File.ReadAllText(path);
        var settings = JsonSerializer.Deserialize<LauncherSettings>(json, JsonOptions)
            ?? new LauncherSettings();
        if (!string.Equals(settings.Version, LaunchVersion.Display, StringComparison.Ordinal)
            || !FileHasProperty(json, "electronHardwareAcceleration")
            || !FileHasProperty(json, "useDxvk")
            || !FileHasProperty(json, "useDgVoodoo"))
        {
            settings.Version = LaunchVersion.Display;
            settings.Save(path);
        }

        return settings;
    }

    public static LauncherSettings Load(string path)
    {
        var json = File.ReadAllText(path);
        return JsonSerializer.Deserialize<LauncherSettings>(json, JsonOptions)
            ?? new LauncherSettings();
    }

    private static bool FileHasVersion(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.ValueKind == JsonValueKind.Object
                && doc.RootElement.TryGetProperty("version", out var version)
                && version.ValueKind == JsonValueKind.String
                && !string.IsNullOrWhiteSpace(version.GetString());
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static bool FileHasProperty(string json, string name)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.ValueKind == JsonValueKind.Object
                && doc.RootElement.TryGetProperty(name, out _);
        }
        catch (JsonException)
        {
            return false;
        }
    }

    public void Save() => Save(GetPath());

    public void Save(string path)
    {
        Version = LaunchVersion.Display;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(this, JsonOptions));
    }

    public EnvironmentSettings GetEnvironment(GameEnvironment environment)
    {
        var fallback = Environments.TryGetValue(environment.ToString(), out var settings)
            ? settings
            : new EnvironmentSettings();
        return EnvironmentCatalog.Resolve(environment, fallback);
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };
}

public sealed class EnvironmentSettings
{
    public string AuthHost { get; set; } = "aisp.moe";

    public ushort AuthPort { get; set; } = 50050;

    public int BypassNicoLogin { get; set; } = 1;

    public string DownloadHost { get; set; } = "aisp.moe";

    public string DownloadPath { get; set; } = "ai-sp/dev/download.php";

    public string UploadHost { get; set; } = "aisp.moe";

    public string UploadPath { get; set; } = "ai-sp/dev/upload.php";
}

internal sealed class EnvironmentSettingsOverride
{
    public string? AuthHost { get; set; }

    public ushort? AuthPort { get; set; }

    public int? BypassNicoLogin { get; set; }

    public string? DownloadHost { get; set; }

    public string? DownloadPath { get; set; }

    public string? UploadHost { get; set; }

    public string? UploadPath { get; set; }

    public EnvironmentSettings Apply(EnvironmentSettings fallback) =>
        new()
        {
            AuthHost = AuthHost ?? fallback.AuthHost,
            AuthPort = AuthPort ?? fallback.AuthPort,
            BypassNicoLogin = BypassNicoLogin ?? fallback.BypassNicoLogin,
            DownloadHost = DownloadHost ?? fallback.DownloadHost,
            DownloadPath = DownloadPath ?? fallback.DownloadPath,
            UploadHost = UploadHost ?? fallback.UploadHost,
            UploadPath = UploadPath ?? fallback.UploadPath,
        };
}
