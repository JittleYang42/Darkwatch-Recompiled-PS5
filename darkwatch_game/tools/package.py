#!/usr/bin/env python3
"""Put a built darkwatch.exe into a folder ready to play or to zip.

    python darkwatch_game/tools/package.py build/darkwatch.exe <folder>

Copies the executable and the player readme (release/README.txt). The game
files are not copied: they go in <folder>/game, from your own disc. Optional
launcher pictures (assets/launcher_*.png) are copied from --assets when given.
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe", help="the built darkwatch.exe")
    ap.add_argument("folder", help="where to put it (made if missing)")
    ap.add_argument("--assets", help="a folder with launcher_*.png to copy into <folder>/assets")
    a = ap.parse_args()

    if not os.path.isfile(a.exe):
        sys.exit(f"No executable at {a.exe}")
    os.makedirs(a.folder, exist_ok=True)
    shutil.copy2(a.exe, os.path.join(a.folder, "darkwatch.exe"))
    shutil.copy2(os.path.join(GAME, "release", "README.txt"), os.path.join(a.folder, "README.txt"))
    if a.assets:
        dst = os.path.join(a.folder, "assets")
        os.makedirs(dst, exist_ok=True)
        for name in sorted(os.listdir(a.assets)):
            if name.startswith("launcher_") and name.endswith(".png"):
                shutil.copy2(os.path.join(a.assets, name), os.path.join(dst, name))
    game = os.path.join(a.folder, "game")
    have = os.path.isfile(os.path.join(game, "default.xbe"))
    print(f"{a.folder}: darkwatch.exe, README.txt" + (", assets" if a.assets else ""))
    if not have:
        print(f"Next: copy your disc's files into {game} (or pick their folder in Settings > General).")


if __name__ == "__main__":
    main()
