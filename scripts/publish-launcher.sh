#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/aisp.launch/aisp.launch.csproj"
HOOK_BUILDER="$ROOT/scripts/build-hook.sh"
WIN_PUBLISH_DIR="$ROOT/aisp.launch/bin/publish/win-x86"
WEBSITE_URL="${AISP_WEBSITE_URL:-https://aisp.moe}"
GITHUB_REPO="${AISP_GITHUB_REPO:-Tricon2-Elf/aisp-launcher}"

dotnet publish "$PROJECT" -c Release \
    -p:PublishProfile="win-x86" \
    -p:DefaultWebsiteUrl="$WEBSITE_URL" \
    -p:DefaultGitHubRepo="$GITHUB_REPO"
"$HOOK_BUILDER" "$WIN_PUBLISH_DIR/aisp.hook.dll"

echo "Published to aisp.launch/bin/publish/win-x86/"
