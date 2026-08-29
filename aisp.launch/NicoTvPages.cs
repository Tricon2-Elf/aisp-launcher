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
