#!/usr/bin/env python3
"""Translate your copy of Darkwatch into C: darkwatch_game/src/recomp/gen.

    python darkwatch_game/tools/generate.py <game folder or default.xbe>

The game's executable is not part of this repository. This runs the
xboxrecomp pipeline over the default.xbe from your own disc, with the settings
the released builds were made with:

  1. tools.xbe_parser   the XBE's sections and imports
  2. tools.disasm       every function, from the entry point, the call graph,
                        jump tables, vtables and the seed lists in
                        xboxrecomp/config (about 15 minutes)
  3. tools.func_id      library / runtime functions by signature
  4. tools.abi_analysis calling conventions
  5. tools.recomp       C, 1000 functions per file (a few minutes)
  6. postgen            two D3D calls pointed at the hooks in src/dw_patches.c

Needs Python 3.9+ and capstone (pip install capstone). Expect about 34,000
functions lifted, 0 failed. Intermediate files go to xboxrecomp/tools/*/output
(git-ignored).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)                                  # darkwatch_game
DEFAULT_TOOLKIT = os.path.join(os.path.dirname(GAME), "xboxrecomp")

# The D3D KickOff functions src/dw_patches.c hooks. The linker's --wrap only
# redirects calls between files; calls inside the one generated file that
# defines them are pointed at the wrappers here.
WRAPPED_SAME_FILE = ("sub_003485D0", "sub_00348680")


def run(step, args, cwd):
    print(f"==> {step}", flush=True)
    t0 = time.time()
    r = subprocess.run([sys.executable, "-m"] + args, cwd=cwd)
    if r.returncode != 0:
        sys.exit(f"{step} failed (exit {r.returncode})")
    print(f"    {step}: {time.time() - t0:.0f} s", flush=True)


def postgen(gen_dir):
    decls = "".join(f"void __wrap_{f}(void);\n" for f in WRAPPED_SAME_FILE)
    changed = 0
    for name in sorted(os.listdir(gen_dir)):
        if not (name.startswith("recomp_") and name.endswith(".c")):
            continue
        path = os.path.join(gen_dir, name)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        out = src
        for fn in WRAPPED_SAME_FILE:
            addr = fn[len("sub_"):]
            out = out.replace(f"RECOMP_ABI_CALL(0x{addr}u, {fn})",
                              f"RECOMP_ABI_CALL(0x{addr}u, __wrap_{fn})")
        if out == src:
            continue
        if decls not in out:
            # Declared before the file's first function.
            m = re.search(r"^void sub_", out, re.M)
            if not m:
                sys.exit(f"postgen: no function definition in {name}")
            out = out[:m.start()] + decls + "\n" + out[m.start():]
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
            f.write(out)
        changed += 1
        print(f"    postgen: {name}: KickOff calls -> __wrap_")
    if not changed:
        sys.exit("postgen: no KickOff call sites found -- is this the Darkwatch (USA) default.xbe?")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("game", help="folder with default.xbe (your disc's files), or the .xbe itself")
    ap.add_argument("--toolkit", default=DEFAULT_TOOLKIT, help="xboxrecomp folder (default: ../xboxrecomp)")
    ap.add_argument("--gen-dir", default=os.path.join(GAME, "src", "recomp", "gen"),
                    help="where the C goes (default: darkwatch_game/src/recomp/gen)")
    ap.add_argument("--skip-analysis", action="store_true",
                    help="reuse the last run's disassembly (steps 1-4) and only re-lift")
    a = ap.parse_args()

    xbe = a.game
    if os.path.isdir(xbe):
        xbe = os.path.join(xbe, "default.xbe")
    xbe = os.path.abspath(xbe)
    if not os.path.isfile(xbe):
        sys.exit(f"No default.xbe at {xbe}")
    toolkit = os.path.abspath(a.toolkit)
    if not os.path.isfile(os.path.join(toolkit, "tools", "recomp", "__main__.py")):
        sys.exit(f"No xboxrecomp toolkit at {toolkit}")
    try:
        import capstone  # noqa: F401
    except ImportError:
        sys.exit("capstone is missing: pip install capstone")

    cfg = os.path.join(toolkit, "config")
    work = os.path.join(toolkit, "tools", "xbe_parser", "output")
    os.makedirs(work, exist_ok=True)
    analysis = os.path.join(work, "default_analysis.json")
    if not a.skip_analysis:
        run("xbe_parser", ["tools.xbe_parser", xbe, "--json", analysis], toolkit)
        # All code sections, not --text-only: D3D, DSOUND, XACT and the
        # network library live outside .text and the game calls into them.
        run("disasm", ["tools.disasm", xbe, "--force", "--analysis-json", analysis,
                       "--seed-functions", os.path.join(cfg, "seed_functions.json"),
                       "--seed-functions", os.path.join(cfg, "merged_seeds.json")], toolkit)
        run("func_id", ["tools.func_id", xbe], toolkit)
        run("abi_analysis", ["tools.abi_analysis", xbe], toolkit)

    gen = os.path.abspath(a.gen_dir)
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    os.makedirs(gen)
    run("recomp", ["tools.recomp", xbe, "--game-name", "Darkwatch", "--all",
                   "--split", "1000", "--gen-dir", gen], toolkit)
    print("==> postgen", flush=True)
    postgen(gen)
    n = len([f for f in os.listdir(gen) if f.endswith(".c")])
    print(f"Done: {n} files in {gen}. Next: build (docs/BUILDING.md).")


if __name__ == "__main__":
    main()
