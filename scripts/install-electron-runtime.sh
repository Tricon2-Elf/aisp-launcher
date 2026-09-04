#!/usr/bin/env bash
# Stock Electron (win32-x64) next to the game for electron:<url> screens. The 32-bit hook
# launches electron.exe as a separate process; 64-bit Chromium is fine (and is what has H.264).
# App sources live in aisp.electron/app/; this script unpacks the official zip and copies them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${AISP_ELECTRON_VERSION:-44.1.1}"
ARCH="${AISP_ELECTRON_ARCH:-win32-x64}"
DESTINATION="${1:-$ROOT/aisp.launch/bin/publish/win-x64/aisp.electron}"
CACHE="${AISP_ELECTRON_CACHE:-$HOME/.cache/aisp-electron}"
ARCHIVE="electron-v${VERSION}-${ARCH}.zip"
BASE_URL="https://github.com/electron/electron/releases/download/v${VERSION}"

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$ARCHIVE" ]; then
    echo "Downloading $ARCHIVE ..."
    curl --fail --location --retry 3 --output "$CACHE/$ARCHIVE.part" "$BASE_URL/$ARCHIVE"
    curl --fail --location --retry 3 --output "$CACHE/SHASUMS256.txt" "$BASE_URL/SHASUMS256.txt"
    expected="$(grep " \*${ARCHIVE}$" "$CACHE/SHASUMS256.txt" | awk '{print $1}')"
    if [ -z "$expected" ]; then
        echo "No SHA256 entry for $ARCHIVE in SHASUMS256.txt" >&2
        exit 1
    fi
    echo "$expected  $CACHE/$ARCHIVE.part" | sha256sum --check --strict
    mv "$CACHE/$ARCHIVE.part" "$CACHE/$ARCHIVE"
fi

if [[ "$DESTINATION" != /* ]]; then
    DESTINATION="$ROOT/$DESTINATION"
fi

mkdir -p "$DESTINATION"
unzip -q -o "$CACHE/$ARCHIVE" -d "$DESTINATION"
install -Dm644 "$ROOT/aisp.electron/app/main.js" "$DESTINATION/app/main.js"
install -Dm644 "$ROOT/aisp.electron/app/package.json" "$DESTINATION/app/package.json"

echo "Installed Electron $VERSION ($ARCH) runtime: $DESTINATION"
