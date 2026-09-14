using System.Net.Sockets;
using System.Text;

namespace aisp.launch;

internal static class NativeElectronBroker
{
    public const string ListenAddress = "127.0.0.1:18764";
    private const int ListenPort = 18764;

    public static async Task EnsureListeningAsync(
        IProgress<string>? status = null,
        CancellationToken cancellationToken = default
    )
    {
        if (await IsListeningAsync(cancellationToken).ConfigureAwait(false))
            return;

        status?.Report("Starting native Electron broker…");

        var electronWin = Path.GetFullPath(ElectronRuntime.ElectronExecutablePath);
        var hostJsWin = Path.GetFullPath(ElectronRuntime.HostJsPath);
        var appWin = Path.GetFullPath(ElectronRuntime.AppDirectory);
        var scriptWin = Path.Combine(RuntimeDataPaths.ElectronDirectory, "run-host.sh");

        if (!File.Exists(electronWin) || !File.Exists(hostJsWin))
        {
            throw new InvalidOperationException(
                "Native Electron or host.js is missing; cannot start the Wine browser broker."
            );
        }

        var electronUnix = WineUnix.GetUnixPath(electronWin);
        var hostUnix = WineUnix.GetUnixPath(hostJsWin);
        var appUnix = WineUnix.GetUnixPath(appWin);

        Directory.CreateDirectory(RuntimeDataPaths.ElectronDirectory);
        File.WriteAllText(
            scriptWin,
            BuildRunScript(electronUnix, hostUnix, appUnix),
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false)
        );

        WineUnix.RunWait("/bin/chmod", "+x", electronUnix, hostUnix);
        var crashpad = Path.Combine(RuntimeDataPaths.ElectronDirectory, "chrome_crashpad_handler");
        if (File.Exists(crashpad))
            WineUnix.RunWait("/bin/chmod", "+x", WineUnix.GetUnixPath(crashpad));

        var scriptUnix = WineUnix.GetUnixPath(scriptWin);
        WineUnix.RunDetached("/bin/sh", scriptUnix);

        var deadline = DateTime.UtcNow.AddSeconds(15);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (await IsListeningAsync(cancellationToken).ConfigureAwait(false))
                return;
            await Task.Delay(200, cancellationToken).ConfigureAwait(false);
        }

        throw new TimeoutException(
            "Native Electron broker did not start listening on 127.0.0.1:18764. "
                + "Check that aisp.launch.data/electron-linux/electron is a Linux binary."
        );
    }

    public static async Task<bool> IsListeningAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            using var client = new TcpClient();
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(250));
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                timeout.Token
            );
            await client.ConnectAsync("127.0.0.1", ListenPort, linked.Token).ConfigureAwait(false);
            return client.Connected;
        }
        catch
        {
            return false;
        }
    }

    private static string BuildRunScript(string electronUnix, string hostUnix, string appUnix)
    {
        static string Q(string value) => "'" + value.Replace("'", "'\\''") + "'";

        return string.Join(
            "\n",
            "#!/bin/sh",
            "export ELECTRON_RUN_AS_NODE=1",
            $"export AISP_ELECTRON_NATIVE_BIN={Q(electronUnix)}",
            $"export AISP_ELECTRON_APP={Q(appUnix)}",
            $"export AISP_ELECTRON_NATIVE={Q(ListenAddress)}",
            $"exec {Q(electronUnix)} {Q(hostUnix)}",
            ""
        );
    }
}
