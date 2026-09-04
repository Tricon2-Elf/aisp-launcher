#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${AISP_ELECTRON_VERSION:-44.1.1}"
DESTINATION="${1:-$ROOT/aisp.launch/bin/publish/win-x86/aisp.electron}"
ARCHIVE="electron-v${VERSION}-win32-x64.zip"
BASE_URL="https://github.com/electron/electron/releases/download/v${VERSION}"
TEMP_DIRECTORY="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIRECTORY"' EXIT

curl --fail --location --retry 3 --output "$TEMP_DIRECTORY/$ARCHIVE" "$BASE_URL/$ARCHIVE"
curl --fail --location --retry 3 --output "$TEMP_DIRECTORY/SHASUMS256.txt" "$BASE_URL/SHASUMS256.txt"
(
    cd "$TEMP_DIRECTORY"
    grep " \*${ARCHIVE}$" SHASUMS256.txt | sha256sum --check --strict
)

mkdir -p "$DESTINATION"
unzip -q -o "$TEMP_DIRECTORY/$ARCHIVE" -d "$DESTINATION"
install -Dm644 "$ROOT/aisp.electron/app/main.js" "$DESTINATION/app/main.js"
install -Dm644 "$ROOT/aisp.electron/app/package.json" "$DESTINATION/app/package.json"

echo "Installed Electron $VERSION runtime: $DESTINATION"
