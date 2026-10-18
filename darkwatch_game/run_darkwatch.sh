#!/bin/bash
# Run the Linux build of Darkwatch Recompiled (development and testing).
#
#   ./run_darkwatch.sh [seconds]        default: run until it exits
#   DW_RENDERER=vulkan ./run_darkwatch.sh   GPU renderer (src/gpu/nv2a_vulkan.c)
#
# Expects, next to this script:
#   darkwatch           the binary (DW_PORTABLE build)
#   game/               default.xbe, darkwatch.uix and the 19 .pck files
# Writes:
#   saves/              the Xbox hard disk (T:\, U:\, cache partitions)
#   frames/             rendered frames as BMP (when DW_FRAMES=1)
#   logs/run-<time>.log everything the runtime prints
set -u
cd "$(dirname "$0")"
mkdir -p saves logs
log="logs/run-$(date +%Y%m%d-%H%M%S).log"

export DW_GAME_DIR="${DW_GAME_DIR:-game}"
export DW_SAVE_DIR="${DW_SAVE_DIR:-saves}"

# What Darkwatch needs from the runtime (each one found during bring-up):
export RECOMP_AC97_READY=1     # emulated MCPX APU: DirectSound/XACT talk to it directly
export RECOMP_TITLE_KEVENTS=1  # XDK D3D builds its own KEVENTs (no KeInitializeEvent import)
export RECOMP_PB_EXEC=1        # execute the NV2A pushbuffer (CPU rasteriser for now)
export RECOMP_PB_WALK=${RECOMP_PB_WALK:-dma}   # walk the DMA pushbuffer: D3D fence waits need its kick marker
export RECOMP_HEAP_RECLAIM=${RECOMP_HEAP_RECLAIM:-1}   # reuse freed heap: the level load runs out of the 48 MB heap without it
export RECOMP_VBLANK=1         # 60 Hz vblank interrupt: D3D's present path waits on it
# RECOMP_USB=1: the USB controllers XAPI drives (the gamepad). Not on yet:
# XAPI enumeration stalls the boot (work in progress).

# Renderer: DW_RENDERER=vulkan rasterises through Vulkan (src/gpu). The
# system driver is used (libvulkan.so.1); with no GPU, lib/libvk_swiftshader.so
# (a CPU Vulkan driver) stands in, which is what the headless VM uses.
if [ "${DW_RENDERER:-}" = vulkan ] && [ -z "${DW_VK_LIB:-}" ]; then
    if ! ldconfig -p 2>/dev/null | grep -q libvulkan.so.1 && [ -f lib/libvk_swiftshader.so ]; then
        export DW_VK_LIB="$PWD/lib/libvk_swiftshader.so"
    fi
fi
export DW_RENDERER="${DW_RENDERER:-}"

# A display (WSLg sets DISPLAY): open the game window and play sound.
# None (the headless VM): draw nowhere, but keep the frames.
# DW_HEADLESS=1 forces headless even with a display.
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || [ "${DW_HEADLESS:-0}" = 1 ]; then
    export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy RECOMP_HEADLESS=1
else
    export RECOMP_FB_WINDOW=1
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    for l in libX11.so.6 libXext.so.6 libpulse.so.0; do
        ldconfig -p 2>/dev/null | grep -q "$l" || missing="${missing:-} $l"
    done
    if [ -n "${missing:-}" ]; then
        echo "missing:$missing  (window/sound need: sudo apt install libx11-6 libxext6 libpulse0)"
    fi
fi
if [ "${DW_FRAMES:-0}" = 1 ]; then
    mkdir -p frames
    export RECOMP_FB_DUMP=frames/frame_ RECOMP_FB_DUMP_FLIPS=1
fi
[ -n "${1:-}" ] && export RECOMP_WATCHDOG_SECS="$1"

echo "log: $log"
./darkwatch > "$log" 2>&1
status=$?
echo "exit $status"
grep -E '^\[CRASH\]|^\[WATCHDOG\]' -A4 "$log" | head -12
exit $status
