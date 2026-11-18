// UI state for the PC controls. Port of Zelda-on-PC's [BotWPCE_UI] patches (src/pcedition/PCGraphics.cpp and
// PCUI.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC). This Source Code Form is subject to the terms
// of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
// one at https://mozilla.org/MPL/2.0/. Phase 11 ports the rest (screens, texts, menu cursor).
#include <atomic>
#include <chrono>

#include "hooks.h"

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int64_t> g_map_ms{-1000000};
}  // namespace

// The world map is on screen (its update ran in the last 150 ms): the wheel zooms it.
bool pce_map_screen() { return now_ms() - g_map_ms.load() < 150; }

BOTW_HOOK(0x02f033e0) { g_map_ms = now_ms(); }  // lwz r6,0xa8(r27): world map screen update, r27 = map screen
