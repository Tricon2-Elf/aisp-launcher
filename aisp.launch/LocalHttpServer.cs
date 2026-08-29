using System.Diagnostics;
using System.Net;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace aisp.launch;

public sealed class LocalHttpServer : IAsyncDisposable
{
    public const string NicoPlayerPath = "/player/jdfoiajwpefha/nicoplayer.php";
    public const string LiveNicoHost = "live.nicovideo.jp";

    private WebApplication? _app;

    public async Task<LocalHttpServerResult> StartAsync(CancellationToken cancellationToken = default)
    {
        if (_app is not null)
            return LocalHttpServerResult.Success("Local HTTP server is already running.");

        try
        {
            var builder = WebApplication.CreateBuilder(
                new WebApplicationOptions
                {
                    Args = Array.Empty<string>(),
                    ContentRootPath = AppContext.BaseDirectory,
                }
            );

            builder.Logging.ClearProviders();
            builder.WebHost.ConfigureKestrel(options =>
            {
                options.Listen(IPAddress.Loopback, 80);
                options.Listen(IPAddress.IPv6Loopback, 80);
            });

            var app = builder.Build();

            app.Use(
                async (context, next) =>
                {
                    context.Response.OnStarting(() =>
                    {
                        context.Response.Headers["X-UA-Compatible"] = "IE=edge";
                        context.Response.Headers["Cache-Control"] = "no-store";
                        context.Response.Headers["Connection"] = "close";
                        return Task.CompletedTask;
                    });

                    await next();
                }
            );

            app.MapMethods(
                "/health",
                [HttpMethods.Get, HttpMethods.Head],
                (HttpContext context) => WriteText(context, "ok\n", "text/plain; charset=utf-8")
            );

            app.MapMethods(
                "/favicon.ico",
                [HttpMethods.Get, HttpMethods.Head],
                (HttpContext context) =>
                {
                    context.Response.StatusCode = StatusCodes.Status204NoContent;
                    return Task.CompletedTask;
                }
            );

            app.MapMethods(
                NicoPlayerPath,
                [HttpMethods.Get, HttpMethods.Head],
                HandleNicoPlayerRequest
            );

            app.MapMethods(
                "/watch/{*liveId}",
                [HttpMethods.Get, HttpMethods.Head],
                (HttpContext context, string? liveId) =>
                {
                    if (!IsLiveNicoHost(context))
                        return WriteNotFound(context);

                    return WriteDiagnosticPage(
                        context,
                        "Nico Live request intercepted",
                        new Dictionary<string, string>
                        {
                            ["Live ID"] = string.IsNullOrWhiteSpace(liveId)
                                ? "(empty)"
                                : Uri.UnescapeDataString(liveId.Trim('/')),
                            ["Host"] = GetRequestHost(context),
                            ["Status"] = "The request reached the local launcher proxy.",
                        }
                    );
                }
            );

            app.MapFallback(async (HttpContext context) =>
            {
                var logger = context.RequestServices.GetRequiredService<ILogger<LocalHttpServer>>();
                logger.LogDebug(
                    "Unhandled request: {Method} {Host}{Path}{Query}",
                    context.Request.Method,
                    context.Request.Host.Value,
                    context.Request.Path,
                    context.Request.QueryString
                );
                await WriteNotFound(context);
            });

            await app.StartAsync(cancellationToken);
            _app = app;
            return LocalHttpServerResult.Success("Local HTTP server started on http://127.0.0.1/");
        }
        catch (Exception ex) when (IsBindFailure(ex))
        {
            return LocalHttpServerResult.Failure(
                "Could not start the local HTTP server on port 80.",
                "Port 80 may already be in use, or the launcher may need administrator/root privileges to bind that port."
                    + Environment.NewLine
                    + Environment.NewLine
                    + ex.Message
            );
        }
        catch (Exception ex)
        {
            return LocalHttpServerResult.Failure(
                "Could not start the local HTTP server.",
                ex.Message
            );
        }
    }

    private static Task HandleNicoPlayerRequest(HttpContext context)
    {
        var query = context.Request.Query;
        var movieId = query["movieid"].ToString().Trim();
        var tvId = query["tvid"].ToString().Trim();
        var channelId = query["chid"].ToString().Trim();

        Trace.WriteLine(
            $"Nico TV request: host={GetRequestHost(context)}, path={context.Request.Path.Value}, "
                + $"movieid={movieId}, tvid={tvId}, chid={channelId}"
        );

        if (!string.IsNullOrEmpty(movieId))
        {
            return WriteDiagnosticPage(
                context,
                "Movie request intercepted",
                new Dictionary<string, string>
                {
                    ["Movie ID"] = movieId,
                    ["Status"] = "Media playback is not configured in the launcher yet.",
                }
            );
        }

        if (!string.IsNullOrEmpty(tvId) || !string.IsNullOrEmpty(channelId))
        {
            var channelKey = $"{tvId}:{channelId}";
            return WriteDiagnosticPage(
                context,
                "TV request intercepted",
                new Dictionary<string, string>
                {
                    ["TV ID"] = string.IsNullOrEmpty(tvId) ? "(empty)" : tvId,
                    ["Channel ID"] = string.IsNullOrEmpty(channelId) ? "(empty)" : channelId,
                    ["Channel-map key"] = channelKey,
                    ["Status"] = "Media playback is not configured in the launcher yet.",
                }
            );
        }

        return WriteDiagnosticPage(
            context,
            "ai sp@ce Nico TV proxy",
            new Dictionary<string, string>
            {
                ["Request path"] = context.Request.Path.Value ?? "/",
                ["Query"] = context.Request.QueryString.Value?.TrimStart('?') ?? "(empty)",
                ["Status"] = "The request reached the local launcher proxy.",
            }
        );
    }

    private static Task WriteDiagnosticPage(
        HttpContext context,
        string title,
        IReadOnlyDictionary<string, string> details
    )
    {
        var body = NicoTvPages.BuildDiagnosticPage(title, details);
        return WriteBytes(context, body, "text/html; charset=utf-8");
    }

    private static Task WriteText(HttpContext context, string body, string contentType)
    {
        return WriteBytes(context, System.Text.Encoding.UTF8.GetBytes(body), contentType);
    }

    private static Task WriteBytes(HttpContext context, byte[] body, string contentType)
    {
        context.Response.StatusCode = StatusCodes.Status200OK;
        context.Response.ContentType = contentType;
        context.Response.ContentLength = body.Length;

        if (HttpMethods.IsHead(context.Request.Method))
            return Task.CompletedTask;

        return context.Response.Body.WriteAsync(body).AsTask();
    }

    private static Task WriteNotFound(HttpContext context)
    {
        context.Response.StatusCode = StatusCodes.Status404NotFound;
        context.Response.ContentType = "text/plain; charset=utf-8";
        context.Response.ContentLength = 9;

        if (HttpMethods.IsHead(context.Request.Method))
            return Task.CompletedTask;

        return context.Response.WriteAsync("Not found");
    }

    private static string GetRequestHost(HttpContext context) =>
        context.Request.Headers.Host.ToString().Split(':', 2)[0];

    private static bool IsLiveNicoHost(HttpContext context) =>
        string.Equals(GetRequestHost(context), LiveNicoHost, StringComparison.OrdinalIgnoreCase);

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        var app = Interlocked.Exchange(ref _app, null);
        if (app is null)
            return;

        try
        {
            await app.StopAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Timed out while shutting down Kestrel.
        }

        try
        {
            await app.DisposeAsync();
        }
        catch
        {
            // Best-effort cleanup during app exit.
        }
    }

    public async ValueTask DisposeAsync() => await StopAsync();

    private static bool IsBindFailure(Exception ex)
    {
        for (var current = ex; current is not null; current = current.InnerException)
        {
            if (current is HttpListenerException or System.Net.Sockets.SocketException)
                return true;
        }

        return false;
    }
}

public readonly record struct LocalHttpServerResult(
    bool Succeeded,
    string Message,
    string? Details = null
)
{
    public static LocalHttpServerResult Success(string message) => new(true, message);

    public static LocalHttpServerResult Failure(string message, string? details = null) =>
        new(false, message, details);
}
