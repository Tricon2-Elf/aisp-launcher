using System.Globalization;
using System.Reflection;
using System.Text.Json;

namespace aisp.launch;

/// <summary>
/// Loads environment hosts from environments.json in the GitHub repo root.
/// The same file is embedded for offline first-run defaults. Optional
/// aisp.launch.data/environments-override.json can add or override entries.
/// The combo list is those two sources only.
/// </summary>
internal static class EnvironmentCatalog
{
    public const string FileName = "environments.json";
    public const string OverrideFileName = "environments-override.json";

    public static string OverridePath =>
        Path.Combine(RuntimeDataPaths.DataRoot, OverrideFileName);

    private const string EmbeddedName = "aisp.launch.environments.json";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static Dictionary<string, EnvironmentSettings> CreateDefaults() =>
        Parse(ReadEmbedded()) ?? NewMap();

    public static async Task<bool> TryRefreshAsync(
        LauncherSettings settings,
        CancellationToken cancellationToken = default
    )
    {
        try
        {
            var repo = settings.GitHubRepo.Trim();
            if (string.IsNullOrWhiteSpace(repo) || !repo.Contains('/'))
                return false;

            var url = $"https://raw.githubusercontent.com/{repo}/HEAD/{FileName}";
            using var http = new RuntimeHttpClient();
            var json = await http.GetStringAsync(url, cancellationToken).ConfigureAwait(false);
            var parsed = Parse(json);
            if (parsed is null || parsed.Count == 0)
                return false;

            settings.Environments = parsed;
            settings.Save();
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static void RetainOfficialKeys(LauncherSettings settings)
    {
        var official = CreateDefaults();
        var next = NewMap();
        foreach (var (key, value) in official)
        {
            next[key] = settings.Environments.TryGetValue(key, out var cached)
                ? cached
                : value;
        }

        settings.Environments = next;
    }

    public static IReadOnlyList<string> ListVisible(LauncherSettings settings)
    {
        var names = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var key in settings.Environments.Keys)
        {
            if (!seen.Add(key))
                continue;
            names.Add(key);
        }

        foreach (var key in TryReadOverrides().Keys)
        {
            if (!seen.Add(key))
                continue;
            names.Add(key);
        }

        return names;
    }

    public static string DisplayName(string key)
    {
        if (string.IsNullOrWhiteSpace(key))
            return key;
        var text = key.Trim();
        return char.ToUpper(text[0], CultureInfo.InvariantCulture) + text[1..];
    }

    public static EnvironmentSettings Resolve(string environment, EnvironmentSettings fallback)
    {
        var overrides = TryReadOverrides();
        if (overrides.TryGetValue(environment, out var overlay))
            return overlay.Apply(fallback);

        return fallback;
    }

    internal static Dictionary<string, EnvironmentSettings>? Parse(string json)
    {
        Dictionary<string, EnvironmentSettings>? raw;
        try
        {
            raw = JsonSerializer.Deserialize<Dictionary<string, EnvironmentSettings>>(
                json,
                JsonOptions
            );
        }
        catch (JsonException)
        {
            return null;
        }

        if (raw is null || raw.Count == 0)
            return null;

        var mapped = NewMap();
        foreach (var (key, value) in raw)
        {
            if (string.IsNullOrWhiteSpace(key) || value is null || !IsUsable(value))
                continue;
            mapped[key.Trim()] = value;
        }

        return mapped.Count > 0 ? mapped : null;
    }

    private static Dictionary<string, EnvironmentSettingsOverride> TryReadOverrides()
    {
        var mapped = new Dictionary<string, EnvironmentSettingsOverride>(
            StringComparer.OrdinalIgnoreCase
        );
        if (!File.Exists(OverridePath))
            return mapped;

        try
        {
            var json = File.ReadAllText(OverridePath);
            var raw = JsonSerializer.Deserialize<
                Dictionary<string, EnvironmentSettingsOverride>
            >(json, JsonOptions);
            if (raw is null)
                return mapped;

            foreach (var (key, value) in raw)
            {
                if (string.IsNullOrWhiteSpace(key) || value is null)
                    continue;
                mapped[key.Trim()] = value;
            }
        }
        catch (Exception ex) when (ex is JsonException or IOException)
        {
            return mapped;
        }

        return mapped;
    }

    private static Dictionary<string, EnvironmentSettings> NewMap() =>
        new(StringComparer.OrdinalIgnoreCase);

    private static bool IsUsable(EnvironmentSettings settings) =>
        !string.IsNullOrWhiteSpace(settings.AuthHost)
        && !string.IsNullOrWhiteSpace(settings.DownloadHost)
        && !string.IsNullOrWhiteSpace(settings.DownloadPath)
        && !string.IsNullOrWhiteSpace(settings.UploadHost)
        && !string.IsNullOrWhiteSpace(settings.UploadPath);

    private static string ReadEmbedded()
    {
        var assembly = Assembly.GetExecutingAssembly();
        using var stream =
            assembly.GetManifestResourceStream(EmbeddedName)
            ?? throw new InvalidOperationException(
                $"Embedded environment catalog '{EmbeddedName}' was not found."
            );
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }
}
