// Game patches as host hooks. BOTW_HOOK(0xADDR) { ... } runs right after the (non-branch) guest
// instruction at ADDR; tools/recompile.py finds these by text, so write each at the start of a line
// with a literal 0x%08x address and re-run the recompiler after adding or removing one.
// Addresses are for U-King.rpx 1.5.0 (v208).
#pragma once
#include "../hle.h"
#include "../os.h"

#define BOTW_HOOK(addr) extern "C" void hook_##addr(PPCContext* c)

// The calling thread's next GX2WaitForVsync returns immediately.
extern thread_local bool g_skip_vsync_wait;
// Frame pacing for the 30 fps cap: wait until `interval` vblanks after the last paced frame. A frame
// that is already late does not wait (shows at the next vblank instead of the one after).
void pace_vblanks(uint32_t interval);
