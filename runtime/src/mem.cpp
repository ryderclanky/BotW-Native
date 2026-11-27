#include "mem.h"

#include <stdlib.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <atomic>
#include <map>
#include <mutex>

#include "log.h"

uint8_t* g_mem = nullptr;
uint8_t* memory_base = nullptr;  // Cemu-compat alias of g_mem

namespace mem {
uint64_t g_fast_commit_end = 0;
namespace {
// Simple first-fit allocator over [kSysBase, kSysEnd); runtime allocations are few and long-lived.
std::mutex g_lock;
std::map<uint32_t, uint32_t> g_free = {{kSysBase, kSysEnd - kSysBase}};  // addr -> size
std::map<uint32_t, uint32_t> g_used;
#if defined(_WIN32)
DWORD g_page = 4096;

bool page_writable(DWORD protect) {
    return (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool page_readable(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Committed guest intervals, published without a lock on the hit path. Each slot only grows, so a
// torn read is a subset of memory that is already committed: treating it as covered is safe, and
// missing it just takes the slow path. The mutex is for writers (startup and the first touch of a
// new range). A contended lock on memset would put a syscall back on the hot path.
struct Iv {
    std::atomic<uint64_t> b{0};
    std::atomic<uint64_t> e{0};  // [b, e); e == 0 is empty
};
constexpr int kMaxIv = 48;
Iv g_iv[kMaxIv];
std::atomic<int> g_iv_n{0};
std::mutex g_iv_mu;
// Guest address of the single BOTW_WATCH page, or this when the page is writable.
std::atomic<uint64_t> g_ro_page{UINT64_MAX};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "interval loads must not take a lock");
static_assert(std::atomic<int>::is_always_lock_free, "interval count must not take a lock");

bool covered(uint64_t addr, uint64_t end) {
    uint64_t b0 = g_iv[0].b.load(std::memory_order_relaxed);
    uint64_t e0 = g_iv[0].e.load(std::memory_order_relaxed);
    if (__builtin_expect(e0 != 0 && addr >= b0 && end <= e0, 1)) return true;
    int n = g_iv_n.load(std::memory_order_acquire);
    for (int i = 1; i < n; i++) {
        uint64_t b = g_iv[i].b.load(std::memory_order_relaxed);
        uint64_t e = g_iv[i].e.load(std::memory_order_relaxed);
        if (e && addr >= b && end <= e) return true;
    }
    return false;
}

bool watch_blocks_write(uint64_t begin, uint64_t end) {
    uint64_t p = g_ro_page.load(std::memory_order_relaxed);
    if (__builtin_expect(p == UINT64_MAX, 1)) return false;
    if (!g_page) return false;
    return begin < p + g_page && end > p;
}

void add_interval(uint64_t b, uint64_t e) {
    if (e <= b) return;
    std::lock_guard<std::mutex> lock(g_iv_mu);
    int n = g_iv_n.load(std::memory_order_relaxed);
    int host = -1;
    // Repeat so a window to the left of the scan is merged once a later slot pulls the range
    // over to it. Otherwise a memset that crosses that boundary would miss every slot.
    bool grew = true;
    while (grew) {
        grew = false;
        for (int i = 0; i < n; i++) {
            uint64_t ib = g_iv[i].b.load(std::memory_order_relaxed);
            uint64_t ie = g_iv[i].e.load(std::memory_order_relaxed);
            // Adjacent spans (e == ib or b == ie) merge, so code/sys and MEM2 become one window.
            if (!ie || e < ib || b > ie) continue;
            if (ib < b) {
                b = ib;
                grew = true;
            }
            if (ie > e) {
                e = ie;
                grew = true;
            }
            if (host < 0) host = i;
        }
    }
    if (host >= 0) {
        uint64_t hb = g_iv[host].b.load(std::memory_order_relaxed);
        uint64_t he = g_iv[host].e.load(std::memory_order_relaxed);
        // Start moves first. A reader can see the old interval or a larger one, and both are
        // committed. Slots that were absorbed stay as subsets; shrinking one would be a race.
        if (b < hb) g_iv[host].b.store(b, std::memory_order_relaxed);
        if (e > he) g_iv[host].e.store(e, std::memory_order_release);
        return;
    }
    if (n >= kMaxIv) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            LOG("guest commit: interval table full (%d), %08llx-%08llx and later new ranges use the per-thread page "
                "cache", kMaxIv, (unsigned long long)b, (unsigned long long)e);
        return;
    }
    g_iv[n].b.store(b, std::memory_order_relaxed);
    g_iv[n].e.store(e, std::memory_order_relaxed);
    g_iv_n.store(n + 1, std::memory_order_release);
}
#endif
}  // namespace

void init() {
    size_t size = (1ull << 32) + 0x10000;  // + guard so accesses near 0xFFFFFFFF stay in bounds
#if defined(_WIN32)
    // Reserve only. Committing all 4 GB fails on a small pagefile. The Cafe windows are committed
    // just below; a vectored handler still commits anything outside them on first touch. The low
    // 64 KB stays reserved and inaccessible so a guest null access is still fatal.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwPageSize) g_page = si.dwPageSize;
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        LOG("FATAL: cannot reserve the 4 GB guest address space (VirtualAlloc %lu)", GetLastError());
        abort();
    }
    g_mem = (uint8_t*)p;
    memory_base = g_mem;
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOG("FATAL: cannot reserve the 4 GB guest address space");
        abort();
    }
    g_mem = (uint8_t*)p;
    memory_base = g_mem;
    mprotect(g_mem, 0x10000, PROT_NONE);  // catch guest null-pointer accesses
#endif
    LOG("guest memory at %p", (void*)g_mem);
    // Windows commits the Cafe windows the game and the GPU actually touch. Commit charge is not
    // resident memory: the pages stay demand-zero until first use. The hole from the end of MEM2
    // to the foreground bucket, and the overlay arena, stay reserved.
#if defined(_WIN32)
    struct Range {
        uint32_t addr;
        uint32_t size;
        const char* name;
    };
    // Matches the ranges in runtime/cemu_shims/cemu_shims_impl.cpp. Code cave through the sys heap
    // is one span (0x01800000..0x10000000). Locked cache is three cores at a 256 KB stride.
    const Range ranges[] = {
        {0x01800000, 0x0E800000, "code/sys"},
        {kMem2, kMem2Size, "MEM2"},
        {kMem1, kMem1Size, "MEM1"},
        {kFgBucket, kFgBucketSize, "foreground"},
        {0xE8000000, 0x02000000, "tiling"},
        {0xF6000000, 0x02000000, "rpl"},
        {0xF8000000, 0x02000000, "shared"},
        {kLockedCache, 0x000C0000, "locked cache"},
        {0x00E00000, 0x00200000, "trampoline"},
        {0x00010000, 0x000F0000, "low"},
    };
    uint64_t charged = 0;
    for (const Range& r : ranges) {
        if (commit(r.addr, r.size, true)) {
            charged += r.size;
            continue;
        }
        LOG("guest commit failed for %s at %08x size %x (err %lu); that span commits per access", r.name, r.addr,
            r.size, (unsigned long)GetLastError());
    }
    if (covered(0x01800000ull, 0x50000000ull)) g_fast_commit_end = 0x50000000ull;
    LOG("guest commit charge %llu MB (not resident)", (unsigned long long)(charged >> 20));
#else
    g_fast_commit_end = 0x50000000ull;
#endif
}

bool commit_slow(uint32_t addr, size_t bytes, bool write) {
#if !defined(_WIN32)
    if (bytes == 0) return true;
    if (!g_mem) return false;
    uint64_t end = (uint64_t)addr + bytes;
    return addr >= 0x10000 && end >= (uint64_t)addr && end <= (1ull << 32);
#else
    if (bytes == 0) return true;
    if (!g_mem || !g_page) return false;
    uint64_t begin = addr;
    uint64_t end = begin + bytes;
    if (end < begin || begin < 0x10000 || end > (1ull << 32)) return false;
    // Hot path: memset, OSBlockMove, OSInitEvent. No VirtualQuery and no lock.
    if (covered(begin, end)) {
        if (write && watch_blocks_write(begin, end)) return false;
        return true;
    }
    // Per-thread cache of spans this thread already verified (guest memory is never decommitted, so a verified
    // span stays committed). Catches ranges the interval table could not record once it is full: e.g. the
    // Continue screen's JPEG decoder at 0xA88xxxxx copies 3 bytes per pixel, and each miss cost a VirtualQuery.
    // A span verified for reading only does not satisfy a write check.
    struct Seen {
        uint64_t b, e;
        bool w;
    };
    thread_local Seen seen[4] = {};
    thread_local unsigned seen_next = 0;
    for (const Seen& s : seen) {
        if (s.e && begin >= s.b && end <= s.e && (s.w || !write)) {
            if (write && watch_blocks_write(begin, end)) return false;
            return true;
        }
    }
    const uintptr_t base = (uintptr_t)g_mem;
    const uintptr_t mask = (uintptr_t)g_page - 1;
    uintptr_t start = (base + (uintptr_t)begin) & ~mask;
    uintptr_t last = (base + (uintptr_t)end + mask) & ~mask;
    if (start < base + 0x10000) return false;
    for (uintptr_t p = start; p < last;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)p, &mbi, sizeof mbi) || mbi.RegionSize == 0) return false;
        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return false;
        uintptr_t slice_end = region_end < last ? region_end : last;
        if (mbi.State != MEM_COMMIT) {
            // MEM_COMMIT of an already-reserved span. Pages that are already committed (none in
            // this region) would keep their protection; a fresh commit is PAGE_READWRITE.
            if (!VirtualAlloc((void*)p, slice_end - p, MEM_COMMIT, PAGE_READWRITE)) {
                for (uintptr_t q = p; q < slice_end; q += g_page) {
                    if (VirtualAlloc((void*)q, g_page, MEM_COMMIT, PAGE_READWRITE)) continue;
                    MEMORY_BASIC_INFORMATION again{};
                    if (!VirtualQuery((void*)q, &again, sizeof again) || again.State != MEM_COMMIT) return false;
                    if (write ? !page_writable(again.Protect) : !page_readable(again.Protect)) return false;
                }
            }
        } else if (write ? !page_writable(mbi.Protect) : !page_readable(mbi.Protect)) {
            return false;
        }
        p = slice_end;
    }
    // Record the pages this call actually committed. The next memset of the same object hits
    // covered() above. commit_on_fault does not record; the next commit() of that page does.
    uint64_t gb = (uint64_t)(start - base);
    uint64_t ge = (uint64_t)(last - base);
    if (ge > (1ull << 32)) ge = (1ull << 32);
    add_interval(gb, ge);
    seen[seen_next++ & 3] = Seen{gb, ge, write};
    return true;
#endif
}

#if defined(_WIN32)
void note_writable(uint32_t addr, bool writable) {
    if (!g_page) return;
    uint64_t page = (uint64_t)addr & ~((uint64_t)g_page - 1);
    if (writable) {
        uint64_t cur = g_ro_page.load(std::memory_order_relaxed);
        if (cur == page) {
            g_ro_page.store(UINT64_MAX, std::memory_order_release);
            if (covered(0x01800000ull, 0x50000000ull)) g_fast_commit_end = 0x50000000ull;
        }
    } else {
        g_fast_commit_end = 0;
        g_ro_page.store(page, std::memory_order_release);
    }
}
#endif

#if defined(_WIN32)
bool commit_on_fault(void* addr) {
    if (!g_mem || !addr) return false;
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)g_mem;
    uintptr_t begin = base + 0x10000;
    uintptr_t end = base + (1ull << 32) + 0x10000;
    if (a < begin || a >= end) return false;
    uintptr_t page = a & ~((uintptr_t)g_page - 1);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)page, &mbi, sizeof mbi)) return false;
    if (mbi.State == MEM_COMMIT) return page_writable(mbi.Protect);
    if (VirtualAlloc((void*)page, g_page, MEM_COMMIT, PAGE_READWRITE)) return true;
    // Another thread can commit the same page between the query and the alloc.
    if (!VirtualQuery((void*)page, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return page_writable(mbi.Protect);
}
#endif

uint32_t sys_alloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> l(g_lock);
    size = (size + 15) & ~15u;
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        uint32_t a = (it->first + align - 1) & ~(align - 1);
        uint32_t pad = a - it->first;
        if (it->second < pad + size) continue;
        uint32_t base = it->first, len = it->second;
        g_free.erase(it);
        if (pad) g_free[base] = pad;
        if (len > pad + size) g_free[a + size] = len - pad - size;
        g_used[a] = size;
        commit(a, size, true);
        memset(g_mem + a, 0, size);
        return a;
    }
    LOG("FATAL: runtime guest heap exhausted (size %x)", size);
    abort();
}

void sys_free(uint32_t addr) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_used.find(addr);
    if (it == g_used.end()) return;
    uint32_t a = addr, s = it->second;
    g_used.erase(it);
    auto nx = g_free.lower_bound(a);
    if (nx != g_free.end() && nx->first == a + s) {
        s += nx->second;
        g_free.erase(nx);
    }
    auto pv = g_free.lower_bound(a);
    if (pv != g_free.begin()) {
        --pv;
        if (pv->first + pv->second == a) {
            pv->second += s;
            return;
        }
    }
    g_free[a] = s;
}

bool readable(uint32_t addr, uint32_t bytes) {
    if (!g_mem || bytes == 0) return false;
    uint64_t end = (uint64_t)addr + bytes;
    if (end > (1ull << 32)) return false;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* p = g_mem + addr;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)(p + bytes) <= region_end;
#else
    return addr >= 0x10000;
#endif
}

uint32_t sys_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    uint32_t a = sys_alloc((uint32_t)n, 4);
    memcpy(g_mem + a, s, n);
    return a;
}
}  // namespace mem

// Host pointer inside the guest space. Used by the GPU, which hands Cafe pointers to memcpy and,
// on the host-imported buffer path, to the driver.
void memory_commit_span(const void* ptr, size_t bytes) {
    if (!g_mem || !ptr || bytes == 0) return;
    uintptr_t a = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)g_mem;
    uintptr_t limit = base + (1ull << 32);
    if (a < base + 0x10000 || a >= limit) return;
    size_t max = (size_t)(limit - a);
    if (bytes > max) bytes = max;
    mem::commit((uint32_t)(a - base), bytes, true);
}
