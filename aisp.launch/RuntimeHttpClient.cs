using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace aisp.launch;

/// <summary>
/// Shared HTTP helpers for downloading GitHub release assets and plain files.
/// </summary>
internal sealed class RuntimeHttpClient : IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly HttpClient _http;

    public RuntimeHttpClient()
    {
        _http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        _http.DefaultRequestHeaders.UserAgent.Add(
            new ProductInfoHeaderValue("aisp.launch", LaunchVersion.Display)
        );
        _http.DefaultRequestHeaders.Accept.Add(
            new MediaTypeWithQualityHeaderValue("application/vnd.github+json")
        );
    }

    public async Task<GitHubApiReleaseDocument> GetReleaseAsync(
        string apiUrl,
        CancellationToken cancellationToken = default
    )
    {
        using var response = await _http.GetAsync(apiUrl, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        await using var stream = await response
            .Content.ReadAsStreamAsync(cancellationToken)
            .ConfigureAwait(false);
        return await JsonSerializer
                .DeserializeAsync<GitHubApiReleaseDocument>(stream, JsonOptions, cancellationToken)
                .ConfigureAwait(false)
            ?? throw new InvalidOperationException("GitHub returned an empty release payload.");
    }

    public async Task<string> GetStringAsync(
        string url,
        CancellationToken cancellationToken = default
    )
    {
        using var response = await _http.GetAsync(url, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        return await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
    }

    public async Task DownloadToFileAsync(
        string url,
        string destinationPath,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default
    )
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);

        using var response = await _http
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
            (
                read = await source
                    .ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken)
                    .ConfigureAwait(false)
            ) > 0
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

    public static async Task<string> ComputeSha256Async(
        string path,
        CancellationToken cancellationToken = default
    )
    {
        await using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 81920,
            useAsync: true
        );
        var hash = await SHA256.HashDataAsync(stream, cancellationToken).ConfigureAwait(false);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public void Dispose() => _http.Dispose();
}

internal sealed class GitHubApiReleaseDocument
{
    [JsonPropertyName("tag_name")]
    public string TagName { get; set; } = "";

    [JsonPropertyName("assets")]
    public List<GitHubApiAssetDocument> Assets { get; set; } = [];
}

internal sealed class GitHubApiAssetDocument
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
