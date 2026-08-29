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

    private const string PlaceholderHtml =
        """
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8" />
            <title>AI SP@CE Local Player</title>
        </head>
        <body>
            <h1>Local player proxy</h1>
            <p>Placeholder page served by aisp.launch.</p>
        </body>
        </html>
        """;

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

            app.MapGet("/health", () => Results.Text("ok"));

            app.MapGet(
                NicoPlayerPath,
                () => Results.Content(PlaceholderHtml, "text/html; charset=utf-8")
            );

            app.MapFallback(async (HttpContext context) =>
            {
                var logger = context.RequestServices.GetRequiredService<ILogger<LocalHttpServer>>();
                logger.LogDebug(
                    "Unhandled request: {Method} {Path}",
                    context.Request.Method,
                    context.Request.Path
                );
                context.Response.StatusCode = StatusCodes.Status404NotFound;
                await context.Response.WriteAsync("Not found");
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

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        if (_app is null)
            return;

        await _app.StopAsync(cancellationToken);
        await _app.DisposeAsync();
        _app = null;
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
