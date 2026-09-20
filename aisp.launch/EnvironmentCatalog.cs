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

            using var http = new RuntimeHttpClient();
            var json = await FetchRepoFileAsync(http, repo, cancellationToken)
                .ConfigureAwait(false);
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

    private static async Task<string> FetchRepoFileAsync(
        RuntimeHttpClient http,
        string repo,
        CancellationToken cancellationToken
    )
    {
        var apiUrl = $"https://api.github.com/repos/{repo}/contents/{FileName}";
        var payload = await http.GetStringAsync(apiUrl, cancellationToken).ConfigureAwait(false);
        using var doc = JsonDocument.Parse(payload);
        var root = doc.RootElement;

        if (
            root.TryGetProperty("encoding", out var encoding)
            && encoding.ValueKind == JsonValueKind.String
            && encoding.GetString() is "base64"
            && root.TryGetProperty("content", out var content)
            && content.ValueKind == JsonValueKind.String
            && content.GetString() is { Length: > 0 } base64
        )
        {
            var compact = base64.Replace("\n", "", StringComparison.Ordinal);
            return System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(compact));
        }

        if (
            root.TryGetProperty("download_url", out var download)
            && download.GetString() is { Length: > 0 } downloadUrl
        )
        {
            return await http.GetStringAsync(downloadUrl, cancellationToken).ConfigureAwait(false);
        }

        throw new InvalidOperationException($"GitHub did not return {FileName}.");
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
