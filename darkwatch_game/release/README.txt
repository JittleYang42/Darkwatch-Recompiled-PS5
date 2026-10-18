DARKWATCH - RECOMPILED FOR PC
=============================

Darkwatch (Xbox, 2005) running natively on Windows: the game's own code,
translated into C and compiled for the PC, with a Vulkan renderer and the
console's sound chip in software.

You need your own copy of the game: nothing from it is included here.


GAME FILES
----------
Copy the files from your Darkwatch (USA) disc -- default.xbe, darkwatch.uix
and the .pck files -- into a folder named "game" next to darkwatch.exe. A
"game" folder one to three levels up is found too. Anywhere else: Settings >
General > Game folder.


STARTING
--------
Double-click darkwatch.exe. The launcher opens: Play, Settings, Controls, Exit.
Mouse, keyboard (arrows, Enter, Esc) or an Xbox controller (D-pad, A, B, LB/RB
for pages, Start to play) all work in it.

To start the game directly from a shortcut: darkwatch.exe --play


IN GAME
-------
  F1                 settings while you play: frame rate, resolution, mouse
                     sensitivity, key bindings, volume. F1 or Esc closes them
                     (the game keeps running underneath, so pause it first)
  F11 or Alt+Enter   fullscreen / window
  F12                screenshot for bug reports: screenshots\shot-<time>.png plus
                     a .txt list of what was drawn in that frame
  F9                 free the mouse (and take it back)
  Alt+F4             quit (the game saves at its checkpoints as on the console)

Keyboard and mouse (change any of it in Controls):
  W A S D            move              Mouse         look / aim
  Left mouse         shoot             Right mouse   sight mode / zoom
  Space              jump (twice: vampire jump)
  E / Enter          interact, menu confirm
  R / Backspace      reload, menu back
  F / V              melee             G / Mouse 4   throw
  Q                  activate blood power
  T                  cycle blood powers
  Left Ctrl / C      crouch
  Wheel down / 1     cycle guns        Wheel up / 2  cycle thrown weapons
  Z / X              lean left / right
  Esc                pause menu        Tab           score menu
  Arrow keys         menus (D-pad)

Controller: an Xbox controller works as on the console (LB = Black, RB = White).
PlayStation controllers work through Steam Input or DS4Windows. Buttons can be
remapped in Controls > Controller.


SETTINGS (darkwatch.ini, written by the launcher)
--------
Graphics
  Display mode         borderless fullscreen or a window
  Internal resolution  the 3D drawn at 1x-4x the console's 640x480;
                       Auto picks 3x for 1080p/1440p, 4x for 4K
  Aspect ratio         4:3 as the game was made, or stretched
  Frame interpolation  in-between frames up to your display's refresh rate;
                       the game logic keeps its original timing
  Frame rate limit     30 ... 360 fps or Unlimited
  V-Sync               on: no tearing; off: frames shown as soon as ready
  Graphics card        which GPU draws the game
Sound      volume, mute in the background
General    folders, skip the launcher (hold Shift while starting to get it
           back), log console, advanced options


FILES
-----
  darkwatch.exe        the game
  darkwatch.ini        settings (made on first start)
  darkwatch-log.txt    the last run's log (the one before: -prev.txt)
  saves\               save games (UDATA) and the console's cache partitions
  screenshots\         F12 captures
  assets\              optional launcher pictures (launcher_bg.png,
                       launcher_bg_blur.png, launcher_emblem.png,
                       launcher_wordmark.png); without them the launcher
                       draws a plain background
  env.txt              optional: KEY=VALUE lines for the runtime's switches
                       (developer use; they override the settings)

If something goes wrong, darkwatch-log.txt says what happened.


REQUIREMENTS
------------
Windows 10/11 64-bit, a CPU with AVX2 (Intel Haswell / AMD Zen or newer), a
Vulkan 1.2 graphics card.
