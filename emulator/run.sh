#!/usr/bin/env bash
#
# Interactive, host-controlled Mass Effect Infiltrator emulator.
#
# The game still runs through qemu-arm and Mesa/llvmpipe, as in the immutable
# verifier, but it has no fixed frame limit and mounts a bidirectional control
# directory. emulator_control.cpp consumes commands from that directory and
# writes PNG screenshots/status back to it.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${MASSEFFECT_GAMEDIR:-$PORT_DIR/../NeededFiles/data/masseffect}"
CONTROL_DIR="${MASSEFFECT_CONTROL_DIR:-$PORT_DIR/emulator/runtime}"
IMAGE="${MASSEFFECT_BUILD_IMAGE:-masseffect-build}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --game-dir)
            GAME_DIR="${2:?--game-dir needs a path}"
            shift 2
            ;;
        --control-dir)
            CONTROL_DIR="${2:?--control-dir needs a path}"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$GAME_DIR/lib/armeabi/libMassEffect.so" ]; then
    echo "game tree not found at $GAME_DIR" >&2
    exit 2
fi

mkdir -p "$CONTROL_DIR/screenshots"
: > "$CONTROL_DIR/commands"
rm -f "$CONTROL_DIR/status.json" "$CONTROL_DIR/status.json.tmp"

if [ "${MASSEFFECT_EMULATOR_SKIP_BUILD:-0}" != "1" ]; then
    docker run --rm \
        -v "$PORT_DIR":/src \
        -w /src \
        "$IMAGE" make -j4
fi

exec docker run --rm \
    -v "$PORT_DIR":/src \
    -v "$GAME_DIR":/game:ro \
    -v "$CONTROL_DIR":/control \
    -w /src \
    -e SDL_VIDEODRIVER=offscreen \
    -e SDL_AUDIODRIVER=dummy \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e EGL_PLATFORM=surfaceless \
    -e LOADER_TRACE=1 \
    -e MASSEFFECT_AUTOPILOT="${MASSEFFECT_AUTOPILOT:-}" \
    -e MASSEFFECT_FRAME_LIMIT="${MASSEFFECT_FRAME_LIMIT:-}" \
    -e MASSEFFECT_NO_VFP_PATCH="${MASSEFFECT_NO_VFP_PATCH:-}" \
    -e MASSEFFECT_VFP_SELFTEST="${MASSEFFECT_VFP_SELFTEST:-}" \
    -e MASSEFFECT_CONTROL_DIR=/control \
    -e MASSEFFECT_GL_DIAG="${MASSEFFECT_GL_DIAG:-}" \
    -e MASSEFFECT_MOTION_TRACE_ALL="${MASSEFFECT_MOTION_TRACE_ALL:-}" \
    -e MASSEFFECT_MOTION_NO_GATE="${MASSEFFECT_MOTION_NO_GATE:-}" \
    -e MASSEFFECT_CAMERA_NO_SWIPE="${MASSEFFECT_CAMERA_NO_SWIPE:-}" \
    -e MASSEFFECT_TRIGGER_ACCEL="${MASSEFFECT_TRIGGER_ACCEL:-}" \
    -e MASSEFFECT_FORCE_SOFTWARE_TEXTURE_DECODE="${MASSEFFECT_FORCE_SOFTWARE_TEXTURE_DECODE:-}" \
    -e MASSEFFECT_S3TC_DXT3_OFF="${MASSEFFECT_S3TC_DXT3_OFF:-}" \
    -e MASSEFFECT_CURSOR_DIAG="${MASSEFFECT_CURSOR_DIAG:-}" \
    -e MASSEFFECT_CURSOR_RESTORE="${MASSEFFECT_CURSOR_RESTORE:-}" \
    -e MASSEFFECT_EGL_PRESERVE="${MASSEFFECT_EGL_PRESERVE:-}" \
    "$IMAGE" \
    qemu-arm -L /usr/arm-linux-gnueabihf \
        ./build/masseffect /game
