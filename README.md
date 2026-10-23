# BotW Recompiled for Windows

Breath of the Wild (Wii U) boots and plays on **Windows 10 and Windows 11** with Vulkan. The
PowerPC game code is statically recompiled to C++ and runs on a Cafe OS runtime. Cemu's Latte
GPU layer draws through Vulkan. There is no emulator and no JIT.

Verified on an RTX 5060 laptop with 16 GB of RAM, from a USA v208 (1.5.0) dump: the title
screen, the intro, the wake-up scene, and gameplay in the Shrine of Resurrection, at 60 fps
(about 2000–3700 draws per frame in the shrine, 22–34 AX voices). That session ran 180 seconds
with no assert and no crash. The title screen in that run was the pulsing "(A) Start" prompt,
the version line, and the corner icons. The Zelda logo, the TitleBG background, and the title
music were not playing before A was pressed. A fix for that is in the tree and is waiting on
another run on the same PC. See [Known issues](#known-issues).

> [!IMPORTANT]
> This repository contains no game files, no keys, and no game code. You supply your own
> legally dumped copy. Nothing here is a download for the game.

## Requirements

- Windows 10 or Windows 11, and a GPU with a Vulkan 1.1 driver. The verified machine is an
  RTX 5060 laptop.
- [MSYS2](https://www.msys2.org/) with the **UCRT64** environment. The Cafe runtime uses
  pthreads, so the build is MinGW gcc. MSVC and clang-cl are not supported.
- Your own legally dumped **Breath of the Wild for Wii U, USA, with update v208 (1.5.0)**.
  The recompiler and the game patches match that executable. The dump is one folder with
  three directories in it:

  ```text
  game/
    code/     U-King.rpx and the rest of the code
    content/  packs and the rest of the content
    meta/     meta.xml
  ```

  Check the executable:

  ```text
  SHA256 of code/U-King.rpx
  ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30
  ```

## Build on Windows

Open an **UCRT64** shell from MSYS2 (not the MSYS or MINGW64 shell).

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-vulkan-headers \
  mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-libpng \
  mingw-w64-ucrt-x86_64-zstd \
  mingw-w64-ucrt-x86_64-glm \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-fmt \
  mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-glslang
```

If SDL3 or glslang is missing, CMake fetches SDL 3.4 and glslang 15.1. `fmt` from the distro
is used as a header-only library.

Recompile your copy of `U-King.rpx` to C++. This writes `generated/` and does not download
anything. `generated/` is not in this repository.

```bash
python tools/recompile.py /path/to/game/code/U-King.rpx generated
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j6
```

Use **`-j6`** on a 16 GB machine. Compiling `generated/` is the step that uses RAM. A higher
job count can run the machine out of memory. The same `CMakeLists.txt` is what
`.github/workflows/windows.yml` configures. Vulkan is the only graphics backend on Windows.

## Run

```bash
./build/BotWRecomp.exe --game /path/to/game
```

`BOTW_GAME` is the same path. With a game loaded, the process runs until you close the window.
`--seconds N` (N > 0) is a time limit for tests. `--seconds 0` and a negative value leave the
run unlimited. Saves, logs, and the shader cache go to `%APPDATA%\BotWRecomp`.
`BOTW_USER_ROOT` overrides that folder.

Startup commits the Cafe windows the game uses (code, heaps, MEM2, MEM1, foreground): about
1.4 GB of commit charge, not resident RAM. A pagefile that is too small can fail a later
commit. The log line is `guest commit charge 1451 MB (not resident)`.

The window title is **BotW Recompiled (Windows)**. The first log line is
`BotW Recompiled for Windows (Vulkan)`. The same control map is printed at startup.

Useful flags: `--fps 30` for the game's original pace (60 is the default), `--scale 1` for
1280×720, `--display fit|stretch|fill`, `--no-gyro`, `--mute`. `--renderer vulkan` is the
only choice on Windows. `--fullscreen` starts in fullscreen (F11 still toggles it). Leave
`--trace` and `--trace-hle` off for a normal run.

## Launcher

The same build also produces `build/BotWLauncher.exe`, a separate program from the game.
From the UCRT64 shell:

```bash
./build/BotWLauncher.exe
```

Double-click `build/BotWLauncher.cmd` if you are not in that shell. It puts
`C:\msys64\ucrt64\bin` on `PATH` so the SDL and MinGW DLLs load. Paths are remembered in
`BotWLauncher.ini` next to the exe.

Set the game folder (`code/`, `content/`, `meta/`), `BotWRecomp.exe`, and the user-data
folder. Play launches the game with `BOTW_USER_ROOT` set to that folder, `--game`, and
`--renderer vulkan`. Seconds `0` means no time limit; a positive number passes `--seconds`.
The fullscreen box passes `--fullscreen`.

**Controls.** Click the Keyboard or Controller cell, then press a key or a button (Backspace
clears, Esc cancels). Reset writes the built-in map. The file is `<user data>\controls.ini`.
The game reads it once at startup. With no file, the built-in map is unchanged (WASD, IJKL,
E / Shift / Space / F, and the auto controller layout in the table below). Face buttons in a
new file are Auto, so they follow that layout. Choosing a position (East, South, North, West)
overrides Auto for that button only. Keyboard remaps, including a quick tap, use the file too.

**Saves.** The game reads `<user data>\save\80000001\` (account slot 1, persistent id
`80000001`) and `<user data>\save\common\`. Slot folders are `0`, `1`, … plus `option.sav`.
Import save takes a folder or a `.zip` (or drop one on the window): a Cemu dump
(`mlc01/usr/save/00050000/101c9400/user/80000001/`, and the JP `101c9300` / EU `101c9500`
ids), the account folder itself, or a bare folder of slot directories and `option.sav`.
The current save is copied to `<user data>\save-sets\<timestamp>-backup` first, and the
import is kept there too. Switch to and Delete only touch those sets. Open saves folder
opens `save\`. Import, switch, and delete do nothing while `BotWRecomp.exe` is running.

Switch saves (little-endian) and saves newer than 1.5.0 (save version above `0x471B`,
including 1.6.0 / `0x471E`) are called out and are not installed unless you confirm.
**Migrate old save** copies `save\2147483649` (the folder an older build used for slot 1)
onto `save\80000001`. It backs up a save that is already in `80000001` and does not delete
or move the decimal folder. View log shows the tail of `<user data>\runtime.log`.

## Controls

Keyboard and controller are read together. Stick axes are -1..1 with +Y up. The controller
stick uses a radial deadzone of 0.15, rescaled so the edge of the deadzone is still zero.
ZL and ZR follow the analog triggers once they pass about a quarter of the pull.

The default face mapping is **Auto**. It follows the labels SDL reports for that pad, so the
Wii U A button is the button labeled A or Cross:

| GamePad | Keyboard | Auto, Xbox / PlayStation / standard | Auto, Switch Pro, Joy-Con, 8BitDo in Switch mode | Auto, GameCube |
| --- | --- | --- | --- | --- |
| A | E | South (A / Cross) | East (A) | South (A) |
| B | Shift | East (B / Circle) | South (B) | West (B) |
| X | Space | West (X / Square) | North (X) | East (X) |
| Y | F | North (Y / Triangle) | West (Y) | North (Y) |
| L | 1 | Left shoulder | Left shoulder | Left shoulder |
| R | 3 | Right shoulder | Right shoulder | Right shoulder |
| ZL | Q | Left trigger, past about a quarter pull | Left trigger | Left trigger |
| ZR | R | Right trigger, past about a quarter pull | Right trigger | Right trigger |
| Plus | Enter | Start | Plus | Start |
| Minus | Tab | Back | Minus | (none on a real cube pad) |
| D-pad | Arrow keys | D-pad | D-pad | D-pad |
| Left stick | W A S D | Left stick | Left stick | Stick |
| Right stick | I J K L (J left, L right, I up, K down) | Right stick | Right stick | C-stick |
| Left stick click | C | Left stick click | Left stick click | (none) |
| Right stick click | V | Right stick click | Right stick click | (none) |
| Home | H, or the Home key | Guide | Home | (none) |
| Sync | G | Extra button, when the pad has one | Sync, when the pad has one | (none) |
| TV | T | Share / Capture | Capture | (none) |

The in-game menu can force one layout for every pad. **Auto (per controller)** is
`layout=-1`. **A = right (Circle / Xbox B)** is `layout=0` (Nintendo positions on every
pad). **A = bottom (Cross / Xbox A)** is `layout=1`. A `settings.ini` that already has
`layout=0` keeps Nintendo positions.

Pads already connected at launch are opened after the mappings load. A pad plugged in
later is opened from the connect event, and unplugging closes it. When several pads are
connected, the one used most recently drives the game. Plugging in a new pad does not
steal control from a pad that has already been used. Keyboard input keeps working at the
same time. Gyro is enabled on the active pad when that pad has a gyro and an accelerometer
(DualShock 4, DualSense, Switch Pro, and Joy-Con pairs that expose them). Rumble goes to
the active pad only.

Connect and disconnect lines name the controller and the type, for example
`controller connected: DualSense Wireless Controller (DualSense, gyro, A = bottom)`.
Switching pads logs `controller active: ...`.

Mappings come from SDL's built-in list, then the bundled `gamecontrollerdb.txt` copied
next to the executable (the community [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB),
zlib license, Copyright (C) 1997-2025 Sam Lantinga, retrieved 2026-10-22 from commit
`ff333823a85a`). A `gamecontrollerdb.txt` in the user folder (`%APPDATA%\BotWRecomp`, or
`BOTW_USER_ROOT`) is loaded after that. `SDL_GAMECONTROLLERCONFIG_FILE` points at another
file. `SDL_GAMECONTROLLERCONFIG` is applied by SDL at startup and overrides the files.

On Windows, PlayStation, Switch, Joy-Con, and 8BitDo pads are opened through SDL's HIDAPI
drivers. Xbox pads stay on RawInput correlated with XInput so each pad is opened once.
DirectInput stays on for other pads. Those hints are set before SDL starts. This environment
cannot see a physical controller; the table above is what the code maps, and it still needs
a pass on real hardware.

F1, both stick clicks, or the touchpad opens the settings menu. Escape closes it. While the
menu is open the game sees no buttons. F11 toggles fullscreen. Close the window to quit.

## Environment variables

| Variable | What it does |
| --- | --- |
| `BOTW_GAME` | Folder that contains `code/`, `content/`, and `meta/`. Same as `--game`. |
| `BOTW_USER_ROOT` | Saves, `settings.ini`, `runtime.log`, the shader cache, and an optional `gamecontrollerdb.txt`. Default is `%APPDATA%\BotWRecomp`. |
| `SDL_GAMECONTROLLERCONFIG` | One extra SDL controller mapping string. Applied by SDL at startup, ahead of the bundled database. |
| `SDL_GAMECONTROLLERCONFIG_FILE` | Path to an extra `gamecontrollerdb.txt`, loaded after the user folder's file. |
| `BOTW_DIAG` | Set to `1` for four extra log lines every 10 seconds: draws per color target, AX voices, `VPADRead`, and a cutscene/UI line (`prim`, `depth`, `small`, `afterScan`, `scissor`, `copy`, `pipe`, `skip`, `fontClear`, `blendBoth`, `depthOff`). `[cutscene]` lines for a skipped draw or an unsupported format are printed once per unique case even when this is unset. |
| `BOTW_RENDERER` | Renderer name. On Windows the build only includes Vulkan. |
| `BOTW_WATCH` | Debug watch on a guest address. Leave it unset for play. |

## Known issues

- **Title logo, TitleBG, and title music.** Before A is pressed, the verified run drew the
  prompt and stayed silent (5 draws per frame). Gameplay after A is what the 60 fps shrine
  result covers. The frame-span cap that should let the logo, background, and music start is
  pending verification on the RTX 5060. Title music missing means that screen is at least
  partly a timeline skip, not only a GPU pass.
- **Cutscene text.** Free-camera gameplay text renders. Text during locked-camera / cinematic
  moments does not. Dialogue boxes in free play do appear. Two Vulkan cases that drop a draw
  are fixed in this branch (blend factors `0x0B`/`0x0C`, and a depth test left enabled after
  the depth buffer was detached). Whether a cutscene hits either one is not proven offline.
  Confirm by playing a cutscene: subtitles should be visible. If they are not, set
  `BOTW_DIAG=1`, play ten seconds of a cutscene and ten seconds of free-camera gameplay, and
  send both `[title] prim ...` lines plus every `[cutscene]` line and any
  `Failed to create graphics pipeline` line from `%APPDATA%\BotWRecomp\runtime.log`.
  `depthOff` and `blendBoth` count pipeline compiles in that window, not draws. A non-zero
  `scissor`, `copy`, `pipe`, `skip`, `fontClear`, or `afterScan` during the cutscene window
  and not during free play is the comparison that matters. Also look for
  `frame span ... clamped to 50 ms` on the title screen.
- **Title-to-intro hitch.** One slow frame of about 2 seconds, a CPU texture untile, then the
  game recovered. It did not crash.
- **First-run shaders.** Pipelines compile on first use, so the first time through a scene
  hitches. Later runs use the cache under `%APPDATA%\BotWRecomp`.
- **A few cutscenes stay at 30 fps** on purpose. The shrine gameplay in the verified run was
  60 fps.
- **Beyond the shrine.** The verified session is the opening through the Shrine of
  Resurrection. A long open-world session is not part of that result. DLC is untested.
- **Dialogue and controllers.** Talking to the Old Man on the Great Plateau stopped accepting
  keyboard input once the text box appeared (E for A, Left Shift for B, from the build that
  was played). The controller path was not tried. The fix is in this branch and has not been
  played on the RTX 5060. Hotplug, family face buttons, gyro, and advancing that conversation
  from a real pad are untested here: this environment has no controller.
- **Linux.** The Vulkan backend configures and compiles. Without game files the process
  brings up a device and swapchain, presents one empty frame, and exits. A full Linux
  play-through is not what was verified.
- **macOS and iOS.** Metal is the original backend, from upstream BotW Recompiled, and it
  stays the default on Apple. Upstream's Mac and iPhone results live in
  [`ios/PORTING.md`](ios/PORTING.md). This fork's verified run is the Windows Vulkan path.
  The experimental native Metal renderer is off by default
  ([`docs/native-renderer.md`](docs/native-renderer.md)).

More notes are indexed in [`docs/README.md`](docs/README.md).

## Credits and origins

This project started from [BotW Recompiled](https://github.com/IshaanKalra2103/BotWRecomp) by
[Ishaan Kalra](https://github.com/IshaanKalra2103), the static recompilation of Breath of the
Wild for iPhone and Mac. The recompiler, the Cafe OS runtime, and the original Metal backend
come from that project. This repository adds the Windows build and the Vulkan backend on top
of it. Thank you to Ishaan Kalra and to everyone whose work it builds on:

- [Cemu](https://github.com/cemu-project/Cemu): the GPU layer in `runtime/cemu/`, and parts of
  GX2, snd_core, and the motion code in `runtime/src/` (each such file says so at the top).
  Vendored from commit `a53cf0ece75964aa361056edab46f281dcc0bc44`.
- Cemu's graphic packs (CC0): the resolution rules in `runtime/cemu_shims/botw_texture_rules.inc`.
  The 60 fps approach follows the FPS++ pack by Xalphenos, rajkosto, epigramx, Crementif,
  leoetlino, Pig, and Exzap.
- [SDL3](https://libsdl.org), [fmt](https://fmt.dev), [zstd](https://facebook.github.io/zstd/),
  [glslang](https://github.com/KhronosGroup/glslang),
  [Dear ImGui](https://github.com/ocornut/imgui), and
  [metal-cpp](https://developer.apple.com/metal/cpp/).

This project's own code is MIT licensed ([LICENSE](LICENSE)). Code from Cemu stays under the
Mozilla Public License 2.0 ([runtime/cemu/LICENSE.txt](runtime/cemu/LICENSE.txt)). Dear ImGui
and metal-cpp keep their own licenses, in `runtime/cemu/dependencies/`.

The Legend of Zelda and Breath of the Wild are trademarks of Nintendo. This is an unofficial
fan project, not affiliated with or endorsed by Nintendo.
