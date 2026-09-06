#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="$ROOT/aisp.hook"
OUTPUT="${1:-$ROOT/aisp.launch/bin/publish/win-x64/aisp.hook.dll}"
DOCKER_IMAGE="${LOCALEHOOK_DOCKER_IMAGE:-debian:bookworm-slim}"

if [ ! -f "$SOURCE_DIR/aisp.hook.cpp" ]; then
    echo "Locale hook source not found: $SOURCE_DIR" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to build the locale hook DLL." >&2
    exit 1
fi

if [[ "$OUTPUT" != /* ]]; then
    OUTPUT="$ROOT/$OUTPUT"
fi

case "$OUTPUT" in
    "$ROOT"/*) ;;
    *)
        echo "Output path must be inside repository root: $ROOT" >&2
        exit 1
        ;;
esac

mkdir -p "$(dirname "$OUTPUT")"

SOURCE_DIR_IN_CONTAINER="${SOURCE_DIR/#$ROOT/\/workspace}"
OUTPUT_IN_CONTAINER="${OUTPUT/#$ROOT/\/workspace}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# attach.cpp is a helper exe, not part of the DLL. ws2_32 is for browser.cpp's loopback sockets.
HOOK_SOURCES="$SOURCE_DIR_IN_CONTAINER/aisp.hook.cpp $SOURCE_DIR_IN_CONTAINER/browser.cpp $SOURCE_DIR_IN_CONTAINER/screen.cpp $SOURCE_DIR_IN_CONTAINER/source.cpp $SOURCE_DIR_IN_CONTAINER/source_electron.cpp $SOURCE_DIR_IN_CONTAINER/source_ffmpeg.cpp $SOURCE_DIR_IN_CONTAINER/source_pattern.cpp $SOURCE_DIR_IN_CONTAINER/tv_panel.cpp $SOURCE_DIR_IN_CONTAINER/https.cpp $SOURCE_DIR_IN_CONTAINER/config.cpp $SOURCE_DIR_IN_CONTAINER/document.cpp"
ATTACH_OUTPUT="$(dirname "$OUTPUT")/aisp.attach.exe"
ATTACH_OUTPUT_IN_CONTAINER="${ATTACH_OUTPUT/#$ROOT/\/workspace}"

# One-off containerized MinGW build: a 32-bit DLL for the x86 launcher/game.
docker run --rm \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$DOCKER_IMAGE" \
    bash -lc "set -euo pipefail \
        && apt-get update \
        && apt-get install -y --no-install-recommends g++-mingw-w64-i686 binutils-mingw-w64-i686 \
        && i686-w64-mingw32-g++ -shared -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ $HOOK_SOURCES -o \"$OUTPUT_IN_CONTAINER\" -loleaut32 -lgdi32 -lole32 -lws2_32 -lwinhttp \
        && i686-w64-mingw32-g++ -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ $SOURCE_DIR_IN_CONTAINER/attach.cpp -o \"$ATTACH_OUTPUT_IN_CONTAINER\" -lshell32 \
        && i686-w64-mingw32-objdump -p \"$OUTPUT_IN_CONTAINER\" \
        && chown \"$HOST_UID:$HOST_GID\" \"$OUTPUT_IN_CONTAINER\" \"$ATTACH_OUTPUT_IN_CONTAINER\""

echo "Built locale hook DLL: $OUTPUT"
echo "Built attach helper: $ATTACH_OUTPUT"
echo "Electron runtime for electron: sources: ./scripts/install-electron-runtime.sh."
