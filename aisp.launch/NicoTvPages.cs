using System.Net;
using System.Text;

namespace aisp.launch;

internal static class NicoTvPages
{
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
