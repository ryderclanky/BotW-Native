# BotW-Native

Breath of the Wild (Wii U) boots and plays on **Windows 10 and Windows 11** with Vulkan. The
PowerPC game code is statically recompiled to C++ and runs on a Cafe OS runtime. Cemu's Latte
GPU layer draws through Vulkan. There is no emulator and no JIT.

Verified on an RTX 5060 laptop with 16 GB of RAM, from a USA v208 (1.5.0) dump: the title
screen, the intro, the wake-up scene, and gameplay in the Shrine of Resurrection, at 60 fps
(about 2000–3700 draws per frame in the shrine, 22–34 AX voices). That session ran 180 seconds
with no assert and no crash. The title screen in that run was the pulsing "(A) Start" prompt,
the version line, and the corner icons. The Zelda logo, the TitleBG background, and the title
music were not playing before A was pressed. See [Known issues](#known-issues).

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
./build/BotWRecomp.exe --game /path/to/game --seconds 180
```

`BOTW_GAME` is the same path. `--seconds N` ends the run after N seconds. The log line is
`time limit reached`. Saves, logs, and the shader cache go to `%APPDATA%\BotWRecomp`.
`BOTW_USER_ROOT` overrides that folder.

Startup commits the Cafe windows the game uses (code, heaps, MEM2, MEM1, foreground): about
1.4 GB of commit charge, not resident RAM. A pagefile that is too small can fail a later
commit. The log line is `guest commit charge 1451 MB (not resident)`.

The window title is **BotW-Native (Windows)**. The first log line is
`BotW-Native for Windows (Vulkan)`.

Useful flags: `--fps 30` for the game's original pace (60 is the default), `--scale 1` for
1280×720, `--display fit|stretch|fill`, `--no-gyro`, `--mute`. `--renderer vulkan` is the
only choice on Windows. Leave `--trace` and `--trace-hle` off for a normal run.

## Controls

On this build, **E = A**, **Left Shift = B**, and **Enter = Plus**. A full keyboard and
gamepad map for every GamePad button and both sticks is coming in the next update.

F1 opens the settings menu. F11 toggles fullscreen.

## Environment variables

| Variable | What it does |
| --- | --- |
| `BOTW_GAME` | Folder that contains `code/`, `content/`, and `meta/`. Same as `--game`. |
| `BOTW_USER_ROOT` | Saves, `settings.ini`, `runtime.log`, and the shader cache. Default is `%APPDATA%\BotWRecomp`. |
| `BOTW_DIAG` | Set to `1` for three extra log lines every 10 seconds: draws per color target and draws the command processor dropped, AX voices in the mixer and the peak sample sent to the speakers, and how often `VPADRead` runs. The GamePad sampling callback runs at 200 Hz on its own thread. |
| `BOTW_RENDERER` | Renderer name. On Windows the build only includes Vulkan. |
| `BOTW_WATCH` | Debug watch on a guest address. Leave it unset for play. |

## Known issues

- **Title logo, TitleBG, and title music.** Before A is pressed, the verified run drew the
  prompt and stayed silent (5 draws per frame). Gameplay after A is what the 60 fps shrine
  result covers. That fix is not in this branch.
- **Title-to-intro hitch.** One slow frame of about 2 seconds, a CPU texture untile, then the
  game recovered. It did not crash.
- **First-run shaders.** Pipelines compile on first use, so the first time through a scene
  hitches. Later runs use the cache under `%APPDATA%\BotWRecomp`.
- **A few cutscenes stay at 30 fps** on purpose. The shrine gameplay in the verified run was
  60 fps.
- **Beyond the shrine.** The verified session is the opening through the Shrine of
  Resurrection. A long open-world session is not part of that result. DLC is untested.
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
