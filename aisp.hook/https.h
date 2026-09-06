// HTTPS for the client's web traffic, which the client itself can only do in the clear:
// the drama and AI-tune uploads are an in-house HTTP/1.1 writer on a VCE TCP stream to port 80,
// and download.php is a WinINet POST with the secure flag off. With [web] https on (auto: when
// connection.txt names one of https_hosts) the hook forces TLS on both paths without the client
// noticing; see the comment in https.cpp.
#pragma once

namespace aisp
{
// [web] https in aisp.hook.ini (or AISP_HTTPS): whether upload.php and download.php go over
// TLS. Read once.
bool HttpsEnabled();

// [screens] https (or AISP_SCREEN_HTTPS): whether the screen pages at `host` go over TLS; auto
// means when the host is one of [web] https_hosts.
bool ScreensHttpsEnabled(const wchar_t* host);

// Patches the game executable's WinINet imports for download.php and CAIProtoAuth's connect
// slot for upload.php, each only when its endpoint is on; logs one line per endpoint either way.
void PatchHttps();
} // namespace aisp
