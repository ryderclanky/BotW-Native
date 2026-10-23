#include "diag.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "mem.h"
#include "thread.h"
#include "title_diag.h"

namespace {
std::atomic<int> g_diag_on{-1};
}

bool diag_on() {
    int v = g_diag_on.load(std::memory_order_relaxed);
    if (v >= 0) return v == 1;
    const char* e = getenv("BOTW_DIAG");
    v = (e && e[0] && e[0] != '0') ? 1 : 0;
    g_diag_on.store(v, std::memory_order_relaxed);
    return v == 1;
}

namespace {
constexpr int kFs = 256;
constexpr int kSync = 768;
constexpr int kSticky = 48;
constexpr int kLine = 192;

char g_fs[kFs][kLine];
char g_sync[kSync][kLine];
char g_sticky[kSticky][kLine];
std::atomic<uint32_t> g_fs_n{0}, g_sync_n{0}, g_sticky_n{0};

bool enabled() { return ::diag_on(); }

void format_line(char* dst, const char* fmt, va_list ap) {
    const char* tn = threads::current_name();
    int n = snprintf(dst, kLine, "[%.24s] ", tn && tn[0] ? tn : "?");
    if (n < 0 || n >= kLine - 1) {
        dst[kLine - 1] = 0;
        return;
    }
    vsnprintf(dst + n, (size_t)(kLine - n), fmt, ap);
}

bool sticky_path(const char* path) {
    if (!path) return false;
    return strstr(path, "Region") || strstr(path, "Lang") || strstr(path, ".txt") || strstr(path, "ystem");
}

bool focus_thread() {
    const char* n = threads::current_name();
    if (!n || !n[0]) return true;
    return strcmp(n, "main") == 0 || strcmp(n, "thread") == 0 || strncmp(n, "Sound", 5) == 0 ||
           strncmp(n, "Prepare", 7) == 0 || strncmp(n, "Default", 7) == 0;
}

void dump_ring(const char* tag, char ring[][kLine], uint32_t n, uint32_t cap) {
    if (!n) return;
    uint32_t count = n < cap ? n : cap;
    uint32_t first = n < cap ? 0 : n - cap;
    if (n > cap) LOG("diag: %s dropped %u older lines", tag, n - cap);
    for (uint32_t i = 0; i < count; i++) LOG("  %s %s", tag, ring[(first + i) % cap]);
}
}  // namespace

void diag_fs(const char* path, const char* fmt, ...) {
    if (!enabled()) return;
    char line[kLine];
    va_list ap;
    va_start(ap, fmt);
    format_line(line, fmt, ap);
    va_end(ap);
    uint32_t i = g_fs_n.fetch_add(1, std::memory_order_relaxed);
    memcpy(g_fs[i % kFs], line, kLine);
    if (sticky_path(path) || sticky_path(line)) {
        uint32_t s = g_sticky_n.fetch_add(1, std::memory_order_relaxed);
        if (s < (uint32_t)kSticky) memcpy(g_sticky[s], line, kLine);
    }
}

void diag_sync(const char* fmt, ...) {
    if (!enabled() || !focus_thread()) return;
    char line[kLine];
    va_list ap;
    va_start(ap, fmt);
    format_line(line, fmt, ap);
    va_end(ap);
    uint32_t i = g_sync_n.fetch_add(1, std::memory_order_relaxed);
    memcpy(g_sync[i % kSync], line, kLine);
}

void diag_crash_object() {
    constexpr uint32_t kObj = 0x26cd0fe8;
    if (!mem::readable(kObj, 0x40)) {
        LOG("diag: object %08x is not readable yet", kObj);
        return;
    }
    LOG("diag: object %08x +28=%08x (watch byte %08x)", kObj, mem::r32(kObj + 28), kObj + 28);
    for (uint32_t i = 0; i < 0x40; i += 16)
        LOG("  +%02x: %08x %08x %08x %08x", i, mem::r32(kObj + i), mem::r32(kObj + i + 4), mem::r32(kObj + i + 8),
            mem::r32(kObj + i + 12));
}

void diag_dump(const char* why) {
    static thread_local int dumping = 0;
    if (dumping) return;
    dumping = 1;
    bool fatal = why && strcmp(why, "fatal") == 0;
    if (!enabled()) {
        if (fatal) {
            diag_crash_object();
            LOG("diag: %s. Rerun with BOTW_DIAG=1 and without --trace or --trace-hle. "
                "Optional second run: BOTW_WATCH=26cd1004",
                why);
        }
        dumping = 0;
        return;
    }
    uint32_t st = g_sticky_n.load(std::memory_order_relaxed);
    LOG("diag: %s (fs %u, sync %u, sticky %u)", why, g_fs_n.load(std::memory_order_relaxed),
        g_sync_n.load(std::memory_order_relaxed), st);
    diag_crash_object();
    uint32_t sn = st < (uint32_t)kSticky ? st : (uint32_t)kSticky;
    for (uint32_t i = 0; i < sn; i++) LOG("  keep %s", g_sticky[i]);
    dump_ring("fs", g_fs, g_fs_n.load(std::memory_order_relaxed), kFs);
    dump_ring("sync", g_sync, g_sync_n.load(std::memory_order_relaxed), kSync);
    dumping = 0;
}

namespace {
struct RtSlot {
    uint32_t base = 0, w = 0, h = 0, draws = 0;
};
// Written and logged on the GPU thread only.
RtSlot g_rt[8];
int g_rt_n = 0;
uint32_t g_rt_extra = 0;
uint32_t g_skip_index = 0, g_skip_compute = 0;
uint32_t g_indirect = 0, g_indirect_words = 0;
uint32_t g_clears = 0, g_clear_w = 0, g_clear_h = 0;
uint32_t g_scans = 0, g_scan_w = 0, g_scan_h = 0, g_scan_fmt = 0, g_scan_addr = 0;
uint32_t g_scan_max_w = 0, g_scan_max_h = 0;
uint32_t g_prim_tri = 0, g_prim_quad = 0, g_prim_rect = 0, g_prim_other = 0;
uint32_t g_depth_on = 0, g_depth_off = 0, g_blend_on = 0;
uint32_t g_small_depth = 0, g_small_nodepth = 0, g_small_after = 0;
uint32_t g_after_cur = 0, g_after_total = 0;
int g_after_phase = 0;

constexpr int kKindMax = 18;
constexpr int kKindCap = 12;
uint32_t g_kind_count[kKindMax] = {};
int g_kind_logged[kKindMax] = {};
struct EvKey {
    uint32_t a, b, c, d, e, f;
};
EvKey g_ev[kKindMax][kKindCap] = {};

void log_cutscene_event(int kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    switch (kind) {
        case TITLE_DIAG_SCISSOR:
            LOG("[cutscene] scissor drop color %ux%u smaller than scissor %ux%u fmt %04x", a, b, c, d, e);
            break;
        case TITLE_DIAG_COPY_RATIO:
            LOG("[cutscene] copy skipped, scale ratio srcEff %ux%u fmt %04x dstEff %ux%u fmt %04x", a, b, e, c, d, f);
            break;
        case TITLE_DIAG_COPY_BPP:
            LOG("[cutscene] copy skipped, bpp mismatch srcEff %ux%u fmt %04x dstEff %ux%u fmt %04x", a, b, e, c, d, f);
            break;
        case TITLE_DIAG_PIPE_FAIL:
            LOG("[cutscene] pipeline fail vk %d prim %02x blendC %02x/%02x blendA %02x/%02x", (int)a, b, c, d, e, f);
            break;
        case TITLE_DIAG_SKIP_SHADER:
            LOG("[cutscene] skip shader error prim %02x", a);
            break;
        case TITLE_DIAG_SKIP_RT:
            LOG("[cutscene] skip invalid rendertarget prim %02x", a);
            break;
        case TITLE_DIAG_SKIP_ATTACH:
            LOG("[cutscene] skip no color or depth attachment prim %02x", a);
            break;
        case TITLE_DIAG_SKIP_RAST:
            LOG("[cutscene] skip rasterizer kill prim %02x", a);
            break;
        case TITLE_DIAG_SKIP_PIPE:
            LOG("[cutscene] skip null pipeline prim %02x", a);
            break;
        case TITLE_DIAG_TEX_DIM:
            LOG("[cutscene] null texture shader %08x%08x expected dim %u bound dim %u fmt %04x", a, b, c, d, e);
            break;
        case TITLE_DIAG_RES_CLEAR:
            LOG("[cutscene] resolution overwrite cleared fmt %04x %ux%u tile %u -> %ux%u", a, b, c, d, e, f);
            break;
        case TITLE_DIAG_RES_CLEAR_FONT:
            LOG("[cutscene] resolution overwrite cleared font/BC fmt %04x %ux%u tile %u -> %ux%u", a, b, c, d, e, f);
            break;
        case TITLE_DIAG_BLEND_BOTH:
            LOG("[cutscene] blend factor 0x%02x on %s emulated (0x0B src=SRC_ALPHA dest=ONE_MINUS_SRC_ALPHA, 0x0C swapped)",
                a, b ? "src" : "dst");
            break;
        case TITLE_DIAG_BLEND_BAD:
            LOG("[cutscene] blend factor 0x%02x is outside the GX2 table", a);
            break;
        case TITLE_DIAG_DEPTH_OFF:
            LOG("[cutscene] depth test forced off, no depth attachment, prim %02x z %u stencil %u", a, b, c);
            break;
        case TITLE_DIAG_RECT:
            LOG("[cutscene] rect primitive pipeline, nv_fill_rectangle %u", b);
            break;
        case TITLE_DIAG_VIEWPORT:
            LOG("[cutscene] viewport depth outside 0..1 xy %u,%u size %ux%u near %08x far %08x", a, b, c, d, e, f);
            break;
        default:
            LOG("[cutscene] event %d %u %u %u %u %u %u", kind, a, b, c, d, e, f);
            break;
    }
}

std::atomic<uint32_t> g_ax_playing{0}, g_ax_ppc{0}, g_ax_dsp{0}, g_ax_mask{0};
std::atomic<uint32_t> g_ax_mix{0}, g_ax_lr{0}, g_ax_other{0}, g_ax_flags{0}, g_ax_frames{0};
std::atomic<uint32_t> g_vpad_reads{0}, g_vpad_last{0}, g_vpad_buttons{0}, g_vpad_cb{0}, g_vpad_invoked{0};

void atomic_max(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void decode_color_size(uint32_t color_size, uint32_t& w, uint32_t& h) {
    // Same fields LatteMRT::GetColorAttachmentTexture uses.
    w = (((color_size >> 0) & 0x3FF) + 1) << 3;
    uint32_t pitch_height = (((color_size >> 10) & 0xFFFFF) + 1) << 6;
    h = w ? pitch_height / w : 0;
}

void note_max_area(uint32_t& mw, uint32_t& mh, uint32_t w, uint32_t h) {
    if ((uint64_t)w * h >= (uint64_t)mw * mh) {
        mw = w;
        mh = h;
    }
}
}  // namespace

extern "C" void title_diag_draw(uint32_t color_base, uint32_t color_size) {
    if (!diag_on()) return;
    if (g_after_phase) g_after_cur++;
    uint32_t base = color_base & 0xFFFFFF00u;
    uint32_t w = 0, h = 0;
    decode_color_size(color_size, w, h);
    for (int i = 0; i < g_rt_n; i++) {
        if (g_rt[i].base != base) continue;
        g_rt[i].draws++;
        note_max_area(g_rt[i].w, g_rt[i].h, w, h);
        return;
    }
    if (g_rt_n >= 8) {
        g_rt_extra++;
        return;
    }
    g_rt[g_rt_n++] = {base, w, h, 1};
}

extern "C" void title_diag_draw_state(uint32_t prim, int depth_on, uint32_t blend_mask, uint32_t count) {
    if (!diag_on()) return;
    if (prim == 0x4) g_prim_tri++;
    else if (prim == 0x13) g_prim_quad++;
    else if (prim == 0x11) g_prim_rect++;
    else g_prim_other++;
    if (depth_on) g_depth_on++;
    else g_depth_off++;
    if (blend_mask) g_blend_on++;
    if (count <= 6) {
        if (depth_on) g_small_depth++;
        else g_small_nodepth++;
        if (g_after_phase) g_small_after++;
    }
}

extern "C" void title_diag_draw_skip(int why) {
    if (!diag_on()) return;
    if (why == 1) g_skip_index++;
    else if (why == 2) g_skip_compute++;
}

extern "C" void title_diag_event(int kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    if (kind <= 0 || kind >= kKindMax) return;
    if (diag_on()) g_kind_count[kind]++;
    const int n = g_kind_logged[kind];
    const int stored = n > kKindCap ? kKindCap : n;
    for (int i = 0; i < stored; i++) {
        const EvKey& k = g_ev[kind][i];
        if (k.a == a && k.b == b && k.c == c && k.d == d && k.e == e && k.f == f) return;
    }
    if (n >= kKindCap) {
        if (n == kKindCap) {
            g_kind_logged[kind] = kKindCap + 1;
            LOG("[cutscene] kind %d further unique lines suppressed (BOTW_DIAG=1 still counts them)", kind);
        }
        return;
    }
    g_ev[kind][g_kind_logged[kind]++] = {a, b, c, d, e, f};
    log_cutscene_event(kind, a, b, c, d, e, f);
}

extern "C" void title_diag_swap(void) {
    if (!diag_on()) return;
    g_after_total += g_after_cur;
    g_after_cur = 0;
    g_after_phase = 0;
}

extern "C" void title_diag_indirect(uint32_t dwords) {
    if (!diag_on()) return;
    g_indirect++;
    g_indirect_words += dwords;
}

extern "C" void title_diag_scanout(uint32_t addr, uint32_t width, uint32_t height, uint32_t format) {
    if (!diag_on()) return;
    g_scans++;
    g_scan_w = width;
    g_scan_h = height;
    g_scan_fmt = format;
    g_scan_addr = addr;
    note_max_area(g_scan_max_w, g_scan_max_h, width, height);
    g_after_total += g_after_cur;
    g_after_cur = 0;
    g_after_phase = 1;
}

extern "C" void title_diag_clear(uint32_t mask, uint32_t width, uint32_t height) {
    if (!diag_on()) return;
    (void)mask;
    g_clears++;
    note_max_area(g_clear_w, g_clear_h, width, height);
}

extern "C" void title_diag_ax(uint32_t playing, uint32_t linked_ppc, uint32_t linked_dsp, uint32_t mask_tv,
                              uint32_t peak_mix, uint32_t peak_lr, uint32_t peak_other, uint32_t flags) {
    if (!diag_on()) return;
    atomic_max(g_ax_playing, playing);
    atomic_max(g_ax_ppc, linked_ppc);
    atomic_max(g_ax_dsp, linked_dsp);
    atomic_max(g_ax_mask, mask_tv);
    atomic_max(g_ax_mix, peak_mix);
    atomic_max(g_ax_lr, peak_lr);
    atomic_max(g_ax_other, peak_other);
    g_ax_flags.store(flags, std::memory_order_relaxed);
    g_ax_frames.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void title_diag_vpad_read(uint32_t samples, uint32_t buttons) {
    if (!diag_on()) return;
    g_vpad_reads.fetch_add(1, std::memory_order_relaxed);
    g_vpad_last.store(samples, std::memory_order_relaxed);
    g_vpad_buttons.store(buttons, std::memory_order_relaxed);
}

extern "C" void title_diag_vpad_callback(uint32_t addr) {
    if (!diag_on()) return;
    g_vpad_cb.store(addr, std::memory_order_relaxed);
}

extern "C" void title_diag_vpad_invoked() {
    if (!diag_on()) return;
    g_vpad_invoked.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void title_diag_log() {
    if (!diag_on()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::time_point{};
    auto now = clock::now();
    if (next == clock::time_point{}) {
        next = now + std::chrono::seconds(10);
        return;  // keep the counters; the first line is a full interval
    }
    if (now < next) return;
    next = now + std::chrono::seconds(10);

    uint32_t draws = 0;
    char rts[320];
    size_t used = 0;
    rts[0] = 0;
    for (int i = 0; i < g_rt_n; i++) {
        draws += g_rt[i].draws;
        int n = snprintf(rts + used, sizeof(rts) - used, "%s%08x %ux%u x%u", used ? ", " : "", g_rt[i].base, g_rt[i].w,
                         g_rt[i].h, g_rt[i].draws);
        if (n < 0 || (size_t)n >= sizeof(rts) - used) {
            rts[sizeof(rts) - 1] = 0;
            break;
        }
        used += (size_t)n;
    }
    if (!g_rt_n) snprintf(rts, sizeof(rts), "none");
    char extra[16] = "";
    if (g_rt_extra) snprintf(extra, sizeof(extra), " +%u", g_rt_extra);
    LOG("[title] draws %u | rt %s%s | skip index %u compute %u | indirect %u words %u | clear %u max %ux%u | "
        "scan %u last %ux%u fmt %x @%08x max %ux%u",
        draws, rts, extra, g_skip_index, g_skip_compute, g_indirect, g_indirect_words, g_clears, g_clear_w, g_clear_h,
        g_scans, g_scan_w, g_scan_h, g_scan_fmt, g_scan_addr, g_scan_max_w, g_scan_max_h);
    g_rt_n = 0;
    g_rt_extra = g_skip_index = g_skip_compute = 0;
    g_indirect = g_indirect_words = 0;
    g_clears = g_clear_w = g_clear_h = 0;
    g_scans = g_scan_w = g_scan_h = g_scan_fmt = g_scan_addr = g_scan_max_w = g_scan_max_h = 0;
    for (auto& s : g_rt) s = {};

    LOG("[title] prim tri %u quad %u rect %u other %u | depth on %u off %u blend %u | small depth %u nodepth %u "
        "afterScan %u smallAfter %u | scissor %u copy %u/%u pipe %u skip sh %u rt %u att %u rast %u null %u | "
        "texDim %u resClear %u fontClear %u blendBoth %u depthOff %u viewport %u rectPipe %u",
        g_prim_tri, g_prim_quad, g_prim_rect, g_prim_other, g_depth_on, g_depth_off, g_blend_on, g_small_depth,
        g_small_nodepth, g_after_total + g_after_cur, g_small_after, g_kind_count[TITLE_DIAG_SCISSOR],
        g_kind_count[TITLE_DIAG_COPY_RATIO], g_kind_count[TITLE_DIAG_COPY_BPP], g_kind_count[TITLE_DIAG_PIPE_FAIL],
        g_kind_count[TITLE_DIAG_SKIP_SHADER], g_kind_count[TITLE_DIAG_SKIP_RT], g_kind_count[TITLE_DIAG_SKIP_ATTACH],
        g_kind_count[TITLE_DIAG_SKIP_RAST], g_kind_count[TITLE_DIAG_SKIP_PIPE], g_kind_count[TITLE_DIAG_TEX_DIM],
        g_kind_count[TITLE_DIAG_RES_CLEAR], g_kind_count[TITLE_DIAG_RES_CLEAR_FONT],
        g_kind_count[TITLE_DIAG_BLEND_BOTH], g_kind_count[TITLE_DIAG_DEPTH_OFF], g_kind_count[TITLE_DIAG_VIEWPORT],
        g_kind_count[TITLE_DIAG_RECT]);
    g_prim_tri = g_prim_quad = g_prim_rect = g_prim_other = 0;
    g_depth_on = g_depth_off = g_blend_on = 0;
    g_small_depth = g_small_nodepth = g_small_after = 0;
    g_after_cur = g_after_total = 0;
    for (int i = 0; i < kKindMax; i++) g_kind_count[i] = 0;

    uint32_t frames = g_ax_frames.exchange(0, std::memory_order_relaxed);
    uint32_t flags = g_ax_flags.load(std::memory_order_relaxed);
    LOG("[title] ax playing %u linked %u/%u maskTV %u mix %u outLR %u outOther %u | cb mix %u frame %u app %u | "
        "frames %u",
        g_ax_playing.exchange(0, std::memory_order_relaxed), g_ax_ppc.exchange(0, std::memory_order_relaxed),
        g_ax_dsp.exchange(0, std::memory_order_relaxed), g_ax_mask.exchange(0, std::memory_order_relaxed),
        g_ax_mix.exchange(0, std::memory_order_relaxed), g_ax_lr.exchange(0, std::memory_order_relaxed),
        g_ax_other.exchange(0, std::memory_order_relaxed), (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0,
        (flags & 4) ? 1 : 0, frames);
    LOG("[title] vpad reads %u last %u buttons %08x cb %08x invoked %u",
        g_vpad_reads.exchange(0, std::memory_order_relaxed), g_vpad_last.load(std::memory_order_relaxed),
        g_vpad_buttons.load(std::memory_order_relaxed), g_vpad_cb.load(std::memory_order_relaxed),
        g_vpad_invoked.exchange(0, std::memory_order_relaxed));
}
