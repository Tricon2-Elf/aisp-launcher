using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace aisp.launch;

public sealed class TwitchBridge : IDisposable
{
    private static readonly Regex ChannelPattern = new(
        @"^[A-Za-z0-9_]{1,25}$",
        RegexOptions.CultureInvariant
    );

    private static readonly Regex SegmentPattern = new(
        @"^segment-(\d{8})\.m4s$",
        RegexOptions.CultureInvariant
    );

    private readonly string _streamlinkCommand;
    private readonly string _ffmpegCommand;
    private readonly object _stateLock = new();
    private readonly CancellationTokenSource _lifetime = new();
    private readonly List<Task> _backgroundTasks = [];

    private string _state = "starting";
    private string _message = "Waiting for Twitch and the first completed video segment.";
    private string? _tempDirectory;
    private string? _playlistPath;
    private string? _initSegmentPath;
    private Process? _streamlinkProcess;
    private Process? _ffmpegProcess;
    private bool _disposed;

    public TwitchBridge(
        string channel,
        MediaTools mediaTools,
        string quality = "480p",
        int segmentSeconds = 2,
        int retainedSegments = 30,
        int bufferSegments = 3
    )
    {
        if (!ChannelPattern.IsMatch(channel))
        {
            throw new ArgumentException(
                "Twitch channel names may contain only letters, numbers, and underscores.",
                nameof(channel)
            );
        }

        if (segmentSeconds is < 2 or > 10)
            throw new ArgumentOutOfRangeException(nameof(segmentSeconds));
        if (retainedSegments is < 5 or > 300)
            throw new ArgumentOutOfRangeException(nameof(retainedSegments));
        if (bufferSegments is < 2 or > 10)
            throw new ArgumentOutOfRangeException(nameof(bufferSegments));
        if (bufferSegments >= retainedSegments)
        {
            throw new ArgumentException(
                "Buffer segments must be smaller than retained segments.",
                nameof(bufferSegments)
            );
        }

        Channel = channel;
        Quality = quality;
        SegmentSeconds = segmentSeconds;
        RetainedSegments = retainedSegments;
        BufferSegments = bufferSegments;
        Generation = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(8));
        _streamlinkCommand = mediaTools.Streamlink;
        _ffmpegCommand = mediaTools.Ffmpeg;
    }

    public string Channel { get; }

    public string Quality { get; }

    public int SegmentSeconds { get; }

    public int RetainedSegments { get; }

    public int BufferSegments { get; }

    public string Generation { get; }

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        _tempDirectory = TempCleanup.CreateSessionDirectory();
        _playlistPath = Path.Combine(_tempDirectory, "stream.m3u8");
        _initSegmentPath = Path.Combine(_tempDirectory, "init.mp4");
        var segmentPattern = Path.Combine(_tempDirectory, "segment-%08d.m4s");

        Trace.WriteLine(
            $"Starting Twitch bridge for '{Channel}' at quality '{Quality}' ({SegmentSeconds}s segments)."
        );

        var streamlinkDirectory = Path.GetDirectoryName(_streamlinkCommand)
            ?? AppContext.BaseDirectory;

        var streamlinkStartInfo = new ProcessStartInfo
        {
            FileName = _streamlinkCommand,
            Arguments =
                $"--stdout --loglevel info --retry-streams 5 --retry-open 3 --hls-live-edge 3 "
                + $"https://www.twitch.tv/{Channel} {Quality}",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = streamlinkDirectory,
        };
        RedirectTempEnvironment(streamlinkStartInfo, _tempDirectory);

        var ffmpegStartInfo = new ProcessStartInfo
        {
            FileName = _ffmpegCommand,
            Arguments = BuildFfmpegArguments("pipe:0", segmentPattern, _playlistPath, _initSegmentPath),
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = _tempDirectory,
        };
        RedirectTempEnvironment(ffmpegStartInfo, _tempDirectory);

        _streamlinkProcess = Process.Start(streamlinkStartInfo)
            ?? throw new InvalidOperationException("Could not start Streamlink.");
        _ffmpegProcess = Process.Start(ffmpegStartInfo)
            ?? throw new InvalidOperationException("Could not start FFmpeg.");

        _backgroundTasks.Add(PumpProcessLogAsync(_streamlinkProcess, "Streamlink", _lifetime.Token));
        _backgroundTasks.Add(PumpProcessLogAsync(_ffmpegProcess, "FFmpeg", _lifetime.Token));
        _backgroundTasks.Add(PumpStreamAsync(_streamlinkProcess, _ffmpegProcess, _lifetime.Token));
        _backgroundTasks.Add(MonitorProcessesAsync(_ffmpegProcess, _streamlinkProcess, _lifetime.Token));
    }

    public string BuildManifestJson()
    {
        var segments = GetCompletedSegments();
        if (segments.Count > 0)
        {
            lock (_stateLock)
            {
                if (_state == "starting")
                {
                    _state = "running";
                    _message = $"Playing Twitch channel {Channel}.";
                }
            }
        }

        PruneSegments(segments);

        lock (_stateLock)
        {
            var userMessage =
                segments.Count == 0 && _state == "starting"
                    ? "Waiting for the first completed Twitch segment..."
                    : _message;

            var document = new
            {
                generation = Generation,
                channel = Channel,
                status = _state,
                message = userMessage,
                segmentSeconds = SegmentSeconds,
                bufferSegments = BufferSegments,
                mimeType = "video/mp4; codecs=\"avc1.42C01F, mp4a.40.2\"",
                initUrl = NicoTvConstants.TwitchInitPath,
                segments = segments
                    .TakeLast(RetainedSegments)
                    .Select(segment => new
                    {
                        sequence = segment.Sequence,
                        name = segment.Name,
                        url = segment.Url,
                        duration = segment.Duration,
                    }),
            };

            return JsonSerializer.Serialize(document);
        }
    }

    public string? GetInitPath()
    {
        if (_initSegmentPath is null || !File.Exists(_initSegmentPath))
            return null;

        return new FileInfo(_initSegmentPath).Length > 0 ? _initSegmentPath : null;
    }

    public string? GetSegmentPath(string name)
    {
        if (!SegmentPattern.IsMatch(name) || _tempDirectory is null)
            return null;

        var completedNames = GetCompletedSegments().Select(segment => segment.Name).ToHashSet();
        if (!completedNames.Contains(name))
            return null;

        var path = Path.Combine(_tempDirectory, name);
        return File.Exists(path) ? path : null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;

        try
        {
            _lifetime.Cancel();
        }
        catch
        {
            // Already disposed.
        }

        // Kill first and do not WaitForExit. With redirected stdout/stderr,
        // WaitForExit waits for those pipes to drain and can hang forever.
        StopProcess(_ffmpegProcess);
        StopProcess(_streamlinkProcess);
        UnblockPipes(_ffmpegProcess);
        UnblockPipes(_streamlinkProcess);

        TempCleanup.TryDeleteDirectory(_tempDirectory);
        _tempDirectory = null;

        _lifetime.Dispose();
        Trace.WriteLine("Twitch bridge stopped.");
    }

    private static void RedirectTempEnvironment(ProcessStartInfo startInfo, string tempDirectory)
    {
        startInfo.Environment["TEMP"] = tempDirectory;
        startInfo.Environment["TMP"] = tempDirectory;
        startInfo.Environment["TMPDIR"] = tempDirectory;
    }

    private string BuildFfmpegArguments(
        string inputSource,
        string segmentPattern,
        string playlistPath,
        string initSegmentPath
    )
    {
        return string.Join(
            " ",
            [
                "-hide_banner",
                "-loglevel",
                "warning",
                "-fflags",
                "+genpts",
                "-thread_queue_size",
                "4096",
                "-f",
                "mpegts",
                "-i",
                inputSource,
                "-map",
                "0:v:0",
                "-map",
                "0:a:0?",
                "-c:v",
                "copy",
                "-c:a",
                "copy",
                "-bsf:a",
                "aac_adtstoasc",
                "-avoid_negative_ts",
                "make_zero",
                "-f",
                "hls",
                "-hls_time",
                SegmentSeconds.ToString(),
                "-hls_segment_type",
                "fmp4",
                "-hls_fmp4_init_filename",
                Path.GetFileName(initSegmentPath),
                "-hls_segment_filename",
                Quote(segmentPattern),
                "-hls_list_size",
                "0",
                "-hls_playlist_type",
                "event",
                "-hls_flags",
                "independent_segments+temp_file",
                "-y",
                Quote(playlistPath),
            ]
        );
    }

    private List<CompletedSegment> GetCompletedSegments()
    {
        if (
            _tempDirectory is null
            || _playlistPath is null
            || !File.Exists(_playlistPath)
            || GetInitPath() is null
        )
        {
            return [];
        }

        var segments = new List<CompletedSegment>();
        double? duration = null;

        try
        {
            using var stream = new FileStream(
                _playlistPath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite
            );
            using var reader = new StreamReader(stream);
            while (reader.ReadLine() is { } rawLine)
            {
                var line = rawLine.Trim();
                if (line.StartsWith("#EXTINF:", StringComparison.Ordinal))
                {
                    var value = line["#EXTINF:".Length..].Split(',', 2)[0];
                    duration = double.TryParse(value, out var parsed) ? parsed : null;
                    continue;
                }

                if (line.Length == 0 || line.StartsWith('#'))
                    continue;

                var name = GetSegmentName(line);
                var match = SegmentPattern.Match(name);
                var path = Path.Combine(_tempDirectory, name);
                if (
                    !match.Success
                    || duration is null
                    || !File.Exists(path)
                    || new FileInfo(path).Length == 0
                )
                {
                    duration = null;
                    continue;
                }

                segments.Add(
                    new CompletedSegment(
                        int.Parse(match.Groups[1].Value),
                        name,
                        $"{NicoTvConstants.TwitchSegmentPrefix}{name}",
                        Math.Max(0.0, duration.Value)
                    )
                );
                duration = null;
            }
        }
        catch (IOException)
        {
            return [];
        }

        segments.Sort((left, right) => left.Sequence.CompareTo(right.Sequence));
        return segments;
    }

    private void PruneSegments(List<CompletedSegment> segments)
    {
        var excessCount = segments.Count - RetainedSegments;
        if (excessCount <= 0 || _tempDirectory is null)
            return;

        foreach (var segment in segments.Take(excessCount))
        {
            try
            {
                File.Delete(Path.Combine(_tempDirectory, segment.Name));
            }
            catch
            {
                // Best-effort pruning.
            }
        }
    }

    private async Task PumpStreamAsync(
        Process streamlinkProcess,
        Process ffmpegProcess,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await streamlinkProcess.StandardOutput.BaseStream.CopyToAsync(
                ffmpegProcess.StandardInput.BaseStream,
                cancellationToken
            );
        }
        catch (OperationCanceledException)
        {
            // Expected during shutdown.
        }
        catch (IOException)
        {
            // One of the processes exited.
        }
        finally
        {
            try
            {
                await ffmpegProcess.StandardInput.BaseStream.FlushAsync(cancellationToken);
            }
            catch
            {
                // Ignore flush failures during shutdown.
            }

            ffmpegProcess.StandardInput.Close();
        }
    }

    private async Task PumpProcessLogAsync(
        Process process,
        string label,
        CancellationToken cancellationToken
    )
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var line = await process.StandardError.ReadLineAsync(cancellationToken);
                if (line is null)
                    break;

                if (line.Length == 0)
                    continue;

                Trace.WriteLine($"[{label}] {line}");
                if (ShouldPromoteLogLine(label, line))
                {
                    lock (_stateLock)
                    {
                        _message = line;
                    }
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Expected during shutdown.
        }
    }

    private static bool ShouldPromoteLogLine(string label, string line)
    {
        if (label == "FFmpeg")
            return true;

        var lowered = line.ToLowerInvariant();
        return lowered.Contains("error", StringComparison.Ordinal)
            || lowered.Contains("warning", StringComparison.Ordinal)
            || lowered.Contains("failed", StringComparison.Ordinal);
    }

    private static string GetSegmentName(string line)
    {
        if (line.Contains("://", StringComparison.Ordinal))
            return Path.GetFileName(new Uri(line, UriKind.Absolute).LocalPath);

        return Path.GetFileName(line.Split('?', 2)[0].Trim());
    }

    private async Task MonitorProcessesAsync(
        Process ffmpegProcess,
        Process streamlinkProcess,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await ffmpegProcess.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (cancellationToken.IsCancellationRequested)
            return;

        var streamlinkExitCode = streamlinkProcess.HasExited ? streamlinkProcess.ExitCode : (int?)null;
        StopProcess(streamlinkProcess);

        lock (_stateLock)
        {
            _state = "error";
            _message =
                $"The Twitch bridge stopped (FFmpeg={ffmpegProcess.ExitCode}, Streamlink={streamlinkExitCode}). "
                + $"Last message: {_message}";
        }
    }

    private static void StopProcess(Process? process)
    {
        if (process is null)
            return;

        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch
        {
            // Best-effort shutdown.
        }
    }

    private static void UnblockPipes(Process? process)
    {
        if (process is null)
            return;

        try
        {
            process.StandardInput.Close();
        }
        catch
        {
            // Ignore.
        }

        try
        {
            process.StandardOutput.Close();
        }
        catch
        {
            // Ignore.
        }

        try
        {
            process.StandardError.Close();
        }
        catch
        {
            // Ignore.
        }
    }

    private static string Quote(string value) =>
        value.Contains(' ', StringComparison.Ordinal) ? $"\"{value}\"" : value;

    private readonly record struct CompletedSegment(
        int Sequence,
        string Name,
        string Url,
        double Duration
    );
}
