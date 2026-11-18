# Zelda-on-PC camera patches → BOTW_HOOK

Source: Zelda-on-PC `src/pcedition/PCGraphics.cpp` (`WriteCameraPatch`, the `[BotWPCE_MouseCamera]` Cemu
patch) and the `HLE_*` handlers in `PCInput.cpp`. Both target U-King.rpx 1.5.0, so the addresses are the same.

Zelda-on-PC overwrites one guest instruction with `b`/`bla` to a code cave, which re-executes the replaced
instruction and calls `import.coreinit.PCE_*`. A `BOTW_HOOK(addr)` runs right **after** the instruction at
`addr`, so every patch becomes a hook at the same address that does what the cave does after the original
instruction. Original instructions below are decoded from our U-King.rpx.

## 1. Entry patches (13): `addr = b _pceXEntry`

Each is the function's first instruction `stwu r1,-N(r1)`; the cave calls the tick with `r3 = this`, then
runs the `stwu`. The hook runs after the `stwu`, `r3` is untouched → direct mapping.

| Addr | Camera | HLE |
|---|---|---|
| 0x02B9B8A0 | main chase camera controller | PCE_CamTick (**phase 1 test hook**) |
| 0x02BEE0F8 | climbing (yaw +0x98, pitch +0x8C) | PCE_ClimbCamTick |
| 0x02BCB3A0 | conversation (yaw +0x48, pitch +0x40) | PCE_TalkCamTick |
| 0x02BD2A44 | horse | PCE_HorseCamTick |
| 0x02BDFB20 | Master Cycle Zero | PCE_CycleCamTick |
| 0x020D72D0 | Magnesis action (object held) | PCE_MagnetTick |
| 0x02BF0234 | lock-on / Magnesis camera (yaw +0xAC, pitch +0xA8) | PCE_LockCamTick |
| 0x02BCED3C | camera rune (pitch +0x28) | PCE_RuneCamTick |
| 0x02BE8B30 | shield surf | PCE_SurfCamTick |
| 0x02BF5ACC | sand seal | PCE_SealCamTick |
| 0x02BF8F88 | Zora waterfall | PCE_FallCamTick |
| 0x02B9250C | zoomed aim (scope, bow drawn) | PCE_ScopeTick |
| 0x02B94E30 | rune aim (Magnesis/Stasis/Cryonis) | PCE_AimTick |

## 2. In-body patches (16): `addr = bla _pceX`

The hook runs after the original instruction and applies the cave's adjustment. Set single-precision results
on both `c->f[n]` and `c->ps1[n]` (as the generated code does). Unlike the caves, hooks need not clobber
`r11`/`f0`/`f2`.

| Addr | Original | Hook does (after it) | HLE |
|---|---|---|---|
| 0x02B9C950 | `fmuls f13,f10,f6` | `f13 += yaw * [r31+0x124]` (**phase 1 test hook**) | PCE_CamYaw |
| 0x02B9C120 | `fmr f23,f1` | normDelta = `guest 0x02B96758(r3=r31, f1=[r31+0xCC]+pitch) - f23` | PCE_CamPitch |
| 0x02B9C188 | `fadds f23,f23,f1` | `f23 += normDelta` (stick path) | — |
| 0x02B9C1AC | `fadds f23,f23,f1` | `f23 += normDelta` (auto path) | — |
| 0x02BCF698 | `fmadds f1,f31,f12,f27` | `f1 = f12 + AimYawAbs(f31*f27, f12)` | PCE_AimYawAbs |
| 0x02B92848 | `fmadds f1,f26,f11,f10` | `f1 += AimPitch()` | PCE_AimPitch |
| 0x02B92A48 | `fmuls f1,f27,f8` | `f1 = AimYawAbs(f1, [r1+0x10])` | PCE_AimYawAbs |
| 0x02B956B0 | `fmuls f1,f26,f9` | `f1 = AimYawAbs(f1, [r1+0x10])` | PCE_AimYawAbs |
| 0x02B95278 | `lfs f1,0x134(r31)` | `f1 += AimPitch()` | PCE_AimPitch |
| 0x02BE8EF8 | `lfs f1,0xA4(r29)` | `f1 = SurfCamPitch(r3=r29, r4=0, f1)` | PCE_SurfCamPitch |
| 0x02BE9698 | `fadds f1,f30,f0` | `f1 = SurfCamYaw(f1)` | PCE_SurfCamYaw |
| 0x02BF5E80 | `lfs f1,0xA0(r30)` | `f1 = SurfCamPitch(r3=r30, r4=1, f1)` | PCE_SurfCamPitch |
| 0x02BF65E8 | `fadds f1,f27,f8` | `f1 = SurfCamYaw(f1)` | PCE_SurfCamYaw |
| 0x02BF914C | `fadds f1,f31,f30` | `f1 = SurfCamYaw(f1)` | PCE_SurfCamYaw |
| 0x02BF02A8 | `or. r27,r3,r3` | r3' = MagnetCamTick(r3, r29); if changed, redo `r27 = r3'` and cr0 | PCE_MagnetCamTick |
| 0x02BD8AB8 | `lwz r0,0x14(r1)` | MagnetTarget(r3, r4=r31), r3 kept | PCE_MagnetTarget |

Read each HLE's exact argument/return use in `PCInput.cpp` when porting (e.g. `AimYawAbs` takes f1/f2).

## Ported so far (runtime/src/game/pc_camera.cpp)

Main camera (phase 3); zoomed aim 0x02B9250C + 0x02B92848/0x02B92A48, rune aim 0x02B94E30 + 0x02B95278/0x02B956B0,
camera rune 0x02BCED3C + 0x02BCF698, lock-on 0x02BF0234 (phase 7); climbing 0x02BEE0F8, conversation 0x02BCB3A0,
horse 0x02BD2A44, Master Cycle 0x02BDFB20 (phase 8); shield surf, sand seal, waterfall and Magnesis with all
their in-body patches from the table above (phase 9). While the Magnesis action 0x020D72D0 ticks, the lock-on hook leaves
that camera to the Magnesis hooks. Every camera with mouse support calls `feed()` at
its entry, which takes the mouse motion into the shared pending yaw/pitch; the zoomed ones (scope/bow, camera
rune) use `mouse_sens_aim` (settings.ini, default 0.8, Zelda-on-PC's AimSensitivity).

## 3. Special cases

- **0x020D6DAC `bl 0x03C6FD44`** (Magnesis distance): a branch, which BOTW_HOOK can't follow. Hook
  **0x020D6DB0** `fsubs f11,f1,f29` instead: compute the corrected distance from `f1` (Link's position at
  `r1+0x38`), then write `f1` and recompute `f11 = (float)(f1 - f29)`.
- **0x02B9C120** calls guest function 0x02B96758 from the hook. Call the recompiled `f_02b96758(c)` on a
  copy-saved context (restore everything except the result), since volatiles are live in the hook.
- The caves also reset `pceNormDelta = 0` in the main camera entry; keep that state host-side.

## Reference: other Zelda-on-PC patch groups

`[BotWPCE_UI]` (same file, after the camera patch) uses the same `bla` pattern for UI text, screens,
paraglider tick, map objects, menu cursor/list, region language, inventory slide/jump. Map them the same way
in phases 5/11.
