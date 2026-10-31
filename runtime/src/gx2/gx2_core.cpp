// GX2 core: GX2Init, command buffer pool and submission, display lists, timestamps,
// swap/vsync/flip and event callbacks. Packet layouts follow Cemu's GX2_Command.cpp / GX2.cpp.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "../bench/bench.h"
#include "../compat/cemu_export.h"
#include "../os.h"
#include "../thread.h"
#include "GX2_Command.h"

thread_local bool g_skip_vsync_wait = false;  // see game/hooks.h

void pace_vblanks(uint32_t interval) {
    static uint64_t last = 0;
    uint64_t now = latte::vsync_count();
    uint64_t target = last + interval;
    if (last == 0 || now >= target || target - now > interval) {  // first frame, late, or counter jumped
        last = now;
        return;
    }
    {
        threads::WaitMark w("vsync pace", 0);
        threads::CoreHandoff handoff;
        handoff.yield();
        latte::wait_vsync_count(target);
    }
    last = target;
}

namespace GX2 {
uint32 sGX2MainCoreIndex = 1;
namespace {
void wait_gpu(uint64_t ts, const char* why) {
    if (latte::retired() >= ts) return;
    threads::WaitMark w(why, (uint32_t)ts);
    threads::CoreHandoff handoff;
    handoff.yield();
    latte::wait_retired(ts);
}
}  // namespace
static thread_local GX2PerCoreCBState t_cb_storage;
thread_local GX2PerCoreCBState* t_cb_ptr = nullptr;
__attribute__((noinline)) GX2PerCoreCBState& cb_slow() {
    t_cb_ptr = &t_cb_storage;
    return t_cb_storage;
}
#define t_cb (cb())
namespace {
thread_local GX2PerCoreCBState t_saved_cb;  // command buffer state while recording a display list

// command pool: ring buffer of command buffers; in-flight regions retire with their timestamps
std::mutex g_pool_lock;
uint32 g_pool_base = 0, g_pool_size = 0;  // bytes
uint32 g_pool_write = 0;                   // next free offset
struct InFlight {
    uint32 start, end;  // pool offsets [start, end)
    uint64 ts;
};
std::deque<InFlight> g_inflight;
uint32 g_init_count = 0;

constexpr uint32 kMinBufferWords = 0x4000;  // 64 KB

bool overlaps(uint32 a0, uint32 a1, uint32 b0, uint32 b1) { return a0 < b1 && b0 < a1; }

// Allocate `words` of pool space, waiting for the GPU to retire buffers that occupy it.
uint32 pool_alloc(uint32 words) {
    uint32 bytes = words * 4;
    std::unique_lock<std::mutex> l(g_pool_lock);
    if (bytes > g_pool_size) bytes = g_pool_size;
    uint32 start = g_pool_write;
    if (start + bytes > g_pool_size) start = 0;
    uint32 end = start + bytes;
    for (;;) {
        // retire finished buffers
        uint64 r = latte::retired();
        while (!g_inflight.empty() && g_inflight.front().ts <= r) g_inflight.pop_front();
        bool busy = false;
        uint64 wait_ts = 0;
        for (auto& f : g_inflight)
            if (overlaps(start, end, f.start, f.end)) {
                busy = true;
                wait_ts = f.ts;
                break;
            }
        if (!busy) break;
        l.unlock();
        wait_gpu(wait_ts, "gpu: cmd pool full");
        l.lock();
    }
    g_pool_write = end;
    return g_pool_base + start;
}

void start_new_buffer(uint32 words) {
    words = std::max(words + 8, kMinBufferWords);
    uint32 a = pool_alloc(words);
    t_cb.bufferPtr = (uint32be*)mem::host(a);
    t_cb.bufferSizeInU32s = words;
    t_cb.currentWritePtr = t_cb.bufferPtr;
    t_cb.isDisplayList = false;
}

void pad_current() {
    if (!t_cb.currentWritePtr) return;
    while ((t_cb.currentWritePtr - t_cb.bufferPtr) & 7) *t_cb.currentWritePtr++ = pm4HeaderType2Filler();
}

void submit(uint32 addr, uint32 words) {
    uint64 ts = latte::submit(addr, words);
    uint32 off = addr - g_pool_base;
    if (addr >= g_pool_base && off < g_pool_size) {
        std::lock_guard<std::mutex> l(g_pool_lock);
        g_inflight.push_back({off, off + words * 4, ts});
    }
}

// ---- event callbacks ----------------------------------------------------------------------------
struct EventCallback {
    uint32 func = 0, user = 0;
} g_event_cb[5];
std::mutex g_ev_lock;
std::condition_variable g_ev_cv;
std::deque<uint32> g_ev_queue;

void event_thread() {
    threads::GuestThread* t = threads::adopt_host_thread("GX2 callbacks", 0x10000, 1);
    for (;;) {
        uint32 type;
        EventCallback cbk;
        {
            std::unique_lock<std::mutex> l(g_ev_lock);
            g_ev_cv.wait(l, [] { return !g_ev_queue.empty(); });
            type = g_ev_queue.front();
            g_ev_queue.pop_front();
            cbk = g_event_cb[type];
        }
        if (cbk.func) guest_call(&t->ctx, cbk.func, {type, cbk.user});
    }
}
}  // namespace

void GX2Command_Flush(uint32 numU32sForNextBuffer, bool triggerMarkerInterrupt) {
    if (t_cb.isDisplayList) return;
    if (!g_pool_base) return;
    if (t_cb.currentWritePtr && t_cb.currentWritePtr != t_cb.bufferPtr) {
        pad_current();
        uint32 words = (uint32)(t_cb.currentWritePtr - t_cb.bufferPtr);
        submit(memory_getVirtualOffsetFromPointer(t_cb.bufferPtr), words);
        // return unused space at the end of the buffer to the pool
        std::lock_guard<std::mutex> l(g_pool_lock);
        uint32 used_end = memory_getVirtualOffsetFromPointer(t_cb.currentWritePtr) - g_pool_base;
        uint32 buf_end = memory_getVirtualOffsetFromPointer(t_cb.bufferPtr + t_cb.bufferSizeInU32s) - g_pool_base;
        if (g_pool_write == buf_end) g_pool_write = used_end;
    }
    start_new_buffer(numU32sForNextBuffer);
}

void GX2ReserveCmdSpace_Slow(uint32 n) {
    if (!t_cb.currentWritePtr) {
        if (!g_pool_base) return;
        start_new_buffer(n);
        return;
    }
    uint32 used = (uint32)(t_cb.currentWritePtr - t_cb.bufferPtr);
    // command buffers keep 8 words for end-of-buffer padding; display lists are sized exactly
    if (used + n + (t_cb.isDisplayList ? 0 : 8) > t_cb.bufferSizeInU32s) {
        if (t_cb.isDisplayList) {
            LOG("GX2: display list overflow (%u + %u > %u words)", used, n, t_cb.bufferSizeInU32s);
            return;
        }
        GX2Command_Flush(n, true);
    }
}

uint64 GX2GetLastSubmittedTimeStamp() { return latte::last_submitted(); }
uint64 GX2GetRetiredTimeStamp() { return latte::retired(); }
bool GX2WaitTimeStamp(uint64 ts) {
    wait_gpu(ts, "gpu: GX2WaitTimeStamp");
    return true;
}
void GX2Flush() { GX2Command_Flush(0x100, true); }
bool GX2DrawDone() {
    GX2Command_Flush(0x100, true);
    wait_gpu(latte::last_submitted(), "gpu: GX2DrawDone");
    return true;
}

void GX2BeginDisplayListEx(MEMPTR<void> addr, uint32 size, bool profiling) {
    if (!t_cb.isDisplayList) {
        pad_current();
        t_saved_cb = t_cb;
    }
    t_cb.bufferPtr = (uint32be*)addr.GetPtr();
    t_cb.bufferSizeInU32s = size / 4;
    t_cb.currentWritePtr = t_cb.bufferPtr;
    t_cb.isDisplayList = true;
}
void GX2BeginDisplayList(MEMPTR<void> addr, uint32 size) { GX2BeginDisplayListEx(addr, size, false); }
uint32 GX2EndDisplayList(MEMPTR<void> addr) {
    if (!t_cb.isDisplayList) return 0;
    pad_current();
    uint32 bytes = (uint32)(t_cb.currentWritePtr - t_cb.bufferPtr) * 4;
    t_cb = t_saved_cb;
    t_saved_cb = {};
    return bytes;
}
bool GX2GetDisplayListWriteStatus() { return t_cb.isDisplayList; }
bool GX2GetCurrentDisplayList(MEMPTR<uint32be>* addr, uint32be* size) {
    if (!t_cb.isDisplayList) return false;
    if (addr) *addr = t_cb.bufferPtr;
    if (size) *size = t_cb.bufferSizeInU32s * 4;
    return true;
}
void GX2CallDisplayList(MPTR addr, uint32 size) {
    GX2ReserveCmdSpace(4);
    gx2WriteGather_submit(pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3), addr, 0u, size / 4);
}
void GX2DirectCallDisplayList(void* addr, uint32 size) {
    if (!t_cb.isDisplayList) GX2Command_Flush(0x100, false);
    submit(memory_getVirtualOffsetFromPointer(addr), size / 4);
}
void GX2CopyDisplayList(MEMPTR<uint32be*> addr, uint32 size) {
    uint32 n = size / 4;
    if (!n) return;
    GX2ReserveCmdSpace(n);
    gx2WriteGather_submitU32AsLEArray((uint32*)addr.GetPtr(), n);
}

// GX2PatchDisplayList(dl, type, byteOffset, shader): rewrite the program address of a shader
// set packet recorded in a display list (type 1 fetch, 2 vertex, 3 GS copy, 4 GS, 5 pixel).
static uint32 program_addr(uint32 shader, uint32 ptr_off, uint32 rbuf_off) {
    uint32 p = mem::r32(shader + ptr_off);
    return p ? p : mem::r32(shader + rbuf_off + 0xC);
}
void GX2PatchDisplayList(uint32be* dl, uint32 type, uint32 offset, void* obj) {
    uint32 s = memory_getVirtualOffsetFromPointer(obj);
    uint32 a = 0;
    switch (type) {
        case 1: a = mem::r32(s + 0x0C); break;
        case 2: a = program_addr(s, 0xD4, 0x124); break;
        case 3: a = program_addr(s, 0x58, 0xB0); break;
        case 4: a = program_addr(s, 0x50, 0xA0); break;
        case 5: a = program_addr(s, 0xA8, 0xD8); break;
        default: LOG("GX2PatchDisplayList: type %u", type); return;
    }
    dl[offset / 4 + 2] = a >> 8;
}

// ---- init / swap / sync ---------------------------------------------------------------------
enum InitArg : uint32 { EndOfArgs = 0, CommandPoolBase = 1, CommandPoolSize = 2 };
void GX2Init(betype<uint32>* args) {
    if (g_init_count++) return;
    uint32 base = 0, size = 0;
    if (args)
        for (;;) {
            uint32 id = *args++;
            if (id == EndOfArgs) break;
            uint32 v = *args++;
            if (id == CommandPoolBase) base = v;
            else if (id == CommandPoolSize) size = v;
        }
    if (!size) size = 0x400000;
    if (!base) base = os::default_heap_alloc(size, 0x100);
    mem::commit(base, size, true);
    g_pool_base = base;
    g_pool_size = size;
    sGX2MainCoreIndex = threads::current() ? threads::current()->core : 1;
    LOG("GX2Init: command pool %08x size %x, main core %u", base, size, sGX2MainCoreIndex);
    latte::set_event_sink([](latte::Event e) {
        uint32 i = (uint32)e;
        if (i < 5) {
            std::lock_guard<std::mutex> l(g_ev_lock);
            if (g_event_cb[i].func) {
                g_ev_queue.push_back(i);
                g_ev_cv.notify_one();
            }
        }
    });
    std::thread(event_thread).detach();
    latte::init();
    start_new_buffer(kMinBufferWords);
}
void GX2Shutdown() {
    if (g_init_count && --g_init_count == 0) GX2DrawDone();
}

void GX2SwapScanBuffers() {
    GX2ReserveCmdSpace(8);
    latte::add_flip_request();
    gx2WriteGather_submit(pm4HeaderType3(IT_HLE_REQUEST_SWAP_BUFFERS, 1), 0u, pm4HeaderType3(IT_HLE_TRIGGER_SCANBUFFER_SWAP, 1), 0u);
    g_game_frames++;
    bench::on_swap();
    while ((sint32)(latte::flip_requests() - latte::flips_executed()) > 5) {
        threads::WaitMark w("flip (swap backlog)", 0);
        threads::CoreHandoff handoff;
        handoff.yield();
        latte::wait_for_flip();
    }
}
void GX2CopyColorBufferToScanBuffer(uint32be* colorBuffer, uint32 target) {
    uint32 cbuf = memory_getVirtualOffsetFromPointer(colorBuffer);
    GX2ReserveCmdSpace(10);
    gx2WriteGather_submit(pm4HeaderType3(IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER, 9), mem::r32(cbuf + 0x24),
                          mem::r32(cbuf + 0x04), mem::r32(cbuf + 0x08), mem::r32(cbuf + 0x3C), mem::r32(cbuf + 0x30),
                          mem::r32(cbuf + 0x34), mem::r32(cbuf + 0x78), mem::r32(cbuf + 0x14), target);
}
void GX2GetSwapStatus(uint32be* swapCount, uint32be* flipCount, uint64be* lastFlip, uint64be* lastVsync) {
    if (swapCount) *swapCount = latte::flip_requests();
    if (flipCount) *flipCount = latte::flips_executed();
    if (lastFlip) *lastFlip = latte::last_flip_time();
    if (lastVsync) *lastVsync = latte::last_vsync_time();
}
void GX2WaitForVsync() {
    if (g_skip_vsync_wait) {  // a game hook (60 fps) asked to skip this wait
        g_skip_vsync_wait = false;
        return;
    }
    threads::WaitMark w("vsync", 0);
    threads::CoreHandoff handoff;
    handoff.yield();
    latte::wait_for_vsync();
}
void GX2WaitForFlip() {
    if (latte::flip_requests() == latte::flips_executed()) return;
    threads::WaitMark w("flip", 0);
    threads::CoreHandoff handoff;
    handoff.yield();
    latte::wait_for_flip();
}
void GX2SetSwapInterval(uint32 v) {
    if (v < 20) latte::set_swap_interval(v);
}
uint32 GX2GetSwapInterval() { return latte::swap_interval(); }
void GX2SetEventCallback(uint32 type, MPTR func, MPTR user) {
    if (type < 5) {
        std::lock_guard<std::mutex> l(g_ev_lock);
        g_event_cb[type] = {func, user};
    }
}
void GX2GetEventCallback(uint32 type, uint32be* func, uint32be* user) {
    if (type >= 5) return;
    EventCallback cbk;
    {
        std::lock_guard<std::mutex> l(g_ev_lock);
        cbk = g_event_cb[type];
    }
    if (func) *func = cbk.func;
    if (user) *user = cbk.user;
}
void GX2SetGPUFence(uint32be* fence, uint32 mask, uint32 compareOp, uint32 value) {
    static const uint8 ops[] = {0x7, 0x1, 0x3, 0x2, 0x6, 0x4, 0x5, 0x0};
    GX2ReserveCmdSpace(7);
    gx2WriteGather_submit(pm4HeaderType3(IT_WAIT_REG_MEM, 6), (uint32)ops[compareOp & 7] | 0x10,
                          memory_getVirtualOffsetFromPointer(fence) | 2, 0u, value, mask, 0xAu);
}
void GX2SampleTopGPUCycle(uint64be* out) { *out = rt_timebase(); }
void GX2SampleBottomGPUCycle(uint64be* out) { *out = 0xFFFFFFFFFFFFFFFFull; }  // like Cemu: no dynamic resolution
uint64 GX2GPUTimeToCPUTime(uint64 t) { return 0; }
uint32 GX2TempGetGPUVersion() { return 2; }
void GX2SetTVBuffer(void* buf, uint32 size, uint32 mode, uint32 format, uint32 bufferMode) {
    LOG("GX2SetTVBuffer: mode %u format %x buffering %u", mode, format, bufferMode);
    latte::set_scanout_srgb(true, (format & 0x400) != 0);  // Latte::E_GX2SURFFMT::FMT_BIT_SRGB
}
void GX2SetTVEnable(uint32 enable) {}
void GX2SetTVScale(uint32 w, uint32 h) {}
void GX2SetDRCBuffer(void* buf, uint32 size, uint32 mode, uint32 format, uint32 bufferMode) {
    latte::set_scanout_srgb(false, (format & 0x400) != 0);
}
void GX2SetDRCEnable(uint32 enable) {}
void GX2SetDRCScale(uint32 w, uint32 h) {}
void GX2SetVerifyLevel(uint32 level) {}
}  // namespace GX2

void gx2WriteGather_submitU32AsLEArray(uint32* v, uint32 n) {
    auto& c = GX2::cb();
    if (!c.currentWritePtr) return;
    mem::commit(mem::guest(c.currentWritePtr), (size_t)n * 4, true);
    memcpy(c.currentWritePtr, v, n * 4);
    c.currentWritePtr += n;
}

CAFE_EXPORT(gx2, GX2Init, GX2::GX2Init);
CAFE_EXPORT(gx2, GX2Flush, GX2::GX2Flush);
CAFE_EXPORT(gx2, GX2DrawDone, GX2::GX2DrawDone);
CAFE_EXPORT(gx2, GX2GetLastSubmittedTimeStamp, GX2::GX2GetLastSubmittedTimeStamp);
CAFE_EXPORT(gx2, GX2GetRetiredTimeStamp, GX2::GX2GetRetiredTimeStamp);
CAFE_EXPORT(gx2, GX2WaitTimeStamp, GX2::GX2WaitTimeStamp);
CAFE_EXPORT(gx2, GX2BeginDisplayListEx, GX2::GX2BeginDisplayListEx);
CAFE_EXPORT(gx2, GX2BeginDisplayList, GX2::GX2BeginDisplayList);
CAFE_EXPORT(gx2, GX2EndDisplayList, GX2::GX2EndDisplayList);
CAFE_EXPORT(gx2, GX2GetCurrentDisplayList, GX2::GX2GetCurrentDisplayList);
CAFE_EXPORT(gx2, GX2GetDisplayListWriteStatus, GX2::GX2GetDisplayListWriteStatus);
CAFE_EXPORT(gx2, GX2CallDisplayList, GX2::GX2CallDisplayList);
CAFE_EXPORT(gx2, GX2DirectCallDisplayList, GX2::GX2DirectCallDisplayList);
CAFE_EXPORT(gx2, GX2CopyDisplayList, GX2::GX2CopyDisplayList);
CAFE_EXPORT(gx2, GX2PatchDisplayList, GX2::GX2PatchDisplayList);
CAFE_EXPORT(gx2, GX2SwapScanBuffers, GX2::GX2SwapScanBuffers);
CAFE_EXPORT(gx2, GX2CopyColorBufferToScanBuffer, GX2::GX2CopyColorBufferToScanBuffer);
CAFE_EXPORT(gx2, GX2GetSwapStatus, GX2::GX2GetSwapStatus);
CAFE_EXPORT(gx2, GX2WaitForVsync, GX2::GX2WaitForVsync);
CAFE_EXPORT(gx2, GX2WaitForFlip, GX2::GX2WaitForFlip);
CAFE_EXPORT(gx2, GX2SetSwapInterval, GX2::GX2SetSwapInterval);
CAFE_EXPORT(gx2, GX2GetSwapInterval, GX2::GX2GetSwapInterval);
CAFE_EXPORT(gx2, GX2SetEventCallback, GX2::GX2SetEventCallback);
CAFE_EXPORT(gx2, GX2GetEventCallback, GX2::GX2GetEventCallback);
CAFE_EXPORT(gx2, GX2SetGPUFence, GX2::GX2SetGPUFence);
CAFE_EXPORT(gx2, GX2SampleTopGPUCycle, GX2::GX2SampleTopGPUCycle);
CAFE_EXPORT(gx2, GX2SampleBottomGPUCycle, GX2::GX2SampleBottomGPUCycle);
CAFE_EXPORT(gx2, GX2GPUTimeToCPUTime, GX2::GX2GPUTimeToCPUTime);
CAFE_EXPORT(gx2, GX2TempGetGPUVersion, GX2::GX2TempGetGPUVersion);
CAFE_EXPORT(gx2, GX2SetTVBuffer, GX2::GX2SetTVBuffer);
CAFE_EXPORT(gx2, GX2SetTVEnable, GX2::GX2SetTVEnable);
CAFE_EXPORT(gx2, GX2SetTVScale, GX2::GX2SetTVScale);
CAFE_EXPORT(gx2, GX2SetDRCBuffer, GX2::GX2SetDRCBuffer);
CAFE_EXPORT(gx2, GX2SetDRCEnable, GX2::GX2SetDRCEnable);
CAFE_EXPORT(gx2, GX2SetDRCScale, GX2::GX2SetDRCScale);
CAFE_EXPORT(gx2, GX2SetVerifyLevel, GX2::GX2SetVerifyLevel);
