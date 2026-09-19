using System.Reflection;
using System.Text.Json;

namespace aisp.launch;

/// <summary>
/// Loads Stable/Dev/Local hosts from environments.json in the GitHub repo root.
/// The same file is embedded for offline first-run defaults. Optional
/// aisp.launch.data/environments-override.json wins per environment / field.
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
        Parse(ReadEmbedded()) ?? new Dictionary<string, EnvironmentSettings>();

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

            foreach (var (key, value) in parsed)
                settings.Environments[key] = value;

            settings.Save();
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static EnvironmentSettings Resolve(
        GameEnvironment environment,
        EnvironmentSettings fallback
    )
    {
        var overrides = TryReadOverrides();
        if (
            overrides is not null
            && overrides.TryGetValue(environment.ToString(), out var overlay)
        )
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

        var mapped = new Dictionary<string, EnvironmentSettings>(StringComparer.Ordinal);
        foreach (var environment in Enum.GetValues<GameEnvironment>())
        {
            var match = raw.FirstOrDefault(entry =>
                entry.Key.Equals(environment.ToString(), StringComparison.OrdinalIgnoreCase)
            );
            if (match.Value is null || !IsUsable(match.Value))
                continue;
            mapped[environment.ToString()] = match.Value;
        }

        return mapped.Count > 0 ? mapped : null;
    }

    private static Dictionary<string, EnvironmentSettingsOverride>? TryReadOverrides()
    {
        if (!File.Exists(OverridePath))
            return null;

        try
        {
            var json = File.ReadAllText(OverridePath);
            var raw = JsonSerializer.Deserialize<
                Dictionary<string, EnvironmentSettingsOverride>
            >(json, JsonOptions);
            if (raw is null || raw.Count == 0)
                return null;

            var mapped = new Dictionary<string, EnvironmentSettingsOverride>(
                StringComparer.Ordinal
            );
            foreach (var environment in Enum.GetValues<GameEnvironment>())
            {
                var match = raw.FirstOrDefault(entry =>
                    entry.Key.Equals(environment.ToString(), StringComparison.OrdinalIgnoreCase)
                );
                if (match.Value is null)
                    continue;
                mapped[environment.ToString()] = match.Value;
            }

            return mapped.Count > 0 ? mapped : null;
        }
        catch (Exception ex) when (ex is JsonException or IOException)
        {
            return null;
        }
    }

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
