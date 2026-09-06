using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace aisp.launch;

internal sealed class GitHubReleaseClient(HttpClient httpClient)
{
    private const string ZipAssetPattern = @"^aisp\.launch-.+-win-x86\.zip$";
    private static readonly Regex ZipAssetRegex = new(
        ZipAssetPattern,
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled
    );

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    public GitHubReleaseClient()
        : this(CreateDefaultHttpClient()) { }

    public async Task<GitHubReleaseInfo> GetLatestReleaseAsync(
        string githubRepo,
        CancellationToken cancellationToken = default
    )
    {
        if (string.IsNullOrWhiteSpace(githubRepo) || !githubRepo.Contains('/'))
            throw new ArgumentException(
                "GitHub repo must be in the form 'owner/repo'.",
                nameof(githubRepo)
            );

        var url = $"https://api.github.com/repos/{githubRepo.Trim()}/releases/latest";
        using var response = await httpClient.GetAsync(url, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();

        await using var stream = await response
            .Content.ReadAsStreamAsync(cancellationToken)
            .ConfigureAwait(false);
        var release =
            await JsonSerializer
                .DeserializeAsync<GitHubApiRelease>(stream, JsonOptions, cancellationToken)
                .ConfigureAwait(false)
            ?? throw new InvalidOperationException("GitHub returned an empty release payload.");

        var version = ParseVersionFromTag(release.TagName);
        var zipAsset =
            release.Assets.FirstOrDefault(a => ZipAssetRegex.IsMatch(a.Name))
            ?? throw new InvalidOperationException(
                "Latest release has no aisp.launch-*-win-x86.zip asset."
            );

        var sha256 =
            TryParseDigestSha256(zipAsset.Digest)
            ?? await TryFetchSha256FromSumsAsync(
                    release.Assets,
                    zipAsset.Name,
                    cancellationToken
                )
                .ConfigureAwait(false);

        return new GitHubReleaseInfo(
            version,
            release.TagName,
            zipAsset.Name,
            zipAsset.BrowserDownloadUrl,
            sha256,
            zipAsset.Size
        );
    }

    public async Task DownloadToFileAsync(
        string url,
        string destinationPath,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default
    )
    {
        using var response = await httpClient
            .GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken)
            .ConfigureAwait(false);
        response.EnsureSuccessStatusCode();

        var total = response.Content.Headers.ContentLength;
        await using var source = await response
            .Content.ReadAsStreamAsync(cancellationToken)
            .ConfigureAwait(false);
        await using var destination = new FileStream(
            destinationPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 81920,
            useAsync: true
        );

        var buffer = new byte[81920];
        long written = 0;
        int read;
        while (
            (read = await source.ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken)
                .ConfigureAwait(false)) > 0
        )
        {
            await destination
                .WriteAsync(buffer.AsMemory(0, read), cancellationToken)
                .ConfigureAwait(false);
            written += read;
            if (total is > 0)
                progress?.Report((double)written / total.Value);
        }

        progress?.Report(1.0);
    }

    internal static string ParseVersionFromTag(string? tagName)
    {
        if (string.IsNullOrWhiteSpace(tagName))
            throw new InvalidOperationException("Release has no tag name.");

        var tag = tagName.Trim();
        const string prefix = "launcher-";
        if (tag.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            tag = tag[prefix.Length..];

        if (LaunchVersion.TryParse(tag) is null)
            throw new InvalidOperationException($"Unrecognized release tag '{tagName}'.");

        return tag;
    }

    private static string? TryParseDigestSha256(string? digest)
    {
        if (string.IsNullOrWhiteSpace(digest))
            return null;

        const string prefix = "sha256:";
        if (!digest.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            return null;

        var hash = digest[prefix.Length..].Trim();
        return IsSha256Hex(hash) ? hash.ToLowerInvariant() : null;
    }

    private async Task<string?> TryFetchSha256FromSumsAsync(
        IReadOnlyList<GitHubApiAsset> assets,
        string zipFileName,
        CancellationToken cancellationToken
    )
    {
        var sumsAsset = assets.FirstOrDefault(a =>
            a.Name.Equals("SHA256SUMS", StringComparison.OrdinalIgnoreCase)
        );
        if (sumsAsset is null)
            return null;

        var text = await httpClient
            .GetStringAsync(sumsAsset.BrowserDownloadUrl, cancellationToken)
            .ConfigureAwait(false);

        foreach (
            var line in text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
        )
        {
            var parts = line.Split(
                [' ', '\t'],
                2,
                StringSplitOptions.RemoveEmptyEntries
            );
            if (parts.Length < 2)
                continue;

            var hash = parts[0].Trim();
            var name = parts[1].Trim().TrimStart('*');
            if (
                name.Equals(zipFileName, StringComparison.OrdinalIgnoreCase)
                && IsSha256Hex(hash)
            )
                return hash.ToLowerInvariant();
        }

        return null;
    }

    private static bool IsSha256Hex(string value) =>
        value.Length == 64 && value.All(Uri.IsHexDigit);

    private static HttpClient CreateDefaultHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        client.DefaultRequestHeaders.UserAgent.Add(
            new ProductInfoHeaderValue("aisp.launch", LaunchVersion.Display)
        );
        client.DefaultRequestHeaders.Accept.Add(
            new MediaTypeWithQualityHeaderValue("application/vnd.github+json")
        );
        return client;
    }

    private sealed class GitHubApiRelease
    {
        [JsonPropertyName("tag_name")]
        public string TagName { get; set; } = "";

        [JsonPropertyName("assets")]
        public List<GitHubApiAsset> Assets { get; set; } = [];
    }

    private sealed class GitHubApiAsset
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = "";

        [JsonPropertyName("browser_download_url")]
        public string BrowserDownloadUrl { get; set; } = "";

        [JsonPropertyName("digest")]
        public string? Digest { get; set; }

        [JsonPropertyName("size")]
        public long Size { get; set; }
    }
}

internal sealed record GitHubReleaseInfo(
    string Version,
    string TagName,
    string ZipFileName,
    string DownloadUrl,
    string? Sha256,
    long SizeBytes
);
