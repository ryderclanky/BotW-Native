// coreinit MEM: expanded heaps, frame heaps, allocators, default/base heaps, memory bounds.
// Heap headers live in guest memory (games read heapStart/heapEnd); allocation state is host-side.
#include <map>
#include <mutex>
#include <unordered_map>

#include "hle.h"
#include "loader.h"
#include "os.h"

namespace {
constexpr uint32_t kExpMagic = 0x45585048, kFrmMagic = 0x46524D48;  // 'EXPH', 'FRMH'
constexpr uint32_t kHeaderSize = 0x80;

struct Heap {
    uint32_t magic;
    uint32_t start, end;  // managed range
    uint32_t flags;
    // expanded heap
    std::map<uint32_t, uint32_t> free;   // addr -> size
    std::map<uint32_t, uint32_t> used;   // returned ptr -> (block start, size) packed below
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> blocks;  // ptr -> {block start, block size}
    // frame heap
    uint32_t head, tail;
};
std::recursive_mutex g_heap_lock;
std::unordered_map<uint32_t, Heap*> g_heaps;
uint32_t g_base_heap[9];
uint32_t g_default_heap = 0;

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void write_header(uint32_t h, const Heap& hp) {
    mem::commit(h, kHeaderSize, true);
    memset(mem::host(h), 0, kHeaderSize);
    mem::w32(h + 0x00, hp.magic);
    mem::w32(h + 0x18, hp.start);
    mem::w32(h + 0x1C, hp.end);
    mem::w8(h + 0x33, (uint8_t)hp.flags);
}

uint32_t exp_alloc(Heap* hp, uint32_t size, int32_t align) {
    bool tail = align < 0;
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    size = align_up(size ? size : 1, 4);
    if (!tail) {
        for (auto it = hp->free.begin(); it != hp->free.end(); ++it) {
            uint32_t a = align_up(it->first, al);
            if (a - it->first + size > it->second) continue;
            uint32_t bstart = it->first, bsize = it->second;
            hp->free.erase(it);
            if (a > bstart) hp->free[bstart] = a - bstart;
            uint32_t end = a + size;
            if (bstart + bsize > end) hp->free[end] = bstart + bsize - end;
            hp->blocks[a] = {a, size};
            return a;
        }
    } else {
        for (auto it = hp->free.rbegin(); it != hp->free.rend(); ++it) {
            uint32_t bend = it->first + it->second;
            if (bend < size) continue;
            uint32_t a = (bend - size) & ~(al - 1);
            if (a < it->first) continue;
            uint32_t bstart = it->first;
            hp->free.erase(std::next(it).base());
            if (a > bstart) hp->free[bstart] = a - bstart;
            if (bend > a + size) hp->free[a + size] = bend - (a + size);
            hp->blocks[a] = {a, size};
            return a;
        }
    }
    return 0;
}

void exp_free(Heap* hp, uint32_t p) {
    auto bit = hp->blocks.find(p);
    if (bit == hp->blocks.end()) {
        LOG("MEMFreeToExpHeap: %08x not allocated from heap %08x", p, hp->start);
        return;
    }
    uint32_t a = bit->second.first, s = bit->second.second;
    hp->blocks.erase(bit);
    auto nx = hp->free.lower_bound(a);
    if (nx != hp->free.end() && nx->first == a + s) {
        s += nx->second;
        hp->free.erase(nx);
    }
    auto pv = hp->free.lower_bound(a);
    if (pv != hp->free.begin()) {
        --pv;
        if (pv->first + pv->second == a) {
            pv->second += s;
            return;
        }
    }
    hp->free[a] = s;
}

uint32_t exp_largest(Heap* hp, int32_t align) {
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t best = 0;
    for (auto& [a, s] : hp->free) {
        uint32_t aa = align_up(a, al);
        if (aa - a < s) best = std::max(best, (s - (aa - a)) & ~3u);
    }
    return best;
}

uint32_t create_exp(uint32_t start, uint32_t size, uint32_t flags) {
    uint32_t h = align_up(start, 4);
    size -= h - start;
    if (size < kHeaderSize + 16) return 0;
    Heap* hp = new Heap{};
    hp->magic = kExpMagic;
    hp->start = h + kHeaderSize;
    hp->end = (h + size) & ~3u;
    hp->flags = flags;
    hp->free[hp->start] = hp->end - hp->start;
    write_header(h, *hp);
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    g_heaps[h] = hp;
    return h;
}

uint32_t create_frm(uint32_t start, uint32_t size, uint32_t flags) {
    uint32_t h = align_up(start, 4);
    size -= h - start;
    if (size < kHeaderSize + 16) return 0;
    Heap* hp = new Heap{};
    hp->magic = kFrmMagic;
    hp->start = h + kHeaderSize;
    hp->end = (h + size) & ~3u;
    hp->flags = flags;
    hp->head = hp->start;
    hp->tail = hp->end;
    write_header(h, *hp);
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    g_heaps[h] = hp;
    return h;
}

uint32_t g_mru_h0 = 0, g_mru_h1 = 0;
Heap* g_mru_p0 = nullptr;
Heap* g_mru_p1 = nullptr;

Heap* heap(uint32_t h) {
    if (h == g_mru_h0 && g_mru_p0) return g_mru_p0;
    if (h == g_mru_h1 && g_mru_p1) {
        std::swap(g_mru_h0, g_mru_h1);
        std::swap(g_mru_p0, g_mru_p1);
        return g_mru_p0;
    }
    auto it = g_heaps.find(h);
    if (it == g_heaps.end()) return nullptr;
    g_mru_h1 = g_mru_h0;
    g_mru_p1 = g_mru_p0;
    g_mru_h0 = h;
    g_mru_p0 = it->second;
    return it->second;
}

uint32_t heap_alloc(uint32_t h, uint32_t size, int32_t align) {
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(h);
    if (!hp) {
        LOG("MEM alloc from unknown heap %08x", h);
        return 0;
    }
    uint32_t p = 0;
    if (hp->magic == kExpMagic) {
        p = exp_alloc(hp, size, align);
    } else {
        uint32_t al = (uint32_t)(align < 0 ? -align : align);
        if (al < 4) al = 4;
        size = align_up(size, 4);
        if (align >= 0) {
            uint32_t a = align_up(hp->head, al);
            if (a + size <= hp->tail) {
                p = a;
                hp->head = a + size;
            }
        } else {
            uint32_t a = (hp->tail - size) & ~(al - 1);
            if (a >= hp->head && hp->tail >= size) {
                p = a;
                hp->tail = a;
            }
        }
    }
    if (p && (hp->flags & 1)) {
        mem::commit(p, size, true);
        memset(mem::host(p), 0, size);
    }
    if (!p) LOG("MEM: heap %08x out of memory (size %x align %d)", h, size, align);
    return p;
}

void heap_free(uint32_t h, uint32_t p) {
    if (!p) return;
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(h);
    if (hp && hp->magic == kExpMagic) exp_free(hp, p);
}

uint32_t g_dflt_alloc_thunk, g_dflt_allocex_thunk, g_dflt_free_thunk;
uint32_t g_allocator_funcs;  // {alloc, free} guest function pointer table for exp-heap allocators

void dflt_alloc(PPCContext* c) { RET(heap_alloc(g_default_heap, ARG(0), 0x40)); }
void dflt_allocex(PPCContext* c) { RET(heap_alloc(g_default_heap, ARG(0), (int32_t)ARG(1))); }
void dflt_free(PPCContext* c) { heap_free(g_default_heap, ARG(0)); }
// allocator->funcs->alloc(allocator, size): allocator = {funcs, heap, align, ukn}
void allocator_exp_alloc(PPCContext* c) {
    uint32_t a = ARG(0);
    RET(heap_alloc(mem::r32(a + 4), ARG(1), (int32_t)mem::r32(a + 8)));
}
void allocator_exp_free(PPCContext* c) { heap_free(mem::r32(ARG(0) + 4), ARG(1)); }

struct DataImports {
    DataImports() {
        // Runs at load time (before os::init): create the thunks here.
        loader::define_data_import("coreinit", "MEMAllocFromDefaultHeap", 4, [](uint32_t a) {
            g_dflt_alloc_thunk = dispatch::thunk(dflt_alloc, "default MEMAllocFromDefaultHeap");
            mem::w32(a, g_dflt_alloc_thunk);
        });
        loader::define_data_import("coreinit", "MEMAllocFromDefaultHeapEx", 4, [](uint32_t a) {
            g_dflt_allocex_thunk = dispatch::thunk(dflt_allocex, "default MEMAllocFromDefaultHeapEx");
            mem::w32(a, g_dflt_allocex_thunk);
        });
        loader::define_data_import("coreinit", "MEMFreeToDefaultHeap", 4, [](uint32_t a) {
            g_dflt_free_thunk = dispatch::thunk(dflt_free, "default MEMFreeToDefaultHeap");
            mem::w32(a, g_dflt_free_thunk);
        });
    }
} g_data_imports;
}  // namespace

namespace os {
uint32_t default_heap_alloc(uint32_t size, int32_t align) { return heap_alloc(g_default_heap, size, align); }
void default_heap_free(uint32_t ptr) { heap_free(g_default_heap, ptr); }

uint32_t mem2_bound_start() {
    return (loader::main_module().data_end + 0xFFFF) & ~0xFFFFu;
}

void init_heaps() {
    g_allocator_funcs = mem::sys_alloc(8, 4);
    mem::w32(g_allocator_funcs, dispatch::thunk(allocator_exp_alloc, "MEMAllocator exp alloc"));
    mem::w32(g_allocator_funcs + 4, dispatch::thunk(allocator_exp_free, "MEMAllocator exp free"));
    g_base_heap[0] = create_frm(mem::kMem1, mem::kMem1Size, 0);
    g_base_heap[8] = create_frm(mem::kFgBucket, 0x2800000, 0);
    uint32_t m2 = mem2_bound_start();
    g_default_heap = g_base_heap[1] = create_exp(m2, mem::kMem2 + mem::kMem2Size - m2, 0);
    LOG("heaps: MEM1 frm %08x, FG frm %08x, MEM2 exp %08x (%08x-%08x)", g_base_heap[0], g_base_heap[8], g_base_heap[1],
        m2, mem::kMem2 + mem::kMem2Size);
}
}  // namespace os

HLE_EXPORT(coreinit, MEMCreateExpHeapEx) { RET(create_exp(ARG(0), ARG(1), ARG(2))); }
HLE_EXPORT(coreinit, MEMCreateFrmHeapEx) { RET(create_frm(ARG(0), ARG(1), ARG(2))); }
HLE_EXPORT(coreinit, MEMDestroyExpHeap) {
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(ARG(0));
    if (hp) {
        if (g_mru_h0 == ARG(0)) { g_mru_h0 = 0; g_mru_p0 = nullptr; }
        if (g_mru_h1 == ARG(0)) { g_mru_h1 = 0; g_mru_p1 = nullptr; }
        g_heaps.erase(ARG(0));
        delete hp;
    }
    RET(ARG(0));
}
HLE_EXPORT(coreinit, MEMDestroyFrmHeap) { hle_coreinit_MEMDestroyExpHeap(c); }
HLE_EXPORT(coreinit, MEMAllocFromExpHeapEx) { RET(heap_alloc(ARG(0), ARG(1), (int32_t)ARG(2))); }
HLE_EXPORT(coreinit, MEMAllocFromFrmHeapEx) { RET(heap_alloc(ARG(0), ARG(1), (int32_t)ARG(2))); }
HLE_EXPORT(coreinit, MEMFreeToExpHeap) { heap_free(ARG(0), ARG(1)); }
HLE_EXPORT(coreinit, MEMGetAllocatableSizeForExpHeapEx) {
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(ARG(0));
    RET(hp ? exp_largest(hp, (int32_t)ARG(1)) : 0);
}
HLE_EXPORT(coreinit, MEMGetAllocatableSizeForFrmHeapEx) {
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(ARG(0));
    if (!hp) {
        RET(0);
        return;
    }
    int32_t align = (int32_t)ARG(1);
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t a = align_up(hp->head, al);
    RET(a < hp->tail ? hp->tail - a : 0);
}
HLE_EXPORT(coreinit, MEMGetTotalFreeSizeForExpHeap) {
    std::lock_guard<std::recursive_mutex> l(g_heap_lock);
    Heap* hp = heap(ARG(0));
    uint32_t t = 0;
    if (hp)
        for (auto& [a, s] : hp->free) t += s;
    RET(t);
}
HLE_EXPORT(coreinit, MEMGetBaseHeapHandle) {
    uint32_t i = ARG(0);
    RET(i < 9 ? g_base_heap[i] : 0);
}
HLE_EXPORT(coreinit, MEMInitAllocatorForExpHeap) {
    uint32_t a = ARG(0);
    mem::w32(a, g_allocator_funcs);
    mem::w32(a + 4, ARG(1));
    mem::w32(a + 8, ARG(2));
    mem::w32(a + 12, 0);
}
HLE_EXPORT(coreinit, MEMAllocFromAllocator) {
    uint32_t a = ARG(0);
    uint32_t funcs = mem::r32(a);
    if (__builtin_expect(funcs == g_allocator_funcs, 1)) {
        RET(heap_alloc(mem::r32(a + 4), ARG(1), (int32_t)mem::r32(a + 8)));
        return;
    }
    RET(guest_call(c, mem::r32(funcs), {a, ARG(1)}));
}
HLE_EXPORT(coreinit, MEMFreeToAllocator) {
    uint32_t a = ARG(0);
    uint32_t funcs = mem::r32(a);
    if (__builtin_expect(funcs == g_allocator_funcs, 1)) {
        heap_free(mem::r32(a + 4), ARG(1));
        return;
    }
    guest_call(c, mem::r32(funcs + 4), {a, ARG(1)});
}

// void OSGetMemBound(int type, void** start, uint32_t* size)
HLE_EXPORT(coreinit, OSGetMemBound) {
    uint32_t type = ARG(0), so = ARG(1), zo = ARG(2);
    uint32_t s = 0, z = 0;
    if (type == 1) {
        s = mem::kMem1;
        z = mem::kMem1Size;
    } else if (type == 2) {
        s = os::mem2_bound_start();
        z = mem::kMem2 + mem::kMem2Size - s;
    }
    if (so) mem::w32(so, s);
    if (zo) mem::w32(zo, z);
    RET(0);
}

// ---- block memory / cache ops ---------------------------------------------------------------
// Data-cache flushes tell the GPU buffer cache which CPU-written ranges (vertex / uniform data,
// e.g. skinning matrices) it has to re-upload, as in Cemu's coreinit.
void LatteBufferCache_notifyDCFlush(uint32_t address, uint32_t size);  // vendored Cemu GPU
namespace {
void dc_flush(uint32_t addr, uint32_t size) {
    uint32_t a = addr & ~31u, e = (addr + size + 31) & ~31u;
    LatteBufferCache_notifyDCFlush(a, e - a);
}
}  // namespace
// Copy/fill calls are also preemption points (the caller yields only when a strictly more urgent thread is queued
// for its core): a background thread that loops over them without any lock or event call (uiLowPrioThreadMgr converts the Continue screen's save picture with one memcpy per pixel)
// otherwise keeps its core while a more urgent thread waits for it (WorkerMgr/Worker2 on core 2, which the main
// thread waits for: ~0.2-0.5 s frames after Continue). One relaxed load per call when nobody waits.
namespace threads {
bool yield_to_waiter();  // thread.cpp
}
HLE_EXPORT(coreinit, memcpy) {
    threads::yield_to_waiter();
    mem::commit(ARG(0), ARG(2), true);
    mem::commit(ARG(1), ARG(2), false);
    memcpy(mem::host(ARG(0)), mem::host(ARG(1)), ARG(2));
    if (ARG(2)) LatteBufferCache_notifyDCFlush(ARG(0), ARG(2));  // COS memcpy always flushes
}
HLE_EXPORT(coreinit, memmove) {
    threads::yield_to_waiter();
    mem::commit(ARG(0), ARG(2), true);
    mem::commit(ARG(1), ARG(2), false);
    memmove(mem::host(ARG(0)), mem::host(ARG(1)), ARG(2));
    if (ARG(2)) LatteBufferCache_notifyDCFlush(ARG(0), ARG(2));
}
HLE_EXPORT(coreinit, memset) {
    threads::yield_to_waiter();
    mem::commit(ARG(0), ARG(2), true);
    memset(mem::host(ARG(0)), (int)(ARG(1) & 0xFF), ARG(2));
}
// OSBlockMove(dst, src, size, flushDC)
HLE_EXPORT(coreinit, OSBlockMove) {
    threads::yield_to_waiter();
    mem::commit(ARG(0), ARG(2), true);
    mem::commit(ARG(1), ARG(2), false);
    memmove(mem::host(ARG(0)), mem::host(ARG(1)), ARG(2));
    if (ARG(2) && (ARG(3) & 0xFF)) LatteBufferCache_notifyDCFlush(ARG(0), ARG(2));
}
HLE_EXPORT(coreinit, OSBlockSet) {
    threads::yield_to_waiter();
    mem::commit(ARG(0), ARG(2), true);
    memset(mem::host(ARG(0)), (int)(ARG(1) & 0xFF), ARG(2));
}
HLE_EXPORT(coreinit, DCFlushRange) { dc_flush(ARG(0), ARG(1)); }
HLE_EXPORT(coreinit, DCFlushRangeNoSync) { dc_flush(ARG(0), ARG(1)); }
HLE_EXPORT(coreinit, DCInvalidateRange) {}
HLE_EXPORT(coreinit, DCZeroRange) {
    uint32_t a = ARG(0) & ~31u, e = (ARG(0) + ARG(1) + 31) & ~31u;
    mem::commit(a, e - a, true);
    memset(mem::host(a), 0, e - a);
    LatteBufferCache_notifyDCFlush(a, e - a);
}
HLE_EXPORT(coreinit, OSIsAddressValid) { RET(ARG(0) >= 0x01000000); }
HLE_EXPORT(coreinit, OSIsAddressRangeDCValid) { RET(1); }
HLE_EXPORT(coreinit, OSCoherencyBarrier) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
HLE_EXPORT(coreinit, OSMemoryBarrier) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
HLE_EXPORT(coreinit, OSEnableOverlayArena) { RET(0); }
HLE_EXPORT(coreinit, OSIsEnabledOverlayArena) { RET(0); }

// ---- locked cache (per-core 16 KB scratch + DMA to/from memory) -----------------------------
namespace {
std::mutex g_lc_lock;
uint32_t g_lc_used[3];
bool g_lc_dma[3];
uint32_t lc_base(int core) { return mem::kLockedCache + core * 0x40000; }
}  // namespace
HLE_EXPORT(coreinit, LCAlloc) {
    int core = c->core;
    uint32_t size = (ARG(0) + 511) & ~511u;
    std::lock_guard<std::mutex> l(g_lc_lock);
    if (g_lc_used[core] + size > 0x4000) {
        RET(0);
        return;
    }
    uint32_t a = lc_base(core) + g_lc_used[core];
    g_lc_used[core] += size;
    RET(a);
}
HLE_EXPORT(coreinit, LCDealloc) {
    std::lock_guard<std::mutex> l(g_lc_lock);
    g_lc_used[c->core] = 0;  // games free everything in reverse order; good enough
}
HLE_EXPORT(coreinit, LCGetUnallocated) { RET(0x4000 - g_lc_used[c->core]); }
HLE_EXPORT(coreinit, LCEnableDMA) { g_lc_dma[c->core] = true; RET(1); }
HLE_EXPORT(coreinit, LCDisableDMA) { g_lc_dma[c->core] = false; }
HLE_EXPORT(coreinit, LCIsDMAEnabled) { RET(g_lc_dma[c->core]); }
// LCLoadDMABlocks(lcDst, memSrc, numBlocks) / LCStoreDMABlocks(memDst, lcSrc, numBlocks): 32-byte blocks, 0 = 128
HLE_EXPORT(coreinit, LCLoadDMABlocks) {
    uint32_t n = ARG(2) ? ARG(2) : 128;
    mem::commit(ARG(0), n * 32, true);
    mem::commit(ARG(1), n * 32, false);
    memcpy(mem::host(ARG(0)), mem::host(ARG(1)), n * 32);
}
HLE_EXPORT(coreinit, LCStoreDMABlocks) {
    uint32_t n = ARG(2) ? ARG(2) : 128;
    mem::commit(ARG(0), n * 32, true);
    mem::commit(ARG(1), n * 32, false);
    memcpy(mem::host(ARG(0)), mem::host(ARG(1)), n * 32);
}
HLE_EXPORT(coreinit, LCGetDMAQueueLength) { RET(0); }
HLE_EXPORT(coreinit, LCWaitDMAQueue) {}
