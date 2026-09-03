using System.Net;
using System.Text;

namespace aisp.launch;

internal static class NicoTvPages
{
    public static byte[] BuildVideoPlayerPage(string movieId, string localVideoPath, bool loopVideo)
    {
        var loopAttribute = loopVideo ? " loop=\"loop\"" : string.Empty;
        var html =
            """
            <!doctype html>
            <html>
            <head>
            <meta http-equiv="X-UA-Compatible" content="IE=edge">
            <meta http-equiv="Content-Type" content="text/html; charset=utf-8">
            <title>ai sp@ce Nico TV test</title>
            <style type="text/css">
            html, body { width:1024px; height:1024px; margin:0; overflow:hidden; background:#000; color:#fff; }
            #player {
              position:absolute; left:15px; top:9px; width:480px; height:349px;
              background:#000; pointer-events:none;
            }
            #status {
              position:absolute; left:15px; top:9px; width:468px; padding:6px;
              z-index:2; font:12px/16px Arial,sans-serif; color:#fff;
              background:#80000000; background:rgba(0,0,0,.65);
            }
            </style>
            </head>
            <body>
            <video id="player" width="480" height="349"
                   autoplay="autoplay" preload="auto"{{LOOP_ATTRIBUTE}} oncontextmenu="return false;">
              <source src="{{LOCAL_VIDEO_PATH}}" type='video/mp4; codecs="avc1.42E01E, mp4a.40.2"'>
              HTML5 video is unavailable in this Trident document mode.
            </video>
            <div id="status">Starting local video for ID: {{MOVIE_ID}}</div>
            <script type="text/javascript">
            (function () {
              var player = document.getElementById("player");
              var status = document.getElementById("status");
              var mode = document.documentMode || 0;

              function show(message, failed) {
                status.style.backgroundColor = failed ? "#a00000" : "#000000";
                status.innerHTML = "Trident documentMode=" + mode + " | " + message;
              }

              if (!player || !player.play) {
                show("HTML5 video is not supported. IE9+ document mode is required.", true);
                return;
              }

              player.controls = false;
              show("MP4 support=" + player.canPlayType('video/mp4; codecs="avc1.42E01E, mp4a.40.2"') +
                   " | loading...", false);

              player.addEventListener("playing", function () {
                show("PLAYING", false);
                window.setTimeout(function () { status.style.display = "none"; }, 2000);
              }, false);
              player.addEventListener("error", function () {
                var code = player.error ? player.error.code : "unknown";
                show("VIDEO ERROR code=" + code, true);
              }, false);
              player.addEventListener("stalled", function () { show("STALLED while loading", true); }, false);

              try {
                player.load();
                player.play();
              } catch (error) {
                show("play() failed: " + error.message, true);
              }
            }());
            </script>
            </body>
            </html>
            """
                .Replace("{{LOOP_ATTRIBUTE}}", loopAttribute, StringComparison.Ordinal)
                .Replace("{{LOCAL_VIDEO_PATH}}", WebUtility.HtmlEncode(localVideoPath), StringComparison.Ordinal)
                .Replace("{{MOVIE_ID}}", WebUtility.HtmlEncode(movieId), StringComparison.Ordinal);

        return Encoding.UTF8.GetBytes(html);
    }

    public static byte[] BuildTwitchPlayerPage(
        string twitchChannel,
        string requestLabel,
        string manifestPath
    )
    {
        var html =
            """
            <!doctype html>
            <html>
            <head>
            <meta http-equiv="X-UA-Compatible" content="IE=edge">
            <meta http-equiv="Content-Type" content="text/html; charset=utf-8">
            <title>ai sp@ce Twitch bridge</title>
            <style type="text/css">
            html, body { width:1024px; height:1024px; margin:0; overflow:hidden; background:#000; color:#fff; }
            #player {
              position:absolute; left:15px; top:9px; width:480px; height:349px;
              z-index:1; background:#000; pointer-events:none;
            }
            #status {
              position:absolute; left:15px; top:9px; width:468px; min-height:16px; padding:6px;
              z-index:2; font:12px/16px Arial,sans-serif; color:#fff; background:#000;
            }
            </style>
            </head>
            <body>
            <video id="player" width="480" height="349"
                   autoplay="autoplay" preload="auto" oncontextmenu="return false;">
              HTML5 video is unavailable in this Trident document mode.
            </video>
            <div id="status">Connecting to Twitch channel {{TWITCH_CHANNEL}} ({{REQUEST_LABEL}})...</div>
            <script type="text/javascript">
            (function () {
              var player = document.getElementById("player");
              var status = document.getElementById("status");
              var documentMode = document.documentMode || 0;
              var MediaSourceType = window.MediaSource || window.WebKitMediaSource;
              var URLType = window.URL || window.webkitURL;
              var generation = null;
              var mediaSource = null;
              var mediaObjectUrl = null;
              var sourceBuffer = null;
              var mimeType = null;
              var seen = {};
              var downloadQueue = [];
              var appendQueue = [];
              var downloading = false;
              var appending = null;
              var appendedSegments = 0;
              var initialBufferSegments = 3;
              var started = false;
              var polling = false;
              var lastTrimTime = 0;

              function show(message, failed) {
                status.style.display = "block";
                status.style.backgroundColor = failed ? "#a00000" : "#000000";
                status.innerText = "Trident documentMode=" + documentMode + " | " + message;
              }

              function hideStatusSoon() {
                window.setTimeout(function () {
                  if (started && !player.paused) {
                    status.style.display = "none";
                  }
                }, 1000);
              }

              function resourceUrl(url) {
                return url + (url.indexOf("?") >= 0 ? "&" : "?") +
                  "generation=" + encodeURIComponent(generation);
              }

              function queueResource(key, url, isSegment) {
                downloadQueue.push({
                  key: key,
                  url: resourceUrl(url),
                  isSegment: isSegment,
                  attempts: 0
                });
                pumpDownload();
              }

              function pumpDownload() {
                var item;
                var request;

                if (downloading || !downloadQueue.length) {
                  return;
                }

                item = downloadQueue.shift();
                item.attempts += 1;
                downloading = true;
                request = new XMLHttpRequest();
                request.open("GET", item.url, true);
                request.responseType = "arraybuffer";
                request.onreadystatechange = function () {
                  if (request.readyState !== 4) {
                    return;
                  }
                  downloading = false;
                  if ((request.status === 200 || request.status === 206) && request.response) {
                    appendQueue.push({
                      key: item.key,
                      data: request.response,
                      isSegment: item.isSegment
                    });
                    pumpAppend();
                  } else if (item.attempts < 5) {
                    downloadQueue.unshift(item);
                    window.setTimeout(pumpDownload, 500);
                    return;
                  } else {
                    show("Media download failed with HTTP " + request.status + ": " + item.key, true);
                  }
                  pumpDownload();
                };
                request.send(null);
              }

              function startPlaybackIfBuffered() {
                if (started || appendedSegments < initialBufferSegments || !sourceBuffer) {
                  return;
                }

                if (sourceBuffer.buffered.length &&
                    player.currentTime < sourceBuffer.buffered.start(0)) {
                  player.currentTime = sourceBuffer.buffered.start(0) + 0.05;
                }
                started = true;
                show("Starting continuous live playback...", false);
                try {
                  player.play();
                } catch (error) {
                  started = false;
                  show("play() failed: " + error.message, true);
                }
              }

              function trimOldBuffer() {
                var removeBefore;
                if (!sourceBuffer || sourceBuffer.updating || appendQueue.length ||
                    player.currentTime < 60 || player.currentTime - lastTrimTime < 10) {
                  return;
                }
                lastTrimTime = player.currentTime;
                removeBefore = player.currentTime - 30;
                if (sourceBuffer.buffered.length &&
                    sourceBuffer.buffered.start(0) < removeBefore) {
                  try {
                    sourceBuffer.remove(sourceBuffer.buffered.start(0), removeBefore);
                  } catch (ignored) {}
                }
              }

              function pumpAppend() {
                if (!sourceBuffer || sourceBuffer.updating || !appendQueue.length) {
                  trimOldBuffer();
                  return;
                }

                appending = appendQueue.shift();
                try {
                  sourceBuffer.appendBuffer(appending.data);
                } catch (error) {
                  show("Media append failed for " + appending.key + ": " + error.message, true);
                  appending = null;
                  window.setTimeout(pumpAppend, 250);
                }
              }

              function enqueueSegments(data, initial) {
                var segments = data.segments || [];
                var first = 0;
                var index;
                var segment;

                if (initial) {
                  first = Math.max(0, segments.length - initialBufferSegments);
                  for (index = 0; index < first; index += 1) {
                    seen[segments[index].sequence] = true;
                  }
                }

                for (index = first; index < segments.length; index += 1) {
                  segment = segments[index];
                  if (!seen[segment.sequence]) {
                    seen[segment.sequence] = true;
                    queueResource("segment " + segment.sequence, segment.url, true);
                  }
                }
              }

              function resetMediaSource(data) {
                var expectedGeneration = data.generation;

                try { player.pause(); } catch (ignored) {}
                if (mediaObjectUrl) {
                  try { URLType.revokeObjectURL(mediaObjectUrl); } catch (ignored2) {}
                }

                generation = data.generation;
                mimeType = data.mimeType;
                initialBufferSegments = data.bufferSegments || 3;
                seen = {};
                downloadQueue = [];
                appendQueue = [];
                downloading = false;
                appending = null;
                appendedSegments = 0;
                started = false;
                sourceBuffer = null;
                mediaSource = new MediaSourceType();
                mediaObjectUrl = URLType.createObjectURL(mediaSource);
                player.src = mediaObjectUrl;
                player.controls = false;

                mediaSource.addEventListener("sourceopen", function () {
                  if (generation !== expectedGeneration || sourceBuffer) {
                    return;
                  }
                  try {
                    sourceBuffer = mediaSource.addSourceBuffer(mimeType);
                    sourceBuffer.addEventListener("updateend", function () {
                      if (appending && appending.isSegment) {
                        appendedSegments += 1;
                      }
                      appending = null;
                      startPlaybackIfBuffered();
                      pumpAppend();
                    }, false);
                    sourceBuffer.addEventListener("error", function () {
                      show("The continuous media buffer reported an error.", true);
                    }, false);
                    pumpAppend();
                  } catch (error) {
                    show("Could not create the continuous media buffer: " + error.message, true);
                  }
                }, false);

                queueResource("initialization segment", data.initUrl, false);
                enqueueSegments(data, true);
              }

              function enqueueManifest(data) {
                var segments = data.segments || [];
                var required = data.bufferSegments || 3;

                if (!segments.length) {
                  show(data.message || "Waiting for the first completed Twitch segment...", data.status === "error");
                  return;
                }

                if (generation !== data.generation) {
                  if (segments.length < required) {
                    show("Building continuous playback buffer " + segments.length + "/" + required + "...", false);
                    return;
                  }
                  if (!MediaSourceType.isTypeSupported(data.mimeType)) {
                    show("Trident does not support " + data.mimeType, true);
                    return;
                  }
                  resetMediaSource(data);
                  return;
                }

                enqueueSegments(data, false);
                if (data.status === "error") {
                  show(data.message || "The Twitch bridge stopped.", true);
                }
              }

              function pollManifest() {
                var request;
                if (polling) {
                  return;
                }
                polling = true;
                request = new XMLHttpRequest();
                request.open("GET", "{{TWITCH_MANIFEST_PATH}}?time=" + new Date().getTime(), true);
                request.onreadystatechange = function () {
                  var data;
                  if (request.readyState !== 4) {
                    return;
                  }
                  polling = false;
                  if (request.status === 200) {
                    try {
                      data = JSON.parse(request.responseText);
                      enqueueManifest(data);
                    } catch (error) {
                      show("Invalid live manifest: " + error.message, true);
                    }
                  } else {
                    show("Manifest request failed with HTTP " + request.status, true);
                  }
                  window.setTimeout(pollManifest, 750);
                };
                request.send(null);
              }

              if (!player || !player.play || !MediaSourceType || !URLType ||
                  !MediaSourceType.isTypeSupported || !window.XMLHttpRequest ||
                  !window.JSON || !window.ArrayBuffer) {
                show("IE11 Media Source Extensions are required for continuous live playback.", true);
                return;
              }

              player.controls = false;
              player.addEventListener("playing", function () {
                show("LIVE: {{TWITCH_CHANNEL}}", false);
                hideStatusSoon();
              }, false);
              player.addEventListener("waiting", function () {
                if (started) {
                  show("Live buffer is catching up...", false);
                }
              }, false);
              player.addEventListener("error", function () {
                var code = player.error ? player.error.code : "unknown";
                show("Video error " + code + " in the continuous stream.", true);
              }, false);
              player.addEventListener("timeupdate", trimOldBuffer, false);

              show("Connecting to Twitch channel {{TWITCH_CHANNEL}}...", false);
              pollManifest();
            }());
            </script>
            </body>
            </html>
            """
                .Replace("{{TWITCH_CHANNEL}}", WebUtility.HtmlEncode(twitchChannel), StringComparison.Ordinal)
                .Replace("{{REQUEST_LABEL}}", WebUtility.HtmlEncode(requestLabel), StringComparison.Ordinal)
                .Replace("{{TWITCH_MANIFEST_PATH}}", WebUtility.HtmlEncode(manifestPath), StringComparison.Ordinal);

        return Encoding.UTF8.GetBytes(html);
    }

    public static byte[] BuildDiagnosticPage(string title, IReadOnlyDictionary<string, string> details)
    {
        var rows = new StringBuilder();
        foreach (var (label, value) in details)
        {
            rows.Append("<dt>")
                .Append(WebUtility.HtmlEncode(label))
                .Append("</dt><dd>")
                .Append(WebUtility.HtmlEncode(value))
                .Append("</dd>");
        }

        var html =
            "<!doctype html><html><head>"
            + "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">"
            + "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"
            + $"<title>{WebUtility.HtmlEncode(title)}</title>"
            + "<style>body{font:16px sans-serif;margin:2rem;line-height:1.5}"
            + "dt{font-weight:bold;margin-top:.75rem}dd{margin-left:0}</style>"
            + $"</head><body><h1>{WebUtility.HtmlEncode(title)}</h1><dl>{rows}</dl></body></html>";

        return Encoding.UTF8.GetBytes(html);
    }
}
