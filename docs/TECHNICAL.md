# How Darkwatch Recompiled works

## The pieces

```
default.xbe --(xboxrecomp: disasm, recomp)--> src/recomp/gen/*.c   the game, as C
                                               xboxrecomp/src       the Xbox it expects
                                               darkwatch_game/src   this game's host and renderer
```

### The translated game

Every guest function is a C function, `sub_XXXXXXXX`, named by its Xbox
address. It runs over a register context (`eax` to `edi`, flags computed only
when they're read, the x87 stack, SSE). Guest memory is one reserved block,
so a guest pointer is an offset into it. Indirect calls go through an
address-to-function table. A miss is logged as `[ICALL] Failed to resolve VA`
and skipped. The fix for a miss is a seed in
`xboxrecomp/config/seed_functions.json`, then generating again.

`src/dw_patches.c` wraps a handful of game functions with the linker's
`--wrap`, using the list in `CMakeLists.txt`. Among them are D3D's KickOff,
which hands GPU work to the executor (see below), and the network library,
which is answered as "no network".

### The runtime (xboxrecomp)

- **Kernel**: the 151 imports the game uses (threads, events, DPCs, timers,
  files and memory). The disc (`D:`) is your game folder. The hard disk
  partitions are folders and images under `saves`.
- **NV2A executor** (`src/kernel/nv2a_pb_exec.c`, `nv2a_pb_scan.c`): it walks
  the GPU pushbuffer the way the DMA engine does: JUMP, CALL and RETURN, kick
  markers, fence releases. It keeps the 3D class's state and gathers each
  batch's indices.
  - Fixed-function vertices are transformed on the CPU.
  - Vertex programs run through an interpreter (`nv2a_vsh_interp.c`), eight
    vertices at a time with AVX2.
  - Big batches are split across worker threads. Each batch-size class
    measures whether splitting pays and decides for itself.
  - Each vertex is handed to the renderer once, in homogeneous clip space,
    with an index list (`NV2A_BACKEND_INDEXED`).
- **APU** (`src/apu`): the MCPX voice processor (ADPCM and PCM voices,
  envelopes, the stream lists DirectSound and XACT feed) and the
  global/encode DSPs, mixed to stereo through XAudio2. Gain changes are
  ramped across each 32-sample frame.
- **Input** (`src/input`): raw mouse and keyboard, and XInput. Both are mapped
  onto the Xbox controller the game polls.

### The Vulkan renderer (`darkwatch_game/src/gpu/nv2a_vulkan.c`)

- Every guest render target has a Vulkan image at the internal resolution
  (1x-4x). Guest memory gets the console-sized picture back at each flush,
  so render-to-texture and anything the CPU reads still work.
- The vertex shader takes the executor's homogeneous vertices. The GPU clips,
  including the near/far culling the game asks for with
  `SET_ZMIN_MAX_CONTROL`.
- The fragment shader does texture stage 0 times the diffuse colour, the
  alpha test, and D3D's fog (linear, exp or exp2, from the vertex fog
  factor).
- Textures are decoded once and cached by content checksum: swizzled,
  linear, DXT1/3/5, palettised and YUV formats.
- **Frame interpolation**: each finished frame's draws are recorded and
  published to a presenter thread. The presenter runs at the display's
  rate. It pairs every draw with the same draw in the previous frame, using
  the batch key, its size and where the camera motion fitted from the stable
  draws puts it. It then re-renders the scene at in-between times. A pair
  whose vertices disagree too much is shown as drawn instead.

### The Windows host (`darkwatch_game/src/win`)

The window and message pump run on the main thread, and the game on a
thread with a 64 MB stack. The host also provides borderless fullscreen,
the launcher and settings (drawn in software), the F1 overlay, F12 frame
capture, and a crash handler. Crash reports write
`darkwatch.exe+offset` frames to `darkwatch-log.txt`.

## Switches

The launcher writes `darkwatch.ini` and turns it into the runtime's
environment switches. An `env.txt` next to the executable (`KEY=VALUE` lines)
overrides any of them. Useful ones:

| Switch | Default | |
| --- | --- | --- |
| `DW_RES_SCALE` | from the settings | internal resolution 1-4 |
| `DW_INTERP` | 1 | frame interpolation |
| `DW_FOG` | 1 | `0` draws without the game's fog |
| `DW_KICK_LAG` | 1 | how many GPU kicks the game may run ahead of the executor |
| `RECOMP_XF_THREADS` | CPUs/4, at most 7 | vertex worker threads |
| `RECOMP_XF_ADAPT` | 1 | `0`: always split big batches |
| `RECOMP_NO_INDEXED` | unset | hand the renderer triangle lists instead of indexed batches |
| `RECOMP_XF_REPORT_FLIPS` | 600 | how often the executor's timing report is logged |
| `RECOMP_APU_NO_RAMP` | unset | apply volume changes in steps, as before the ramp |

## Reading the log

`darkwatch-log.txt` reports what the frame's time went to:

- `[VK] last 60 frames: ...` gives the wall time per frame, the renderer's
  draw recording, texture decode and checksums, and flushes.
- `[GPU] executor (600 flips): busy ... triangle setup, back-end draws, vertex
  programs, flip, the rest` gives the executor's time per frame. The next line
  shows how vertex batches were split, and the one after that which
  pushbuffer methods carried the most words.
- `[VK] interp 10s: ...` shows how many frames the presenter showed and
  blended, how draws were paired, and how often pairs were refused.
- `[APU] 10s: ...` covers the audio voices, stream lists, clicks and
  underruns.
