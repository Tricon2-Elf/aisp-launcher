#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/aisp.launch/aisp.launch.csproj"
HOOK_BUILDER="$ROOT/scripts/build-hook.sh"
WIN_PUBLISH_DIR="$ROOT/aisp.launch/bin/publish/win-x86"

dotnet publish "$PROJECT" -c Release -p:PublishProfile="win-x86"
"$HOOK_BUILDER" "$WIN_PUBLISH_DIR/aisp.hook.dll"

echo "Published to aisp.launch/bin/publish/win-x86/"
