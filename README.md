# Double Dragon II Recompiled

A native PC port of **Double Dragon II: The Revenge** (NES, USA Rev 1), built
with [NESRecomp](https://github.com/mstan/nesrecomp). The game's own 6502 code
is recompiled to C and runs natively, cycle-accurate, at the speed of the
real console, with additions the NES could never do:

- **Smooth scrolling.** The camera glides instead of stepping 2 pixels on the
  game's uneven 2,2,3,1 rhythm, horizontally and on ladders.
- **Character glide.** Billy, Jimmy and every enemy glide between the game's
  30 Hz positions instead of hopping. Never ahead of the game, at most 1/30 s
  behind; punches and poses happen exactly when the game shows them.
- **120 / 180 / 240 pictures a second.** In-between pictures move the camera
  and characters a fraction of a pixel each, for high-refresh and FreeSync /
  G-Sync monitors. A steady 240 on a 240 Hz monitor: the next NES frame is
  emulated on a second CPU core while the in-between pictures are shown.
- **No sprite flicker.** Every sprite is drawn every frame, in one fixed
  order (the NES can only draw 8 per line).
- **Steady HUD line** and an **edge crop** that hides the NES's scrolling
  garbage at the screen edges.
- An in-game **settings menu**, save states and rewind, two players with two
  controllers.

Everything above changes only the picture; the game itself runs exactly as
on a NES. Each feature can be switched off in the menu.

## What's new in 1.1

- **A real 240 pictures a second.** Version 1.0 managed about 180 at best;
  at 240 it showed only ~116 unevenly spaced pictures a second. Now each NES
  frame is emulated on a second core (new menu row **Second core**, on by
  default) while the in-between pictures are drawn, measured at 239.7 a
  second with one skipped picture in 100 seconds of play.
- **Fits under a 240 Hz monitor.** Four pictures per NES frame would be 240.4
  a second, just over the monitor's limit. At 240 the game now runs at 59.94
  frames a second instead of 60.10 (0.26% slower, not noticeable) and the
  sound is stretched to match, so it never crackles.
- **Newer engine** (thread pinning, input read before the overlapped frame,
  no leftover in-between pictures). Rewind now starts off: its snapshots made
  the next frame late (turn it on in the menu if you want it).

**You need your own ROM.** This repository contains no game data. You must
supply a dump of *Double Dragon II - The Revenge (USA) (Rev 1)*; the program
checks it is the right one before it runs.

## Download

Get the latest version from the
[Releases page](https://github.com/gabbycat4/DD2Recomp/releases/latest):

- **Windows:** `DD2Recomp-Windows-x64.zip`. Extract it, double-click
  `dd2recomp.exe`.
- **Linux:** `DD2Recomp-x86_64.AppImage`. Mark it executable (right-click,
  Properties, Permissions, or `chmod +x`), then double-click it.

The first time, the game asks for your ROM and checks it. No building needed.

**High-refresh monitor?** A fresh download starts at 60 pictures a second
with VSync **On** (right for ordinary 60 Hz monitors). On a 240 Hz
FreeSync/G-Sync monitor, open the menu (**Escape**) and set **Pictures per
second = 240** and **VSync = Off**; otherwise it will stutter.

## Recommended settings

The game starts with these already on, except two you set yourself:
**Pictures per second** starts at 60, because only high-refresh monitors
benefit from more, and the game opens in a window (press **F11** for
fullscreen). In game, press **Escape** (or the right shoulder button) for the
menu:

| Menu | Setting | Recommended |
|---|---|---|
| Smooth scrolling | Camera smoothing | **Scenery only** |
| Smooth scrolling | Character glide | **On** |
| Smooth scrolling | Pictures per second | **240** on a 240 Hz FreeSync/G-Sync monitor (180 if you prefer it: walking is perfectly even at 240, running at 180); your monitor's rate or lower otherwise; **60 (off)** on a 60 Hz monitor |
| Smooth scrolling | Second core | **On** (needs a CPU with 2+ cores; only used above 60) |
| Picture | No sprite flicker | **On** (always on while Character glide is on) |
| Picture | Steady HUD line | **On** |
| Picture | Edge crop | **Standard** |
| Graphics | VSync | **Off** with FreeSync/G-Sync; **On** without |
| Graphics | Integer scaling / Linear filter | **On / Off** |
| Display | Fullscreen | On (**F11** toggles) |

Settings are saved in `config.ini` beside the program.

## Troubleshooting

- **Stutters at 240:** VSync must be **Off** with FreeSync/G-Sync, and your
  desktop's adaptive sync on (KDE: Display settings, Adaptive sync =
  Always). Check that Second core is On.
- **Buttons feel late:** try Second core Off and a lower picture rate, and
  tell us in an issue.

## Building

Only needed if you want to change the code. You need Git, CMake 3.20+, a C/C++ compiler, Python 3.11+ and SDL2.

**Linux** (Arch/CachyOS: `sudo pacman -S git cmake ninja sdl2 python`;
Debian/Ubuntu: `sudo apt install git cmake ninja-build build-essential libsdl2-dev python3`):

```sh
git clone --recursive https://github.com/gabbycat4/DD2Recomp.git
cd DD2Recomp
cp /path/to/your/rom.nes dd2.nes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/dd2recomp dd2.nes
```

`--recursive` also downloads the engine and the menu (they are submodules).
If you cloned without it, run `git submodule update --init --recursive`.

The first configure takes a while: it builds the recompiler and translates
the game.

**Optional, for 100% recompiled code:** a little of the game's code is copied
into RAM at run time and runs on the built-in interpreter unless it is
captured. `tools/capture.sh` records it (see the comments in the script);
the game plays correctly either way.

## Controls

Keyboard and controllers are rebindable in the launcher's Controls page. A
second controller plays as player 2 (pick a 2-player mode on the title
screen).

| | |
|---|---|
| Escape / right shoulder | Menu |
| F7 | Save states |
| F8 | Rewind |
| F11 | Fullscreen |
| Tab | Fast forward |
| F12 | Screenshot |

## Command line

Every menu setting also has an option for one run (it does not change
`config.ini`): `--smooth 0|1|2`, `--char-smooth 0|1`, `--present-rate N`,
`--emu-thread 0|1`, `--no-sprite-limit 0|1`, `--hud-split-fix 0|1`,
`--overscan T,B,L,R`. Developer logs: `--scroll-log FILE`, `--oam-log FILE`,
and `CYC_PACING_LOG=file` (environment) for when every picture was shown.
`CYC_PACE_5994=0` keeps the console's own 60.10 frames a second at 240.

## How it works

The NESRecomp engine runs the recompiled game cycle by cycle. This project
adds a presentation layer (`src/dd2_extras.c`, `src/dd2_smooth.c`) through
the engine's `HOST_EXTRAS` interface:

- the camera's real per-line scroll is read from the PPU, its speed is
  tracked over whole cycles of the game's rhythm, and the playfield is
  redrawn from the background alone at the smoothed position;
- DD2 writes its sprite list forwards and backwards on alternate frames (that
  is how it flickers); the list is read in one fixed order, split into
  characters (each is written top row to bottom) and matched frame to frame;
- with a picture rate above 60, the window asks for extra pictures between
  machine frames and they are drawn at 4x resolution, so a step can be a
  quarter of a NES pixel;
- everything those pictures need (the NES picture, background, sprites,
  pattern data, colours) is copied once per frame while the machine is idle,
  so the next frame can run on a second core at the same time.

`notes.txt` has the timing measurements that started it.

## Credits and licences

- [NESRecomp](https://github.com/mstan/nesrecomp) and
  [recomp-ui](https://github.com/mstan/recomp-ui) by Matthew Stanley; their
  own licences apply (see the `nesrecomp` and `recomp-ui` folders). The engine
  submodule points at a fork with window additions (in-between pictures,
  the emulation thread and its core pinning, 59.94 pacing at 240,
  display-sized windows, a VSync menu row).
- This project's code is licensed under the PolyForm Noncommercial License
  1.0.0 (`LICENSE`): free to use, share and change; not to be sold.
- *Double Dragon II: The Revenge* is © Technōs Japan / Arc System Works. This
  project is not affiliated with them and includes none of the game.
