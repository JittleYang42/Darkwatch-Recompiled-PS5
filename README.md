# Darkwatch Recompiled

A native Windows PC port of **Darkwatch** (Xbox, 2005), made by static
recompilation. The game's own x86 code is translated from the Xbox executable
into C and compiled for the PC. It runs on a reimplementation of the parts of
the Xbox it talks to: the kernel, the NV2A GPU (drawn with Vulkan) and the
MCPX audio chip. No emulator is involved.

> **You need your own copy of the game.** This repository contains no game
> code, data or artwork. The C translation of the game is generated on your
> machine from the `default.xbe` on your own disc (see [Building](#building)).

## Features

- Native 64-bit Windows executable with a Vulkan renderer
- Internal resolution from 1x to 4x the console's 640x480 (up to 2560x1920),
  with a 4:3 or stretched picture
- Frame interpolation up to your display's refresh rate (the game logic keeps
  its original timing), a frame-rate limit from 30 to 360 fps or unlimited,
  and V-Sync on or off
- Mouse and keyboard with raw mouse input and rebindable keys
- Xbox controllers (XInput) with button remapping and rumble.
  PlayStation controllers work through Steam Input or DS4Windows.
- A launcher with Graphics, Controls, Sound and General settings, and an
  in-game settings overlay (F1)
- Fog and near/far depth culling, as on the console
- F12 frame capture for bug reports: a PNG plus a list of every draw in that
  frame

## Not done yet

- Texture stages 1-3 and the register combiners aren't passed to the
  renderer yet. Surfaces are drawn with their base texture, vertex colour and
  fog, so lightmaps, detail maps and environment maps are missing.
- No mipmaps or anisotropic filtering
- Points and lines aren't drawn
- Multiplayer and Xbox Live aren't supported

## Requirements

**To play**
- Windows 10 or 11, 64-bit
- A CPU with AVX2: Intel Haswell (2013), AMD Zen, or newer
- A GPU with Vulkan 1.2
- The files from your Darkwatch (USA) Xbox disc: `default.xbe`,
  `darkwatch.uix` and the `.pck` files

**To build**: see [docs/BUILDING.md](docs/BUILDING.md). You need Python 3 with
`capstone`, CMake, Ninja and llvm-mingw.

## Building

```sh
# 1. Translate your copy of the game into C (about 20 minutes)
pip install capstone
python darkwatch_game/tools/generate.py path/to/your/game/files

# 2. Build darkwatch.exe (llvm-mingw on PATH)
cmake -S darkwatch_game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=darkwatch_game/cmake/llvm-mingw.cmake
cmake --build build

# 3. Put it in a folder of its own
python darkwatch_game/tools/package.py build/darkwatch.exe Darkwatch
```

Copy your disc's files into a folder named `game` next to `darkwatch.exe`, or
choose their folder in **Settings > General > Game folder**. Then start
`darkwatch.exe`. [docs/BUILDING.md](docs/BUILDING.md) has the details: Windows
and Linux hosts, and what each step does.

## Playing

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| W A S D | move | Mouse | look / aim |
| Left mouse | shoot | Right mouse | sight / zoom |
| Space | jump (twice: vampire jump) | E / Enter | interact, confirm |
| R / Backspace | reload, menu back | F / V | melee |
| G / Mouse 4 | throw | Q | blood power |
| T | cycle blood powers | Left Ctrl / C | crouch |
| Wheel / 1, 2 | cycle guns / thrown weapons | Z / X | lean left / right |
| Esc | pause | Tab | score |
| F1 | settings overlay | F11, Alt+Enter | fullscreen / window |
| F9 | free the mouse | F12 | frame capture |

You can rebind every key in **Controls**. The player readme
([darkwatch_game/release/README.txt](darkwatch_game/release/README.txt)) ships
next to the executable and describes the settings.

## How it works

1. **Translation.** [xboxrecomp](xboxrecomp/) finds every function in the
   Xbox executable: from the entry point, the call graph, jump tables, vtables
   and a list of seeds. It then lifts each one to C over a register context,
   with guest memory mapped as one block so a guest address is an offset.
   That gives about 34,000 functions and 3.2 million lines of C.
2. **Runtime.** The 151 kernel functions the game imports are reimplemented
   on Windows: threads, events, files (the disc and the hard disk as folders
   and images), memory and the timers. Hardware the game programs directly is
   modelled in software:
   - the NV2A's pushbuffer is executed command by command
   - vertex programs run on the CPU, eight vertices at a time on AVX2 and
     across several threads
   - the MCPX APU's voice processor and DSP mix the game's sound, played
     through XAudio2
3. **Renderer.** The executor hands transformed triangles, render state,
   textures and fog to a Vulkan back end ([nv2a_vulkan.c](darkwatch_game/src/gpu/nv2a_vulkan.c)).
   The back end draws at the internal resolution and keeps guest memory in
   sync with the console-sized picture. A presenter thread interpolates
   between game frames up to the display's refresh rate.
4. **Host.** A Win32 window, the launcher and settings, raw mouse and XInput
   input mapped onto the Xbox controller the game reads, and a crash report
   in the log ([darkwatch_game/src/win](darkwatch_game/src/win)).

[docs/TECHNICAL.md](docs/TECHNICAL.md) goes further and lists the runtime's
switches.

## Repository layout

```
darkwatch_game/          the game project
  src/win/               Windows host: window, launcher, settings, input glue
  src/gpu/               Vulkan renderer and its shaders
  src/                   game-specific hooks (dw_patches.c), Linux host
  src/recomp/gen/        generated C (made by tools/generate.py, not in git)
  tools/                 generate.py, package.py, build-win.sh
  cmake/                 llvm-mingw toolchain file
  release/README.txt     the player readme that ships with the exe
  third_party/           stb, Vulkan headers
xboxrecomp/              the recompiler and runtime toolkit
docs/                    building and technical notes
```

## Credits

- **xboxrecomp**: the static recompiler and Xbox runtime this port is built
  on (MIT). It includes components from [xemu](https://github.com/xemu-project/xemu)
  under the LGPL-2.1; see [xboxrecomp/NOTICE](xboxrecomp/NOTICE).
- [stb_image and stb_image_write](https://github.com/nothings/stb) (public
  domain / MIT)
- [Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers)
  (Apache-2.0 / MIT)
- [Cinzel](https://fonts.google.com/specimen/Cinzel) and
  [Lato](https://fonts.google.com/specimen/Lato) fonts (SIL Open Font License 1.1)

## Legal

Darkwatch was developed by High Moon Studios and published by Capcom in North
America. This is
an unofficial fan project and isn't affiliated with or endorsed by them. No
game code, data or artwork is included or distributed here. Use it only with
a copy of the game you own.
