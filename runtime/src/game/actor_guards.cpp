// Null guard for a guest helper that a WorkerMgr task can reach with an actor that is being torn down.
//
// 0x03394870 (looks like Actor::hasTag(hash)):
//   if (!actor) return 0; r12 = actor->[0x39c]; r3 = r12->[0x68]; tail call 0x0371b140(r3, hash)
// 0x0371b140 binary-searches (r3+0x4a0)[0 .. *(r3+0x49c)) and returns 0 when the count is <= 0.
//
// Crash seen ~33 min into open-world play on WorkerMgr/Worker1(Any), stack 02a2c220 02a2c3b0 02a3ccf4
// 02d61844 ...: actor+0x39c was NULL (fault at 0x68) in one run, and actor+0x39c valid but its +0x68 NULL
// (fault at 0x49c) in another. The runtime heap never poisons or clears freed memory, so those NULLs were
// written by the game itself: the actor's param/resources were released on another core while the worker
// still held the actor. The hooks below swap a bad pointer for a zeroed block, so the lookup sees zero
// tags and returns false; the caller (0x02a2c144) then takes its "no actor" path. This hides the symptom;
// it does not fix the underlying ordering.
//
// Done as BOTW_HOOKs rather than a function override: an override turns on the per-call override check
// in every recompiled function, which slows the whole game down to a crawl.
#include <atomic>

#include "../log.h"
#include "../mem.h"
#include "hooks.h"

namespace {

// Below this is never a heap or data pointer (text starts at 0x02000000); catches NULL and NULL+offset.
inline bool bad_ptr(uint32_t p) { return p < 0x01000000u; }

// Zeroed guest block standing in for both missing objects: its +0x68 is 0 and its +0x49c (count) is 0.
// Only ever read by the guest.
uint32_t empty_block() {
    static const uint32_t a = [] {
        uint32_t p = mem::sys_alloc(0x500, 16);
        for (uint32_t i = 0; i < 0x500; i += 4) mem::w32(p + i, 0);
        return p;
    }();
    return a;
}

std::atomic<uint32_t> g_hits{0};

void note_hit(PPCContext* c, const char* what, uint32_t obj) {
    uint32_t n = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || n % 1000 == 0)
        LOG("actor guard: hasTag on a released actor (%s is NULL, obj %08x, key %08x), returned false "
            "[lr %08x, hit %u]",
            what, obj, c->r[4], c->lr, n);
}

}  // namespace

BOTW_HOOK(0x03394880) {  // lwz r12, 0x39c(r3)
    if (bad_ptr(c->r[12])) {
        note_hit(c, "+39c", c->r[3]);
        c->r[12] = empty_block();  // next: lwz r3, 0x68(r12) -> 0, caught below
    }
}

BOTW_HOOK(0x03394884) {  // lwz r3, 0x68(r12)
    if (bad_ptr(c->r[3])) {
        if (c->r[12] != empty_block()) note_hit(c, "+39c->+68", c->r[12]);
        c->r[3] = empty_block();  // count at +0x49c is 0: the lookup returns false
    }
}
