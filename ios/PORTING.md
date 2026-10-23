# BotW-Native on iOS — porting notes

These are the original iPhone and Mac notes from upstream BotW Recompiled. Windows Vulkan is
documented in the [repository README](../README.md). Metal remains the Apple backend.

Target: iPhone 16 Pro (A18 Pro, 8 GB), iOS 18+ deployment target, built with Xcode 27 / iOS 27 SDK. Native build: the recompiled
PowerPC code is plain C++ and needs no JIT, which is what makes an App-Store-style iOS binary possible.

Everything iOS-specific lives in `ios/` (CMake project, plist, entitlements, scripts, sources, compat
headers), build output in `build-ios/`, third-party sources/builds in `ios-deps/`.
`ios/src` is deliberately outside `runtime/`: the macOS CMakeLists globs `runtime/src/**` and
`runtime/cemu_shims/**` recursively, so any new `.cpp/.mm` there would be compiled into the Mac build.

## 1. Survey: macOS-only dependencies

| Area | Where | macOS assumption | iOS handling |
|---|---|---|---|
| Window | `runtime/src/host/window.mm` | `#import <AppKit/AppKit.h>`, `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER` -> `NSWindow.contentView` handed to the renderer | guarded hunk: UIKit import, `SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER` -> `rootViewController.view` |
| Metal layer | `runtime/cemu/.../Metal/MetalLayer.mm`, `Renderer/MetalView.{h,mm}` | NSView subclass, `convertRectToBacking`, `wantsLayer`, `CAMetalLayer.displaySyncEnabled` (macOS-only) | both excluded from the iOS build; `ios/src/MetalLayer_ios.mm` adds a CAMetalLayer-backed UIView child (native scale, touches pass through) |
| GPU glue | `runtime/cemu_shims/latte_glue.mm` | imports AppKit (only CoreGraphics/ImageIO actually used, for `--capture` PNGs) | guarded hunk: Foundation + CoreGraphics on iOS |
| Process spawn | `Metal/MetalCommon.h` `executeCommand()` -> `system()` | `system()` is unavailable in the iOS SDK (used only by the xcrun/AIR shader-cache path) | guarded: returns failure on iOS |
| Pixel formats | `Metal/MetalCommon.h` | `depth24Stencil8PixelFormatSupported` is a macOS-only MTLDevice property | guarded: `false` on iOS -> D24S8 maps to Depth32Float_Stencil8 (same as Apple-silicon Macs today) |
| Storage modes | `MetalRenderer.h:394`, `MetalRenderer.cpp:618`, `MetalBufferAllocator` | `MTLStorageModeManaged` + `didModifyRange` | only chosen when `!hasUnifiedMemory` / non-Apple GPU; never on iOS, no change needed |
| BC textures | `LatteToMtl.cpp` (BC1-5 -> `PixelFormatBC*`) | no fallback if BC is unsupported | A14+ GPUs on iOS 16.4+ support BC (`supportsBCTextureCompression`); logged at start (`ios: GPU ... BC textures N`). If a device reports 0 a CPU-decode fallback is needed |
| Device list | `MetalRenderer::GetDevices` (`MTL::CopyAllDevices`) | macOS-only API | metal-cpp already returns nullptr off macOS; only used when a GPU UUID is configured |
| Platform macros | Cemu `Common/platform.h`, `precompiled.h` (bswap, thread names, timers) | `BOOST_OS_MACOS`, which boost.predef sets to 0 on iOS | `ios/compat/botw_ios_platform.h` (force-included first) maps `BOOST_OS_MACOS` to `BOOST_OS_IOS` |
| OpenSSL | `FetchShader.cpp` (SHA-1 via EVP), `MetalPipelineCache.cpp` (SHA256) | Homebrew libcrypto.dylib | `ios/compat/openssl/{sha,evp}.h` over CommonCrypto (identical digests, no libcrypto) |
| libpng | `Renderer.cpp` includes `<png.h>` | Homebrew header, nothing called | empty `ios/compat/png.h` |
| boost / glm | `precompiled.h` etc. | `/opt/homebrew/include` (also exposes SDL/fmt/openssl headers) | only the header-only boost and glm kegs (`/opt/homebrew/opt/{boost,glm}/include`) |
| SDL3, fmt, zstd | link line | Homebrew dylibs | static iOS builds from pinned sources (`ios/scripts/build_deps.sh`: SDL3 3.4.18, fmt 12.2.0, zstd 1.5.7, SHA-256 checked) |
| zlib | loader, zlib125, serializers | system libz | system libz (in the iOS SDK) |
| Frameworks | link line | AppKit | UIKit, GameController, AVFoundation, AudioToolbox (+ SDL's: CoreHaptics, CoreMotion, CoreBluetooth, OpenGLES) |
| Entry point | `runtime/src/main.cpp` `main()` (argv `--game`, `$HOME/Library/Application Support`) | launched from a shell | compiled with `-Dmain=botw_main`; `ios/src/main_ios.mm` owns `main()` (`SDL_RunApp` -> UIScene lifecycle), finds `Documents/game`, creates Application Support, then calls `botw_main` |
| Paths | `main.cpp` user root `$HOME/Library/Application Support/BotWRecomp` | `mkdir` of one level | works in the iOS container once `Application Support` exists (created by main_ios). Game root: `<Documents>/game` (Finder file sharing) or `--game`/`BOTW_GAME` |
| Case sensitivity | `coreinit/fs.cpp` `host_path` | macOS volume is case-insensitive, iOS is case-sensitive | guarded hunk: game-folder paths that do not exist as spelled are resolved case-insensitively (`ios/src/fs_case_ios.cpp`, logs the first 32 matches) |
| Guest memory | `runtime/src/mem.cpp` | `mmap(nullptr, 4 GB + 64 KB, MAP_NORESERVE)` anywhere | unchanged; see below |
| Host stacks | `thread.cpp`, `ax_ist.cpp` | 64 MB pthread stack per guest thread (~40 threads = 2.5 GB VA) | guarded hunks call `botw_ios_stack_size()` (`ios/src/vm_ios.cpp`): probes a 32 GB reservation once; without extended VA stacks are capped at 16 MB |
| Simulator-only Metal limits | `MetalRenderer.cpp`, `MetalCommon.h` | macOS Metal | `TARGET_OS_SIMULATOR`: no attachment-less void-vertex pipeline (it is unused), no packed 16-bit formats; `TARGET_OS_IOS`: `hasUnifiedMemory` forced true (simulator reports false -> Managed buffers, which iOS rejects) |
| No BC support | `LatteToMtl.cpp` | BC always available on Macs | `TARGET_OS_IOS` + `!supportsBCTextureCompression`: BC1-5 decoded on the CPU with Cemu's GL fallback decoders into RGBA32F/RG32F (memory-hungry; the iPhone 16 Pro should not need it) |
| Signals | `main.cpp`, `debug_watch.cpp` | SIGSEGV/SIGBUS handlers, `mprotect` guard page | same on iOS |
| Processes / dlopen / JIT | — | none used | nothing to do (no `MAP_JIT`, no codegen at runtime besides Metal shader compilation, which iOS allows) |

### Guest address space (the important one)

The runtime reserves one contiguous 4 GB + 64 KB anonymous region (address chosen by the kernel).
iOS limits a process's virtual address space unless it has
`com.apple.developer.kernel.extended-virtual-addressing`. From xnu (`osfmk/arm64/sptm/pmap/pmap.c`,
`osfmk/mach/shared_region.h`) on a > 3 GB device:

* shared cache: `0x1_8000_0000 .. 0x3_0000_0000`
* default max VA: `ARM64_MIN_MAX_ADDRESS (0x3_2000_0000) + 0x1_3800_0000 = 0x4_5800_0000`
* with extended VA ("jumbo"): `0xF_C000_0000` (63 GB) or more

So without the entitlement the only hole large enough is `0x3_0000_0000 .. 0x4_5800_0000` (5.4 GB, of
which malloc's nano zone sits at the bottom). A 4 GB reservation fits there only if it is made early
(mem::init is the first thing botw_main does), and it leaves well under 1 GB for 64 MB thread stacks,
Metal's shared buffers and the heap. **Extended VA is the real fix; it needs a paid Apple Developer
Program team** — verified: Xcode refuses with "Personal development teams, including "<your name>",
do not support the Extended Virtual Addressing capability."

`com.apple.developer.kernel.increased-memory-limit` *is* granted to the free Personal Team (Xcode
created `iOS Team Provisioning Profile: <bundle ID>` with it). The app is signed with
it by default; `BOTW_PAID=1` + `ios/scripts/provision.sh --paid` add extended VA on a paid team.

## 2. Signing

* A free Personal Team works (any Apple ID signed into Xcode > Settings > Accounts). The team ID is
  read from the first "Apple Development" certificate in the keychain; `BOTW_TEAM` overrides it.
* Free-team profiles expire after 7 days; `ios/scripts/provision.sh` renews (it drives a tiny Xcode
  project with `-allowProvisioningUpdates`), `ios/scripts/sign.sh` picks the newest valid profile.
* Bundle ID `BOTW_BUNDLE_ID`, default `com.<Mac user name>.BotWRecomp` (registered on the team by
  provision.sh). Bundle IDs are unique across all Apple accounts; set another one if it is taken.
* All settings: `ios/scripts/env.sh`; put overrides in `ios/local.env`.

## 3. Layout

```
ios/CMakeLists.txt        separate CMake project (same generated/ + runtime/ + Cemu sources)
ios/Info.plist.in         landscape, full screen, UIScene manifest (SDL3), UIFileSharingEnabled,
                          LSSupportsOpeningDocumentsInPlace, GameController keys, UILaunchScreen
ios/*.entitlements        free (increased memory limit) / paid (+ extended VA)
ios/src/main_ios.mm       main(): SDL_RunApp, Documents/game discovery, memory stats in runtime.log
ios/src/MetalLayer_ios.mm CAMetalLayer UIView for Cemu's MetalLayerHandle (+ GPU capability log)
ios/src/fs_case_ios.cpp   case-insensitive game-path fallback
ios/src/vm_ios.cpp        extended-VA probe + host stack cap
ios/compat/               openssl (CommonCrypto), png.h stub, BOOST_OS_MACOS mapping
ios/provision/            Xcode project used only to create/renew the provisioning profile
ios/scripts/              build_deps.sh, build.sh, sign.sh, pick_profile.py, provision.sh, sim.sh, device.sh
ios-deps/                 SDL3-3.4.18/, fmt-12.2.0/, zstd-1.5.7/ (sources), build/, prefix-<sdk>/
```

## 4. Build / run

```
ios/scripts/build_deps.sh                    # once: static SDL3/fmt/zstd for device + simulator
ios/scripts/build.sh iphonesimulator         # build-ios/iphonesimulator/BotWRecomp.app (ad-hoc signed)
ios/scripts/sim.sh --seconds 120             # boots "BotW iPhone 16 Pro" sim, uses the Mac's game folder
ios/scripts/provision.sh                     # create/renew the free-team profile (phone must be registered)
ios/scripts/build.sh iphoneos                # build-ios/iphoneos/BotWRecomp.app, signed for the phone
ios/scripts/device.sh install
ios/scripts/device.sh copy-game              # 12 GB / 38.6k files into the app's Documents/game
ios/scripts/device.sh run                    # console output; runtime.log via `device.sh log`
```

Copying the game without the script: Finder > iPhone > Files > BotW, drag the `game` folder (must
contain `code/ content/ meta/`) onto it. The app also accepts any Documents subfolder containing
`code/U-King.rpx` and shows a Retry dialog until one exists. The folder is excluded from backups.

## 5. Results so far (2026-10-16)

* **iPhone 16 Pro (A18 Pro, free Personal Team signing)**: boots to the title screen and into gameplay
  with the on-screen touch controls and the settings menu. Over about 4 minutes the GPU ran at mostly
  35-60 fps (around 3000 draws/frame in the open world), footprint about 2.4 GB with about 3.7 GB left
  before jetsam, BC textures native, and the runtime got the extended address space. After about
  4 minutes the game threads stopped producing frames (watchdog: "no game frame for 8 s") and the run
  had to be killed; the cause is not known yet. One run, one device.

* **Device build**: `build-ios/iphoneos/BotWRecomp.app` (383 MB arm64 executable, platform iOS, minOS
  18.0, SDK 27.0) links and is signed with the Personal Team profile for the 16 Pro
  incl. increased-memory-limit.
* **Simulator (iOS 27.0, "BotW iPhone 16 Pro", headless)**: full boot through UIScene -> SDL -> runtime:
  RPX load, guest threads, GX2/Latte/Metal, audio init, Cemu shader cache; title screen ("(A) Start")
  at 57-60 fps, footprint ~470 MB; scripted A press reaches "Nintendo Presents"/new-game opening, then
  the simulator's Metal rejects BotW's G-buffer pipeline ("requires 48 bytes of pixel storage, this
  device supports 32") -- a simulator GPU limit (A11+ GPUs allow 64), so the simulator stops there.
  Same title-screen behaviour (5 draws/frame, 60 fps) as the macOS build at that point.
  Logs/captures: `build-ios/evidence/simulator-*`.
* **The device binary on Apple-silicon macOS** ("Designed for iPad" wrapper, real Metal on the M1 Pro,
  UIKit/SDL iOS code paths, sandboxed container with the game in Documents/game): title -> new game ->
  "Open your eyes" opening, 370 -> 1258 draws/frame, ~28-30 fps at a 2732x1536 internal resolution,
  footprint ~1.84 GB, BC textures native. This is the iOS binary, but *not* iPhone hardware: no jetsam
  limit, no VA limit, M1 Pro GPU. Evidence: `build-ios/evidence/runtime-ios-binary-on-mac.log`,
  `ios-binary-on-mac-frame*.png`.
* Not yet known: long-session stability (see the hang above), thermals, other iPhones and iPads.

## 6. Known gaps / next steps

* Background: the GPU thread keeps submitting when the app is backgrounded; iOS rejects GPU work from
  the background. Needs a pause on `SDL_EVENT_WILL_ENTER_BACKGROUND`.
* The hang after about 4 minutes of play on the iPhone 16 Pro (section 5).
* The drawable is the full native resolution (2622x1206 on a 16 Pro); a lower drawable size would cut
  the output pass cost.
* Shader compilation happens on first use (no shipped cache); expect hitches on first run.
* `ios/CMakeLists.txt` mirrors the source globs of `../CMakeLists.txt`; when the macOS build adds a
  source directory (as it did for `runtime/cemu/Cemu/FileCache`), add it here too.
* Free-team profiles last 7 days and Xcode regenerates them per device set: provisioning this Mac
  (for the "Designed for iPad" test) produced a profile without the phone. `ios/scripts/sign.sh` keeps
  copies in `build-ios/profiles/` and prefers one that lists a phone; `provision.sh` with the phone
  connected re-registers it.
