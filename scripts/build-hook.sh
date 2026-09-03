#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/aisp.hook/aisp.hook.cpp"
RENDERER_SOURCE="$ROOT/aisp.hook/cef_renderer.c"
SUBPROCESS_SOURCE="$ROOT/aisp.hook/cef_subprocess.c"
OUTPUT="${1:-$ROOT/aisp.launch/bin/publish/win-x64/aisp.hook.dll}"
DOCKER_IMAGE="${LOCALEHOOK_DOCKER_IMAGE:-debian:bookworm-slim}"
CEF_ROOT="${AISP_CEF_ROOT:-/home/tricon2_elf/.cache/aisp-cef/extracted/cef_binary_144.0.34+g8fc21c8+chromium-144.0.7559.261_windows32}"

if [ ! -f "$SOURCE" ] || [ ! -f "$RENDERER_SOURCE" ] || [ ! -f "$SUBPROCESS_SOURCE" ]; then
    echo "Browser hook source is incomplete." >&2
    exit 1
fi

if [ ! -f "$CEF_ROOT/Release/libcef.lib" ]; then
    echo "CEF x86 distribution not found at: $CEF_ROOT" >&2
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
SUBPROCESS_SOURCE_IN_CONTAINER="${SUBPROCESS_SOURCE/#$ROOT/\/workspace}"
OUTPUT_IN_CONTAINER="${OUTPUT/#$ROOT/\/workspace}"
RUNTIME_DIR="$(dirname "$OUTPUT")/aisp.cef"
RUNTIME_DIR_IN_CONTAINER="${RUNTIME_DIR/#$ROOT/\/workspace}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# One-off containerized MinGW build (32-bit DLL for the x86 launcher/game).
docker run --rm \
    -v "$ROOT:/workspace" \
    -v "$CEF_ROOT:/cef:ro" \
    -w /workspace \
    "$DOCKER_IMAGE" \
    bash -lc "set -euo pipefail \
        && apt-get update \
        && apt-get install -y --no-install-recommends g++-mingw-w64-i686 binutils-mingw-w64-i686 \
        && mkdir -p \"$RUNTIME_DIR_IN_CONTAINER\" \
        && i686-w64-mingw32-g++-posix -shared -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ \"$SOURCE_IN_CONTAINER\" -o \"$OUTPUT_IN_CONTAINER\" \
        && i686-w64-mingw32-g++-posix -shared -O2 -s -std=gnu++17 -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++ -I/cef \"$RENDERER_SOURCE_IN_CONTAINER\" -L/cef/Release -lcef -lgdi32 -luser32 -lshell32 -luuid -o \"$RUNTIME_DIR_IN_CONTAINER/aisp.cef-renderer.dll\" \
        && i686-w64-mingw32-gcc-posix -O2 -s -mwindows -std=gnu11 -I/cef \"$SUBPROCESS_SOURCE_IN_CONTAINER\" -L/cef/Release -lcef -luser32 -o \"$RUNTIME_DIR_IN_CONTAINER/aisp.cef-subprocess.exe\" \
        && i686-w64-mingw32-objdump -p \"$OUTPUT_IN_CONTAINER\" \
        && chown -R \"$HOST_UID:$HOST_GID\" \"$OUTPUT_IN_CONTAINER\" \"$RUNTIME_DIR_IN_CONTAINER\""

echo "Built CEF browser hook DLL: $OUTPUT"
