# BotW Recompiled for Windows

https://github.com/user-attachments/assets/1b7d6766-425b-4e4b-833a-cb3c60369226

Breath of the Wild (Wii U) running on **Windows 10 and Windows 11** with Vulkan. The
PowerPC game code is statically recompiled to C++ and runs on a Cafe OS runtime. Cemu's Latte
GPU layer draws through Vulkan. There is no emulator and no JIT. This is work in progress and
not a full-game release.

## What works

Tested on a recent NVIDIA laptop GPU with a USA v208 (1.5.0) game folder at about 60 fps.
An extracted Europe v208 dump, including DLC, was also verified through startup and the title
scene; Europe gameplay has not had a full play-through yet.

- the opening
- audio
- the title screen with the logo
- loading a save
- combat
- the map and fast travel
- Hateno Village, with NPCs and shops

Not working or not confirmed yet:
- Some effects, parts of the controller mapping, and some cutscene text are unconfirmed or
  incomplete.
- Everything not listed above has not been checked. See [Known issues](#known-issues).

> [!IMPORTANT]
> This repository contains no game files, no keys, and no decryption tools. You must supply
> your own legally obtained copy of the game, as an already-extracted game folder (`code/`,
> `content/`, `meta/`). Nothing here is a download for the game. This is an unofficial fan
> project and is not affiliated with or endorsed by Nintendo.

## Requirements

- Windows 10 or Windows 11, and a GPU with a Vulkan 1.1 driver. The verified machine has
  a recent NVIDIA laptop GPU.
- [MSYS2](https://www.msys2.org/) with the **UCRT64** environment. The Cafe runtime uses
  pthreads, so the build is MinGW gcc. MSVC and clang-cl are not supported.
- Your own legally obtained **Breath of the Wild for Wii U, USA or Europe, with update v208 (1.5.0)**,
  as an already-extracted game folder. The recompiler and the game patches match that
  executable. Base game, update, and optional DLC must come from the same region. The folder
  has three directories in it:

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
cmake --build build -j12
```

Each compile job on `generated/` peaks at about 0.5 GB of RAM. A 16 GB machine can run
`-j12`. Allow roughly (RAM in GB − 2) / 0.75 jobs, and no more jobs than you have CPU threads.
If `ccache` is on PATH, rebuilds use it automatically. The same `CMakeLists.txt` is what
`.github/workflows/windows.yml` configures. Vulkan is the only graphics backend on Windows.

## Run

```bash
./build/BotWRecomp.exe --game /path/to/game --seconds 180
```

`BOTW_GAME` is the same path. `--seconds N` ends the run after N seconds. The log line is
`time limit reached`. Saves, logs, and the shader cache go to `%APPDATA%\BotWRecomp`.
`BOTW_USER_ROOT` overrides that folder.

Startup commits the Cafe windows the game uses (code, heaps, MEM2, MEM1, foreground): about
1.4 GB of commit charge, not resident RAM. A pagefile that is too small can fail a later
commit. The log line is `guest commit charge 1451 MB (not resident)`.

The window title is **BotW Recompiled (Windows)**. The first log line is
`BotW Recompiled for Windows (Vulkan)`.

Mods: put file replacement mods in `%APPDATA%\BotWRecomp\mods` and enable them in `mods.ini`.
Both launchers support BNP preset choices. `--no-mods` skips mods for one run. See
[`docs/mods.md`](docs/mods.md). Runtime replacement calls and preset selection have automated
coverage; a long modded game session has not been checked.

Useful flags: `--fps 30` for the game's original pace (60 is the default), `--scale 1` for
1280×720, `--display fit|stretch|fill`, `--no-gyro`, `--gamepad-view`, `--mute`. `--renderer vulkan` is the
only choice on Windows. Leave `--trace` and `--trace-hle` off for a normal run.

`--no-dlc` hides add-on content for this run. The launcher also offers **Launch without DLC**.
Use `--language en|fr|de|es|it|nl|ru|ja` to select a language present in the game files, or choose
one in the launcher. The default uses an available language for the detected region.

## Controls

On this build, **E = A**, **Left Shift = B**, and **Enter = Plus**. The runtime log lists the
keyboard and gamepad controls.

F1 opens the settings menu. F11 toggles fullscreen.

F2 shows the GamePad screen beside the TV image, and F2 again hides it. On a controller the
same toggle is the Guide button (or the Share button on pads that have one). It is also a row
in the settings menu, so a pad without those buttons can use L3+R3 and pick it there. It is
off by default, and `--gamepad-view` or `--no-gamepad-view` overrides the saved choice for one run.
While it is on, the window shows the TV (1280×720) and the GamePad (854×480) at the same scale,
centered, and `--display` does not apply. Vulkan renderer only.

## Environment variables

| Variable | What it does |
| --- | --- |
| `BOTW_GAME` | Folder that contains `code/`, `content/`, and `meta/`. Same as `--game`. |
| `BOTW_USER_ROOT` | Saves, `settings.ini`, `runtime.log`, and the shader cache. Default is `%APPDATA%\BotWRecomp`. |
| `BOTW_DIAG` | Set to `1` for three extra log lines every 10 seconds: draws per color target and draws the command processor dropped, AX voices in the mixer and the peak sample sent to the speakers, and how often `VPADRead` runs. The GamePad sampling callback runs at 200 Hz on its own thread. |
| `BOTW_RENDERER` | Renderer name. On Windows the build only includes Vulkan. |
| `BOTW_WATCH` | Debug watch on a guest address. Leave it unset for play. |

## Known issues

- **Title-to-intro hitch.** One slow frame of about 2 seconds, a CPU texture untile, then the
  game recovered. It did not crash.
- **First-run shaders.** Pipelines compile on first use, so the first time through a scene
  hitches. Later runs use the cache under `%APPDATA%\BotWRecomp`.
- **A few cutscenes stay at 30 fps** on purpose. Gameplay runs at about 60 fps.
- **Effects, controller mapping, cutscene text.** Some effects and parts of the controller
  mapping remain unconfirmed. Text and Link rendered in local gameplay checks, but some
  reported cutscene cases have not been reproduced here.
- **Coverage.** Only the areas listed under [What works](#what-works) were checked. A long
  open-world session is not part of that result. DLC reached the Europe title scene, but DLC
  gameplay has not been checked.
- **Linux.** The Vulkan backend has a CI build and headless device check. Without game files the process
  brings up a device and swapchain, presents one empty frame, and exits. A full Linux
  play-through is not what was verified.
- **macOS and iOS.** Metal is the original backend, from upstream BotW Recompiled, and it
  stays the default on Apple. Upstream's Mac and iPhone results live in
  [`ios/PORTING.md`](ios/PORTING.md). This fork's testing is on the Windows Vulkan path.
  The experimental native Metal renderer is off by default
  ([`docs/native-renderer.md`](docs/native-renderer.md)).

More notes are indexed in [`docs/README.md`](docs/README.md).

## Contributing and bug reports

Bug reports and questions go in the [Issues](https://github.com/ryderclanky/BotW-Native/issues)
tab. Please include your GPU, driver version, and `runtime.log` from `%APPDATA%\BotWRecomp`.

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
