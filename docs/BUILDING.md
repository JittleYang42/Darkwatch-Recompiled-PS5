# Building Darkwatch Recompiled

There are three steps: generate the C translation of your copy of the game,
compile `darkwatch.exe`, then put it in a folder with your game files.

## What you need

| Tool | Used for | Notes |
| --- | --- | --- |
| Your Darkwatch (USA) Xbox disc files | everything | `default.xbe`, `darkwatch.uix` and the `.pck` files, copied off the disc |
| Python 3.9+ with [capstone](https://pypi.org/project/capstone/) | step 1 | `pip install capstone` |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (x86_64, UCRT) | step 2 | clang, lld and mingw-w64. Use the `ucrt-x86_64` build for your host |
| CMake 3.20+ and Ninja | step 2 | |

The build uses no other libraries. Vulkan is loaded at run time, and its
headers and stb are in `darkwatch_game/third_party`.

The code is tested with llvm-mingw (clang 18). MSVC builds aren't tested.

## 1. Generate the translation

```sh
python darkwatch_game/tools/generate.py <folder with default.xbe>
```

This runs the [xboxrecomp](../xboxrecomp) pipeline over your `default.xbe`,
with the settings the released builds use:

| Step | Takes | What it does |
| --- | --- | --- |
| `tools.xbe_parser` | seconds | reads the sections and the kernel imports |
| `tools.disasm` | ~15 min | finds every function: from the entry point, the call graph, jump tables, vtables and the seed lists in `xboxrecomp/config` |
| `tools.func_id` | seconds | names runtime and library functions by signature |
| `tools.abi_analysis` | ~1 min | calling conventions and stack frames |
| `tools.recomp` | ~3 min | writes C to `darkwatch_game/src/recomp/gen` (about 34,000 functions, 1,000 per file) |
| postgen | seconds | points two D3D calls at the hooks in `src/dw_patches.c` |

Intermediate files go to `xboxrecomp/tools/*/output`. They are derived from
your executable, so they're git-ignored, and so is the generated code. Keep
both out of anything you publish.

To re-lift without disassembling again (after changing the recompiler), use
`--skip-analysis`.

## 2. Compile

### Windows

Unzip llvm-mingw and put its `bin` folder on PATH. Install CMake and Ninja.
Then, from the repository root:

```bat
cmake -S darkwatch_game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=darkwatch_game/cmake/llvm-mingw.cmake
cmake --build build
```

### Linux or macOS (cross-compiling)

```sh
export PATH=/path/to/llvm-mingw/bin:$PATH
sh darkwatch_game/tools/build-win.sh        # -> build/darkwatch.exe
```

The generated code is ~3.2 million lines and is compiled at `-O1`. Expect
10-20 minutes on a few cores the first time. After that, only what you change
is rebuilt.

The build targets x86-64-v3 (AVX2, FMA, BMI2). `src/win/cpu_check.c` is
built for generic x86-64. On a CPU without AVX2 it shows a message and exits,
instead of crashing on an illegal instruction.

A linker map (`build/darkwatch.map`) is written beside the executable. Keep it
with any build you hand out. Crash reports in `darkwatch-log.txt` name frames
as `darkwatch.exe+0xOFFSET`, and the map, or `llvm-nm -n darkwatch.exe`,
turns those offsets back into functions.

## 3. Package

```sh
python darkwatch_game/tools/package.py build/darkwatch.exe Darkwatch
```

This copies `darkwatch.exe` and the player `README.txt` into `Darkwatch/`.
Copy your disc's files into `Darkwatch/game/`, or pick their folder in the
launcher under **Settings > General > Game folder**.

The launcher can show background art (`assets/launcher_*.png`). This repo
doesn't include any: without the art it draws a plain background. You can
add your own with `--assets <folder>`.

## Linux build (for development)

The runtime also builds as a Linux program. That build is used for automated
testing and for headless runs. It needs SDL2:

```sh
cmake -S darkwatch_game -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
cp build-linux/darkwatch darkwatch_game/ && cd darkwatch_game
DW_RENDERER=vulkan ./run_darkwatch.sh      # game files in darkwatch_game/game
```

`run_darkwatch.sh` sets the runtime switches the game needs. Its log goes to
`logs/`.
