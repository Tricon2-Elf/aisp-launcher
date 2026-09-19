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

    public GameEnvironment SelectedEnvironment { get; set; } = GameEnvironment.Stable;

    public Dictionary<string, EnvironmentSettings> Environments { get; set; } =
        new()
        {
            [nameof(GameEnvironment.Stable)] = new()
            {
                AuthHost = "aisp.moe",
                DownloadHost = "game.aisp.moe",
                DownloadPath = "ai-sp/download.php",
                UploadHost = "game.aisp.moe",
                UploadPath = "ai-sp/upload.php",
            },
            [nameof(GameEnvironment.Dev)] = new()
            {
                AuthHost = "game.aisp.moe",
                DownloadHost = "game.aisp.moe",
                DownloadPath = "ai-sp/download.php",
                UploadHost = "game.aisp.moe",
                UploadPath = "ai-sp/upload.php",
            },
            [nameof(GameEnvironment.Local)] = new()
            {
                AuthHost = "127.0.0.1",
                DownloadHost = "127.0.0.1",
                DownloadPath = "ai-sp/download.php",
                UploadHost = "127.0.0.1",
                UploadPath = "ai-sp/upload.php",
            },
        };

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
            || !FileHasProperty(json, "electronHardwareAcceleration"))
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

    public EnvironmentSettings GetEnvironment(GameEnvironment environment) =>
        Environments.TryGetValue(environment.ToString(), out var settings)
            ? settings
            : new EnvironmentSettings();

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
