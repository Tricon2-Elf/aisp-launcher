#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/aisp.hook/aisp.hook.cpp"
RENDERER_SOURCE="$ROOT/aisp.hook/browser_renderer.cpp"
ELECTRON_APP="$ROOT/aisp.electron/app"
OUTPUT="${1:-$ROOT/aisp.launch/bin/publish/win-x86/aisp.hook.dll}"
DOCKER_IMAGE="${LOCALEHOOK_DOCKER_IMAGE:-debian:bookworm-slim}"

if [ ! -f "$SOURCE" ] || [ ! -f "$RENDERER_SOURCE" ] || [ ! -f "$ELECTRON_APP/main.js" ]; then
    echo "Browser hook source is incomplete." >&2
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

SOURCE_IN_CONTAINER="${SOURCE/#$ROOT/\/workspace}"
RENDERER_SOURCE_IN_CONTAINER="${RENDERER_SOURCE/#$ROOT/\/workspace}"
OUTPUT_IN_CONTAINER="${OUTPUT/#$ROOT/\/workspace}"
RUNTIME_DIR="$(dirname "$OUTPUT")/aisp.electron"
RUNTIME_DIR_IN_CONTAINER="${RUNTIME_DIR/#$ROOT/\/workspace}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# One-off containerized MinGW build (32-bit DLL for the x86 launcher/game).
docker run --rm \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$DOCKER_IMAGE" \
    bash -lc "set -euo pipefail \
        && apt-get update \
        && apt-get install -y --no-install-recommends g++-mingw-w64-i686 binutils-mingw-w64-i686 \
        && mkdir -p \"$RUNTIME_DIR_IN_CONTAINER\" \
        && i686-w64-mingw32-g++-posix -shared -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ \"$SOURCE_IN_CONTAINER\" -o \"$OUTPUT_IN_CONTAINER\" \
        && i686-w64-mingw32-g++-posix -shared -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ \"$RENDERER_SOURCE_IN_CONTAINER\" -lgdi32 -luser32 -o \"$RUNTIME_DIR_IN_CONTAINER/aisp.electron-renderer.dll\" \
        && i686-w64-mingw32-objdump -p \"$OUTPUT_IN_CONTAINER\" \
        && chown -R \"$HOST_UID:$HOST_GID\" \"$OUTPUT_IN_CONTAINER\" \"$RUNTIME_DIR_IN_CONTAINER\""

install -Dm644 "$ELECTRON_APP/main.js" "$RUNTIME_DIR/app/main.js"
install -Dm644 "$ELECTRON_APP/package.json" "$RUNTIME_DIR/app/package.json"

echo "Built Electron browser hook DLL: $OUTPUT"
