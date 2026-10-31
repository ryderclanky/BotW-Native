// PM4 statistics tap (--gpu-stats N): measures how the game drives the GPU, for the native
// renderer design (docs/native-renderer.md). Every submission is walked on the submitting thread,
// following INDIRECT_BUFFER_PRIV into display lists, with a private register mirror; nothing is
// written to guest memory. A one-line summary goes to the log every N frames and the full report
// is rewritten to <user root>/gpu_stats.txt.
#if defined(BOTW_NATIVE_GPU)
#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../log.h"
#include "../mem.h"
#include "../os.h"
#include "../utf8_path.h"
#include "gpu_native.h"
#include "pm4.h"

namespace gpu_native {
namespace pm4 {
const char* op_name(uint32_t op) {
    switch (op) {
        case NOP: return "NOP";
        case SET_PREDICATION: return "SET_PREDICATION";
        case DRAW_INDEX_2: return "DRAW_INDEX_2";
        case CONTEXT_CONTROL: return "CONTEXT_CONTROL";
        case INDEX_TYPE: return "INDEX_TYPE";
        case DRAW_INDEX_AUTO: return "DRAW_INDEX_AUTO";
        case DRAW_INDEX_IMMD: return "DRAW_INDEX_IMMD";
        case NUM_INSTANCES: return "NUM_INSTANCES";
        case INDIRECT_BUFFER_PRIV: return "INDIRECT_BUFFER_PRIV";
        case STRMOUT_BUFFER_UPDATE: return "STRMOUT_BUFFER_UPDATE";
        case MEM_SEMAPHORE: return "MEM_SEMAPHORE";
        case WAIT_REG_MEM: return "WAIT_REG_MEM";
        case MEM_WRITE: return "MEM_WRITE";
        case SURFACE_SYNC: return "SURFACE_SYNC";
        case EVENT_WRITE: return "EVENT_WRITE";
        case EVENT_WRITE_EOP: return "EVENT_WRITE_EOP";
        case LOAD_CONFIG_REG: return "LOAD_CONFIG_REG";
        case LOAD_CONTEXT_REG: return "LOAD_CONTEXT_REG";
        case LOAD_ALU_CONST: return "LOAD_ALU_CONST";
        case LOAD_BOOL_CONST: return "LOAD_BOOL_CONST";
        case LOAD_LOOP_CONST: return "LOAD_LOOP_CONST";
        case LOAD_RESOURCE: return "LOAD_RESOURCE";
        case LOAD_SAMPLER: return "LOAD_SAMPLER";
        case SET_CONFIG_REG: return "SET_CONFIG_REG";
        case SET_CONTEXT_REG: return "SET_CONTEXT_REG";
        case SET_ALU_CONST: return "SET_ALU_CONST";
        case SET_BOOL_CONST: return "SET_BOOL_CONST";
        case SET_LOOP_CONST: return "SET_LOOP_CONST";
        case SET_RESOURCE: return "SET_RESOURCE";
        case SET_SAMPLER: return "SET_SAMPLER";
        case SET_CTL_CONST: return "SET_CTL_CONST";
        case STRMOUT_BASE_UPDATE: return "STRMOUT_BASE_UPDATE";
        case SET_ALL_CONTEXTS: return "SET_ALL_CONTEXTS";
        case HLE_COPY_SURFACE_NEW: return "HLE_COPY_SURFACE_NEW";
        case HLE_SYNC_ASYNC_OPERATIONS: return "HLE_SYNC_ASYNC_OPERATIONS";
        case HLE_REQUEST_SWAP_BUFFERS: return "HLE_REQUEST_SWAP_BUFFERS";
        case HLE_WAIT_FOR_FLIP: return "HLE_WAIT_FOR_FLIP";
        case HLE_BOTTOM_OF_PIPE_CB: return "HLE_BOTTOM_OF_PIPE_CB";
        case HLE_COPY_COLORBUFFER_TO_SCANBUFFER: return "HLE_COPY_COLORBUFFER_TO_SCANBUFFER";
        case HLE_CLEAR_COLOR_DEPTH_STENCIL: return "HLE_CLEAR_COLOR_DEPTH_STENCIL";
        case HLE_SAMPLE_TIMER: return "HLE_SAMPLE_TIMER";
        case HLE_TRIGGER_SCANBUFFER_SWAP: return "HLE_TRIGGER_SCANBUFFER_SWAP";
        case HLE_SPECIAL_STATE: return "HLE_SPECIAL_STATE";
        case HLE_BEGIN_OCCLUSION_QUERY: return "HLE_BEGIN_OCCLUSION_QUERY";
        case HLE_END_OCCLUSION_QUERY: return "HLE_END_OCCLUSION_QUERY";
        default: return nullptr;
    }
}
}  // namespace pm4

namespace {
using namespace pm4;

uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull + (h >> 29); }

std::string fmt_s(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt_s(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
int64_t dump_frame() { return gpu_native::dump_frame_index(); }

template <typename K>
using Hist = std::map<K, uint64_t>;

struct DlInfo {
    uint64_t calls = 0, draws = 0;
    uint32_t dwords = 0;
    std::unordered_set<uint64_t> hashes;  // distinct contents seen
    uint64_t last_frame = 0, frames = 0;
    bool direct = false;  // submitted with GX2DirectCallDisplayList (else called via INDIRECT_BUFFER)
    std::vector<uint32_t> prev;  // previous contents (direct DLs), to see what changes between calls
    uint64_t changed_calls = 0, changed_dwords = 0, same_layout = 0;
};

struct Frame {  // per-frame counters
    uint64_t draws_top = 0, draws_dl = 0, draws_direct = 0, ib_calls = 0, dl_dwords = 0, top_dwords = 0, indices = 0;
    uint64_t draws_instanced = 0, draws_gs = 0, draws_so = 0, draws_pred = 0, draws_msaa = 0, draws_compute = 0;
    uint64_t rt_changes = 0, pipelines = 0;
    std::unordered_set<uint64_t> pso;
};

struct Stats {
    std::mutex lock;
    uint32_t regs[kRegCount] = {};
    uint64_t frame = 0, frames_seen = 0;
    Frame cur, total;
    uint64_t frames_with_draws = 0;
    uint64_t max_draws = 0;
    bool predication = false;
    uint32_t instances = 1;
    uint64_t last_rt_sig = 0;
    // run totals
    Hist<std::string> packets_top, packets_dl;
    Hist<uint32_t> prim_types, index_types, color_fmt, color_tile, depth_fmt, depth_tile, msaa, events,
        surface_sync, special_state, ctx_control, tex_dim, tex_fmt, tex_tile, tex_comp, unknown_ops;
    Hist<std::string> copy_surface, clears, scanout, dl_changes;
    std::map<uint32_t, DlInfo> dls;  // by address
    std::unordered_map<uint64_t, uint64_t> program_hash;  // (stage<<56 | addr<<24 | size) -> content hash (this frame)
    std::unordered_set<uint64_t> programs[5];             // FS VS PS GS ES content hashes
    std::unordered_set<uint64_t> pso_all, rt_sigs;
    std::unordered_set<uint32_t> textures, tex_cube, tex_array, tex_3d, tex_msaa;  // base addresses
    uint64_t ubo_sets[3] = {}, tex_sets[3] = {}, attrib_sets = 0, alu_const_dwords = 0, sampler_sets = 0;
    uint64_t submissions = 0, submit_dwords = 0, max_ib_depth = 0, bad_packets = 0, type0_packets = 0;
    uint64_t direct_dl_submits = 0;  // submissions that are game display lists (GX2DirectCallDisplayList)
    bool cur_direct = false;         // walking such a submission
    int logged_changes = 0;
    std::vector<std::string> dump;  // --gpu-dump-frame N: per-operation log of that frame
    uint32_t pool_lo = 0xFFFFFFFF, pool_hi = 0;
    uint64_t occlusion_queries = 0, so_updates = 0, mem_writes = 0, waits = 0, semaphores = 0, timers = 0, eop = 0;
};
Stats* g_stats;

const char* prim_name(uint32_t p) {
    switch (p) {
        case 1: return "POINTS"; case 2: return "LINES"; case 3: return "LINE_STRIP"; case 4: return "TRIANGLES";
        case 5: return "TRIANGLE_FAN"; case 6: return "TRIANGLE_STRIP"; case 0xA: return "LINES_ADJ";
        case 0xC: return "TRIANGLES_ADJ"; case 0x11: return "RECTS"; case 0x12: return "LINE_LOOP";
        case 0x13: return "QUADS"; case 0x14: return "QUAD_STRIP"; default: return "?";
    }
}

uint64_t program(Stats& s, int stage, uint32_t start_reg, uint32_t size_reg) {
    uint32_t addr = s.regs[start_reg] << 8, size = s.regs[size_reg] << 3;
    if (!addr || !size || size > 0x100000) return 0;
    uint64_t key = ((uint64_t)stage << 56) | ((uint64_t)(addr >> 8) << 24) | (size >> 3);
    auto it = s.program_hash.find(key);
    if (it != s.program_hash.end()) return it->second;
    uint64_t h = fnv(mem::host(addr), size);
    s.program_hash[key] = h;
    s.programs[stage].insert(h);
    return h;
}

bool dumping(const Stats& s) { return dump_frame() >= 0 && (int64_t)s.frame == dump_frame(); }

void dump_draw(Stats& s, uint32_t count, uint32_t kind, uint32_t index_addr) {
    const uint32_t* r = s.regs;
    std::string l = fmt_s("draw %s prim 0x%x count %u idx %08x type %u inst %u%s | ", kind == DRAW_INDEX_2 ? "IDX2" : "AUTO",
                          r[VGT_PRIMITIVE_TYPE], count, index_addr, r[VGT_DMA_INDEX_TYPE], s.instances,
                          s.predication ? " PRED" : "");
    uint32_t mask = r[CB_TARGET_MASK];
    for (int i = 0; i < 8; i++) {
        if (!((mask >> (i * 4)) & 0xF)) continue;
        uint32_t base = r[CB_COLOR0_BASE + i] & ~0xFFu, info = r[CB_COLOR0_INFO + i], size = r[CB_COLOR0_SIZE + i];
        if (!base) continue;
        uint32_t pitch = ((size & 0x3FF) + 1) * 8, h = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
        l += fmt_s("RT%d %08x fmt %02x/%u tile %u %ux%u mask %x; ", i, base, (info >> 2) & 0x3F, (info >> 12) & 7,
                   (info >> 8) & 0xF, pitch, h, (mask >> (i * 4)) & 0xF);
    }
    uint32_t dctl = r[DB_DEPTH_CONTROL];
    if (dctl & 6)
        l += fmt_s("Z %08x fmt %u test %u write %u func %u; ", r[DB_DEPTH_INFO + 1] << 8, r[DB_DEPTH_INFO] & 7, (dctl >> 1) & 1,
                   (dctl >> 2) & 1, (dctl >> 4) & 7);
    l += fmt_s("blend0 %08x colorctl %08x | VS %08x+%x PS %08x+%x FS %08x+%x | ", r[CB_BLEND0_CONTROL], r[CB_COLOR_CONTROL],
               r[SQ_PGM_START_VS] << 8, r[SQ_PGM_SIZE_VS] << 3, r[SQ_PGM_START_PS] << 8, r[SQ_PGM_SIZE_PS] << 3,
               r[SQ_PGM_START_FS] << 8, r[SQ_PGM_SIZE_FS] << 3);
    for (uint32_t t = 0; t < kTexPerStage; t++) {
        const uint32_t* w = r + kResourceBase + kResPsTex + t * 7;
        if (!w[2]) continue;
        uint32_t width = ((w[0] >> 19) & 0x1FFF) + 1, height = (w[1] & 0x1FFF) + 1;
        l += fmt_s("t%u %08x %ux%u dim %u fmt %02x tile %u; ", t, w[2] << 8, width, height, w[0] & 7, (w[1] >> 26) & 0x3F,
                   (w[0] >> 3) & 0xF);
    }
    s.dump.push_back(l);
}

void on_draw(Stats& s, uint32_t count, bool in_dl, uint32_t kind, uint32_t index_addr = 0) {
    if (dumping(s)) dump_draw(s, count, kind, index_addr);
    Frame& f = s.cur;
    (in_dl ? f.draws_dl : f.draws_top)++;
    if (s.cur_direct) f.draws_direct++;
    f.indices += count;
    const uint32_t* r = s.regs;
    // compute dispatch (GX2SetShaderModeEx compute: SQ_CONFIG priorities, as Cemu detects it)
    uint32_t sqc = r[SQ_CONFIG];
    if (kind == DRAW_INDEX_AUTO && ((sqc >> 24) & 3) == 0 && ((sqc >> 26) & 3) == 1 && ((sqc >> 28) & 3) == 2 &&
        ((sqc >> 30) & 3) == 3)
        f.draws_compute++;
    bool gs = (r[VGT_GS_MODE] & 3) != 0;
    if (gs) f.draws_gs++;
    if (r[VGT_STRMOUT_EN] & 1) f.draws_so++;
    if (s.predication) f.draws_pred++;
    uint32_t aa = r[PA_SC_AA_CONFIG] & 3;
    s.msaa[aa]++;
    if (aa) f.draws_msaa++;
    s.prim_types[r[VGT_PRIMITIVE_TYPE]]++;
    // render target signature
    uint64_t rt = 0;
    uint32_t mask = r[CB_TARGET_MASK];
    for (int i = 0; i < 8; i++) {
        if (!((mask >> (i * 4)) & 0xF)) continue;
        uint32_t base = r[CB_COLOR0_BASE + i] & ~0xFFu, info = r[CB_COLOR0_INFO + i];
        if (!base) continue;
        uint32_t fmt = (info >> 2) & 0x3F, num = (info >> 12) & 7, tile = (info >> 8) & 0xF;
        if (!fmt) continue;
        s.color_fmt[fmt | num << 8]++;
        s.color_tile[tile]++;
        rt = mix(rt, (uint64_t)base << 32 | info);
        rt = mix(rt, r[CB_COLOR0_SIZE + i]);
    }
    // GX2SetDepthBuffer (Cemu layout) leaves DB_DEPTH_BASE 0 and puts the address in DB_HTILE_DATA_BASE
    uint32_t dbase = r[DB_DEPTH_INFO + 1] << 8, dinfo = r[DB_DEPTH_INFO], dctl = r[DB_DEPTH_CONTROL];
    if (dbase && (dctl & 6)) {  // Z_ENABLE | Z_WRITE_ENABLE
        s.depth_fmt[dinfo & 7]++;
        s.depth_tile[(dinfo >> 15) & 0xF]++;
        rt = mix(rt, (uint64_t)dbase << 32 | dinfo);
    }
    rt = mix(rt, aa);
    if (rt != s.last_rt_sig) f.rt_changes++;
    s.last_rt_sig = rt;
    s.rt_sigs.insert(rt);
    // pipeline-state signature: programs + output formats + fixed-function state that Metal bakes
    uint64_t fs = program(s, 0, SQ_PGM_START_FS, SQ_PGM_SIZE_FS);
    uint64_t vs = gs ? program(s, 4, SQ_PGM_START_ES, SQ_PGM_SIZE_ES) : program(s, 1, SQ_PGM_START_VS, SQ_PGM_SIZE_VS);
    uint64_t ps = program(s, 2, SQ_PGM_START_PS, SQ_PGM_SIZE_PS);
    uint64_t gsp = gs ? program(s, 3, SQ_PGM_START_GS, SQ_PGM_SIZE_GS) : 0;
    uint64_t pso = mix(mix(mix(mix(0, fs), vs), ps), gsp);
    for (int i = 0; i < 8; i++) pso = mix(pso, r[CB_COLOR0_INFO + i] & 0x3F3C);  // format + number type
    pso = mix(pso, dinfo & 7);
    pso = mix(pso, mask);
    pso = mix(pso, r[CB_COLOR_CONTROL]);
    for (int i = 0; i < 8; i++) pso = mix(pso, r[CB_BLEND0_CONTROL + i]);
    pso = mix(pso, aa);
    pso = mix(pso, r[VGT_PRIMITIVE_TYPE] == 0x11 ? 1 : 0);  // RECTS need a different vertex path
    f.pso.insert(pso);
    s.pso_all.insert(pso);
}

void on_resource(Stats& s, uint32_t off, uint32_t n, const uint32_t* w) {
    // w: big-endian words already converted; one SET_RESOURCE can set several 7-dword slots
    for (uint32_t k = 0; k + 7 <= n; k += 7, off += 7) {
        const uint32_t* d = w + k;
        int stage = -1;
        bool tex = false, ubo = false;
        if (off < kResPsTex + kTexPerStage * 7) stage = 0, tex = true;
        else if (off >= kResPsUbo && off < kResPsUbo + kUboPerStage * 7) stage = 0, ubo = true;
        else if (off >= kResVsTex && off < kResVsTex + kTexPerStage * 7) stage = 1, tex = true;
        else if (off >= kResVsUbo && off < kResVsUbo + kUboPerStage * 7) stage = 1, ubo = true;
        else if (off >= kResAttrib && off < kResAttrib + kAttribBuffers * 7) s.attrib_sets++;
        else if (off >= kResGsTex && off < kResGsTex + kTexPerStage * 7) stage = 2, tex = true;
        else if (off >= kResGsUbo && off < kResGsUbo + kUboPerStage * 7) stage = 2, ubo = true;
        if (ubo) s.ubo_sets[stage]++;
        if (!tex) continue;
        s.tex_sets[stage]++;
        uint32_t base = d[2] << 8;
        uint32_t dim = d[0] & 7, tile = (d[0] >> 3) & 0xF, fmt = (d[1] >> 26) & 0x3F;
        uint32_t comp = d[4] & 0x3FF;  // FORMAT_COMP_XYZW + NUM_FORMAT_ALL
        bool fresh = s.textures.insert(base).second;
        if (!fresh) continue;  // histograms count distinct textures (by base address)
        s.tex_dim[dim]++;
        s.tex_fmt[fmt | ((d[4] >> 8) & 3) << 8 | ((d[4] >> 11) & 1) << 12]++;  // fmt | numfmt | degamma
        s.tex_tile[tile]++;
        s.tex_comp[comp]++;
        if (dim == 3) s.tex_cube.insert(base);
        if (dim == 4 || dim == 5 || dim == 7) s.tex_array.insert(base);
        if (dim == 2) s.tex_3d.insert(base);
        if (dim >= 6) s.tex_msaa.insert(base);
    }
}

std::string fmt_surface(const uint32_t* p) {
    // HLE_COPY_SURFACE_NEW: addr swizzle format pitch height slice dim tilemode aa
    char b[96];
    snprintf(b, sizeof b, "fmt %03x dim %u tile %u aa %u", p[2], p[6], p[7], p[8]);
    return b;
}

void walk(Stats& s, uint32_t addr, uint32_t ndw, int depth) {
    if (depth > 8 || !addr || ndw > 0x1000000) return;
    s.max_ib_depth = std::max<uint64_t>(s.max_ib_depth, depth);
    const bool in_dl = depth > 0 || s.cur_direct;
    (in_dl ? s.cur.dl_dwords : s.cur.top_dwords) += ndw;
    auto rd = [&](uint32_t i) { return mem::r32(addr + i * 4); };
    uint32_t i = 0;
    uint32_t body[64];
    while (i < ndw) {
        uint32_t h = rd(i);
        uint32_t type = hdr_type(h);
        if (type == 2) {  // filler
            i++;
            continue;
        }
        if (type == 0) {  // register write packet (not emitted by GX2)
            uint32_t n = hdr_count(h), reg = h & 0xFFFF;
            for (uint32_t k = 0; k < n && reg + k < kRegCount; k++) s.regs[reg + k] = rd(i + 1 + k);
            s.type0_packets++;
            i += 1 + n;
            continue;
        }
        if (type != 3) {
            s.bad_packets++;
            return;
        }
        uint32_t op = hdr_op(h), n = hdr_count(h);
        if (i + 1 + n > ndw) {
            s.bad_packets++;
            return;
        }
        const char* name = op_name(op);
        if (!name) {
            s.unknown_ops[op]++;
            s.bad_packets++;
            i += 1 + n;
            continue;
        }
        (in_dl ? s.packets_dl : s.packets_top)[name]++;
        uint32_t nb = std::min<uint32_t>(n, 64);
        for (uint32_t k = 0; k < nb; k++) body[k] = rd(i + 1 + k);
        switch (op) {
            case SET_CONFIG_REG: case SET_CONTEXT_REG: case SET_ALL_CONTEXTS: case SET_ALU_CONST: case SET_BOOL_CONST:
            case SET_LOOP_CONST: case SET_RESOURCE: case SET_SAMPLER: case SET_CTL_CONST: {
                uint32_t reg = base_for(op) + body[0];
                for (uint32_t k = 1; k < n && reg + k - 1 < kRegCount; k++) s.regs[reg + k - 1] = rd(i + 1 + k);
                if (op == SET_RESOURCE) {
                    std::vector<uint32_t> w(n - 1);
                    for (uint32_t k = 1; k < n; k++) w[k - 1] = rd(i + 1 + k);
                    on_resource(s, body[0], n - 1, w.data());
                } else if (op == SET_ALU_CONST) s.alu_const_dwords += n - 1;
                else if (op == SET_SAMPLER) s.sampler_sets++;
                break;
            }
            case LOAD_CONFIG_REG: case LOAD_CONTEXT_REG: case LOAD_ALU_CONST: case LOAD_LOOP_CONST: case LOAD_RESOURCE:
            case LOAD_SAMPLER: {
                uint32_t shadow = body[0], rb = base_for(op);
                for (uint32_t k = 2; k + 1 < n; k += 2) {
                    uint32_t off = rd(i + 1 + k), cnt = rd(i + 2 + k);
                    for (uint32_t j = 0; j < cnt && rb + off + j < kRegCount; j++)
                        s.regs[rb + off + j] = mem::r32(shadow + (off + j) * 4);
                }
                break;
            }
            case CONTEXT_CONTROL: s.ctx_control[body[0]]++; break;
            case INDEX_TYPE: s.regs[VGT_DMA_INDEX_TYPE] = body[0]; s.index_types[body[0]]++; break;
            case NUM_INSTANCES:
                s.instances = body[0];
                if (body[0] > 1) s.cur.draws_instanced++;
                break;
            case DRAW_INDEX_2: on_draw(s, body[3], in_dl, op, body[1]); break;
            case DRAW_INDEX_AUTO: on_draw(s, body[0], in_dl, op); break;
            case DRAW_INDEX_IMMD: on_draw(s, body[0], in_dl, op); break;
            case SET_PREDICATION: s.predication = ((body[1] >> 13) & 7) != 0; break;
            case INDIRECT_BUFFER_PRIV: {
                uint32_t target = body[0], words = body[2];
                s.cur.ib_calls++;
                if (dumping(s)) s.dump.push_back(fmt_s("call DL %08x %u dwords", target, words));
                DlInfo& d = s.dls[target];
                d.calls++;
                d.dwords = words;
                if (d.last_frame != s.frame) d.frames++, d.last_frame = s.frame;
                if (d.hashes.size() < 64) d.hashes.insert(fnv(mem::host(target), (size_t)words * 4));
                uint64_t before = s.cur.draws_dl;
                walk(s, target, words, depth + 1);
                d.draws += s.cur.draws_dl - before;
                break;
            }
            case SURFACE_SYNC: s.surface_sync[body[0]]++; break;
            case EVENT_WRITE: s.events[body[0] & 0x3F]++; break;
            case EVENT_WRITE_EOP: s.eop++; break;
            case MEM_WRITE: s.mem_writes++; break;
            case WAIT_REG_MEM: s.waits++; break;
            case MEM_SEMAPHORE: s.semaphores++; break;
            case STRMOUT_BUFFER_UPDATE: case STRMOUT_BASE_UPDATE: s.so_updates++; break;
            case HLE_SPECIAL_STATE: s.special_state[body[0] << 16 | (body[1] & 0xFFFF)]++; break;
            case HLE_BEGIN_OCCLUSION_QUERY: s.occlusion_queries++; break;
            case HLE_SAMPLE_TIMER: s.timers++; break;
            case HLE_COPY_SURFACE_NEW:
                if (n >= 22) s.copy_surface[fmt_surface(body + 4) + " -> " + fmt_surface(body + 13)]++;
                if (dumping(s) && n >= 22)
                    s.dump.push_back(fmt_s("copy %08x %s -> %08x %s", body[4], fmt_surface(body + 4).c_str(), body[13],
                                           fmt_surface(body + 13).c_str()));
                break;
            case HLE_CLEAR_COLOR_DEPTH_STENCIL: {
                char b[96];
                snprintf(b, sizeof b, "mask %u color fmt %03x tile %u depth fmt %03x tile %u", body[0], body[2], body[3],
                         body[10], body[11]);
                s.clears[b]++;
                if (dumping(s))
                    s.dump.push_back(fmt_s("clear mask %u color %08x fmt %03x tile %u %ux%u depth %08x fmt %03x rgba %u,%u,%u,%u",
                                           body[0], body[1], body[2], body[3], body[4], body[5], body[9], body[10], body[17],
                                           body[18], body[19], body[20]));
                break;
            }
            case HLE_COPY_COLORBUFFER_TO_SCANBUFFER: {
                char b[96];
                snprintf(b, sizeof b, "%ux%u fmt %03x tile %u target %u", body[1], body[2], body[7], body[4], body[8]);
                s.scanout[b]++;
                if (dumping(s)) s.dump.push_back(fmt_s("scanout %08x %s", body[0], b));
                break;
            }
            case HLE_TRIGGER_SCANBUFFER_SWAP: {
                // frame boundary
                Frame& f = s.cur;
                uint64_t draws = f.draws_top + f.draws_dl;
                f.pipelines = f.pso.size();
                if (draws) s.frames_with_draws++;
                s.max_draws = std::max(s.max_draws, draws);
                Frame& t = s.total;
                t.draws_top += f.draws_top, t.draws_dl += f.draws_dl, t.ib_calls += f.ib_calls;
                t.draws_direct += f.draws_direct;
                t.dl_dwords += f.dl_dwords, t.top_dwords += f.top_dwords, t.indices += f.indices;
                t.draws_instanced += f.draws_instanced, t.draws_gs += f.draws_gs, t.draws_so += f.draws_so;
                t.draws_pred += f.draws_pred, t.draws_msaa += f.draws_msaa, t.draws_compute += f.draws_compute;
                t.rt_changes += f.rt_changes, t.pipelines += f.pipelines;
                const bool dumped_frame = dumping(s);
                s.frame++;
                int every = stats_interval();
                if (every > 0 && s.frame % every == 0) {
                    LOG("[gpu-stats] frame %llu: draws %llu (top %llu, in DLs %llu) DL calls %llu, dwords top %llu DL "
                        "%llu, RT switches %llu, PSO keys %zu, GS %llu SO %llu pred %llu msaa %llu compute %llu",
                        (unsigned long long)s.frame, (unsigned long long)draws, (unsigned long long)f.draws_top,
                        (unsigned long long)f.draws_dl, (unsigned long long)f.ib_calls,
                        (unsigned long long)f.top_dwords, (unsigned long long)f.dl_dwords,
                        (unsigned long long)f.rt_changes, f.pso.size(), (unsigned long long)f.draws_gs,
                        (unsigned long long)f.draws_so, (unsigned long long)f.draws_pred,
                        (unsigned long long)f.draws_msaa, (unsigned long long)f.draws_compute);
                }
                if (dumped_frame) {
                    std::string path = g_user_root + fmt_s("/gpu_frame_%llu.txt", (unsigned long long)s.frame - 1);
                    if (FILE* df = botw::fopen_utf8(path, "w")) {
                        for (auto& l : s.dump) fprintf(df, "%s\n", l.c_str());
                        fclose(df);
                    }
                    s.dump.clear();
                }
                s.cur = Frame{};
                s.program_hash.clear();
                break;
            }
            default: break;
        }
        i += 1 + n;
    }
}

template <typename K>
void dump_hist(FILE* f, const char* title, const Hist<K>& h, const char* hexfmt = nullptr) {
    fprintf(f, "\n%s (%zu)\n", title, h.size());
    std::vector<std::pair<K, uint64_t>> v(h.begin(), h.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [k, n] : v) {
        if constexpr (std::is_same_v<K, std::string>) fprintf(f, "  %10llu  %s\n", (unsigned long long)n, k.c_str());
        else fprintf(f, "  %10llu  0x%x\n", (unsigned long long)n, (unsigned)k);
    }
}

void write_report(Stats& s) {
    std::string path = g_user_root + "/gpu_stats.txt";
    FILE* f = botw::fopen_utf8(path, "w");
    if (!f) return;
    const Frame& t = s.total;
    uint64_t draws = t.draws_top + t.draws_dl;
    fprintf(f, "BotWRecomp PM4 stream statistics (--gpu-stats), %llu frames (%llu with draws), %llu submissions\n",
            (unsigned long long)s.frame, (unsigned long long)s.frames_with_draws, (unsigned long long)s.submissions);
    fprintf(f, "draws %llu: top-level %llu (%.1f%%), inside display lists %llu (%.1f%%); max per frame %llu\n",
            (unsigned long long)draws, (unsigned long long)t.draws_top, draws ? 100.0 * t.draws_top / draws : 0,
            (unsigned long long)t.draws_dl, draws ? 100.0 * t.draws_dl / draws : 0, (unsigned long long)s.max_draws);
    fprintf(f, "  of the display-list draws, %llu were in display lists submitted directly (GX2DirectCallDisplayList)\n",
            (unsigned long long)t.draws_direct);
    fprintf(f, "dwords: top-level %llu, from display lists %llu; DL calls %llu; max IB depth %llu\n",
            (unsigned long long)t.top_dwords, (unsigned long long)t.dl_dwords, (unsigned long long)t.ib_calls,
            (unsigned long long)s.max_ib_depth);
    fprintf(f, "draw flags: instanced %llu, GS %llu, streamout %llu, predicated %llu, MSAA %llu, compute %llu\n",
            (unsigned long long)t.draws_instanced, (unsigned long long)t.draws_gs, (unsigned long long)t.draws_so,
            (unsigned long long)t.draws_pred, (unsigned long long)t.draws_msaa, (unsigned long long)t.draws_compute);
    fprintf(f, "render-target switches %llu (%.1f per frame), distinct RT configurations %zu\n",
            (unsigned long long)t.rt_changes, s.frame ? (double)t.rt_changes / s.frame : 0, s.rt_sigs.size());
    fprintf(f, "pipeline keys: distinct overall %zu, average per frame %.1f\n", s.pso_all.size(),
            s.frame ? (double)t.pipelines / s.frame : 0);
    fprintf(f, "distinct programs (content hash): fetch %zu, vertex %zu, pixel %zu, geometry %zu, export(ES) %zu\n",
            s.programs[0].size(), s.programs[1].size(), s.programs[2].size(), s.programs[3].size(),
            s.programs[4].size());
    fprintf(f, "resources set: tex PS/VS/GS %llu/%llu/%llu, UBO PS/VS/GS %llu/%llu/%llu, attrib buffers %llu, "
               "samplers %llu, ALU-const dwords %llu\n",
            (unsigned long long)s.tex_sets[0], (unsigned long long)s.tex_sets[1], (unsigned long long)s.tex_sets[2],
            (unsigned long long)s.ubo_sets[0], (unsigned long long)s.ubo_sets[1], (unsigned long long)s.ubo_sets[2],
            (unsigned long long)s.attrib_sets, (unsigned long long)s.sampler_sets,
            (unsigned long long)s.alu_const_dwords);
    fprintf(f, "distinct textures %zu: cube %zu, array %zu, 3D %zu, MSAA %zu\n", s.textures.size(), s.tex_cube.size(),
            s.tex_array.size(), s.tex_3d.size(), s.tex_msaa.size());
    fprintf(f, "misc: occlusion queries %llu, streamout updates %llu, MEM_WRITE %llu, WAIT_REG_MEM %llu, "
               "semaphores %llu, timers %llu, EOP %llu, type-0 packets %llu, bad/unknown packets %llu, "
               "direct-called DL submissions %llu\n",
            (unsigned long long)s.occlusion_queries, (unsigned long long)s.so_updates, (unsigned long long)s.mem_writes,
            (unsigned long long)s.waits, (unsigned long long)s.semaphores, (unsigned long long)s.timers,
            (unsigned long long)s.eop, (unsigned long long)s.type0_packets, (unsigned long long)s.bad_packets,
            (unsigned long long)s.direct_dl_submits);
    // display lists
    uint64_t dl_once = 0, dl_reused = 0, dl_changed = 0, dl_with_draws = 0, direct = 0, direct_changed = 0;
    uint64_t ib = 0, ib_changed = 0, ib_calls = 0, ib_changed_calls = 0;
    for (auto& [a, d] : s.dls) {
        if (d.frames <= 1) dl_once++;
        else dl_reused++;
        if (d.hashes.size() > 1) dl_changed++;
        if (d.draws) dl_with_draws++;
        if (d.direct) direct++, direct_changed += d.hashes.size() > 1;
        else {
            ib++;
            ib_changed += d.hashes.size() > 1;
            ib_calls += d.calls;
            if (d.hashes.size() > 1) ib_changed_calls += d.calls;
        }
    }
    fprintf(f, "called display lists (INDIRECT_BUFFER targets): %llu distinct, %llu seen with more than one content; "
               "calls %llu, of which to changing lists %llu\n",
            (unsigned long long)ib, (unsigned long long)ib_changed, (unsigned long long)ib_calls,
            (unsigned long long)ib_changed_calls);
    fprintf(f, "\ndirect-called display lists: %llu distinct addresses, %llu of them seen with changing contents\n",
            (unsigned long long)direct, (unsigned long long)direct_changed);
    fprintf(f, "\ndisplay lists: %zu distinct addresses; called in one frame only %llu, in several frames %llu; "
               "content changed between calls %llu; containing draws %llu\n",
            s.dls.size(), (unsigned long long)dl_once, (unsigned long long)dl_reused, (unsigned long long)dl_changed,
            (unsigned long long)dl_with_draws);
    std::vector<std::pair<uint32_t, const DlInfo*>> top;
    for (auto& [a, d] : s.dls) top.push_back({a, &d});
    std::sort(top.begin(), top.end(), [](auto& x, auto& y) { return x.second->draws > y.second->draws; });
    for (size_t k = 0; k < top.size() && k < 25; k++) {
        auto& d = *top[k].second;
        fprintf(f, "  DL %08x%s: %u dwords, calls %llu in %llu frames, draws %llu, distinct contents %zu; "
                   "changed vs previous call %llu times (same packet layout %llu, %llu dwords changed)\n",
                top[k].first, d.direct ? " (direct)" : "", d.dwords, (unsigned long long)d.calls,
                (unsigned long long)d.frames, (unsigned long long)d.draws, d.hashes.size(),
                (unsigned long long)d.changed_calls, (unsigned long long)d.same_layout,
                (unsigned long long)d.changed_dwords);
    }
    fprintf(f, "  top-level submission range %08x-%08x\n", s.pool_lo, s.pool_hi);
    dump_hist(f, "packets (top-level)", s.packets_top);
    dump_hist(f, "packets (inside display lists)", s.packets_dl);
    dump_hist(f, "unknown opcodes", s.unknown_ops);
    fprintf(f, "\nprimitive types (draws)\n");
    for (auto& [p, n] : s.prim_types) fprintf(f, "  %10llu  %s (0x%x)\n", (unsigned long long)n, prim_name(p), p);
    dump_hist(f, "index types (INDEX_TYPE packets)", s.index_types);
    dump_hist(f, "color buffer formats (draws; hwfmt | numtype<<8)", s.color_fmt);
    dump_hist(f, "color buffer tile modes (draws)", s.color_tile);
    dump_hist(f, "depth formats (draws with Z enabled; DB_DEPTH_INFO&7)", s.depth_fmt);
    dump_hist(f, "depth tile modes (draws)", s.depth_tile);
    dump_hist(f, "MSAA (PA_SC_AA_CONFIG log2 samples, draws)", s.msaa);
    dump_hist(f, "texture dims (distinct textures)", s.tex_dim);
    dump_hist(f, "texture formats (distinct; hwfmt | numfmt<<8 | degamma<<12)", s.tex_fmt);
    dump_hist(f, "texture tile modes (distinct)", s.tex_tile);
    dump_hist(f, "texture component formats (distinct; word4 & 0x3ff)", s.tex_comp);
    dump_hist(f, "HLE copy surface (src -> dst)", s.copy_surface);
    dump_hist(f, "HLE clears", s.clears);
    dump_hist(f, "scan-out copies", s.scanout);
    dump_hist(f, "special states (id<<16|value)", s.special_state);
    dump_hist(f, "SURFACE_SYNC flags", s.surface_sync);
    dump_hist(f, "EVENT_WRITE event types", s.events);
    dump_hist(f, "CONTEXT_CONTROL word0", s.ctx_control);
    dump_hist(f, "direct display lists: changed dwords between consecutive submissions, by packet", s.dl_changes);
    fclose(f);
}
}  // namespace

// What changes in a display list between two submissions of the same address: classify every
// changed dword by the packet (and register range) it belongs to.
void diff_dl(Stats& s, DlInfo& d, uint32_t addr, uint32_t ndw) {
    std::vector<uint32_t> cur(ndw);
    for (uint32_t i = 0; i < ndw; i++) cur[i] = mem::r32(addr + i * 4);
    if (d.prev.size() == ndw) {
        // packet layout identical? (same headers at the same offsets)
        bool same = true;
        uint64_t changed = 0;
        for (uint32_t i = 0; i < ndw;) {
            uint32_t h = cur[i];
            if (h != d.prev[i]) {
                same = false;
                break;
            }
            uint32_t t = hdr_type(h), n = t == 3 ? hdr_count(h) : t == 0 ? hdr_count(h) : 0;
            if (t == 2) {
                i++;
                continue;
            }
            uint32_t op = t == 3 ? hdr_op(h) : 0x100;
            uint32_t reg0 = (t == 3 && n > 0 && i + 1 < ndw) ? cur[i + 1] : 0;
            for (uint32_t k = 1; k <= n && i + k < ndw; k++) {
                if (cur[i + k] == d.prev[i + k]) continue;
                changed++;
                if (op < 0x100 && s.logged_changes < 12 && ndw > 10000) {  // sample a few for write-watching
                    s.logged_changes++;
                    LOG("[gpu-stats] DL %08x changed: dword at %08x (%s +%u) %08x -> %08x", addr, addr + (i + k) * 4,
                        op_name(op) ? op_name(op) : "?", k, d.prev[i + k], cur[i + k]);
                }
                char b[96];
                const char* nm = op < 0x100 ? op_name(op) : "TYPE0";
                if (op == SET_RESOURCE) {
                    uint32_t off = reg0 + (k - 2);
                    struct { uint32_t base, n; const char* name; } cls[] = {
                        {kResPsTex, kTexPerStage, "PS tex"}, {kResPsUbo, kUboPerStage, "PS UBO"},
                        {kResVsTex, kTexPerStage, "VS tex"}, {kResVsUbo, kUboPerStage, "VS UBO"},
                        {kResAttrib, kAttribBuffers, "attrib buffer"}, {kResGsTex, kTexPerStage, "GS tex"},
                        {kResGsUbo, kUboPerStage, "GS UBO"}};
                    const char* cn = "other";
                    uint32_t word = 99;
                    for (auto& c : cls)
                        if (k >= 2 && off >= c.base && off < c.base + c.n * 7) cn = c.name, word = (off - c.base) % 7;
                    snprintf(b, sizeof b, "SET_RESOURCE %s word%u", cn, word);
                } else if (op == DRAW_INDEX_2)
                    snprintf(b, sizeof b, "DRAW_INDEX_2 word%u", k - 1);
                else if (op == SET_CONTEXT_REG || op == SET_CONFIG_REG || op == SET_CTL_CONST)
                    snprintf(b, sizeof b, "%s reg %04x", nm ? nm : "?", base_for(op) + reg0 + (k - 2));
                else
                    snprintf(b, sizeof b, "%s", nm ? nm : "?");
                s.dl_changes[b]++;
            }
            i += 1 + n;
        }
        if (same) d.same_layout++;
        else s.dl_changes["(packet layout changed: re-recorded)"]++, d.changed_calls++;
        if (changed) d.changed_calls++, d.changed_dwords += changed;
    } else if (!d.prev.empty())
        s.dl_changes["(size changed: re-recorded)"]++, d.changed_calls++;
    d.prev.swap(cur);
}

void stats_observe(uint32_t addr, uint32_t ndw) {
    static Stats* s = (g_stats = new Stats());
    std::lock_guard<std::mutex> l(s->lock);
    s->submissions++;
    s->submit_dwords += ndw;
    uint64_t f0 = s->frame;
    // GX2 command-pool buffers vs. game display lists submitted directly (GX2DirectCallDisplayList):
    // the pool is GX2Init's 4 MB ring (see the GX2Init log line), located from the first submissions
    if (s->submissions <= 16) s->pool_lo = std::min(s->pool_lo, addr), s->pool_hi = s->pool_lo + 0x400000;
    s->cur_direct = s->submissions > 16 && (addr < s->pool_lo || addr >= s->pool_hi);
    if (s->cur_direct) {
        s->direct_dl_submits++;
        DlInfo& d = s->dls[addr];
        d.calls++;
        d.dwords = ndw;
        d.direct = true;
        if (d.last_frame != s->frame) d.frames++, d.last_frame = s->frame;
        if (d.hashes.size() < 64) d.hashes.insert(fnv(mem::host(addr), (size_t)ndw * 4));
        diff_dl(*s, d, addr, ndw);
    }
    uint64_t before = s->cur.draws_direct;
    walk(*s, addr, ndw, 0);
    if (s->cur_direct) s->dls[addr].draws += s->cur.draws_direct - before;
    s->cur_direct = false;
    int every = stats_interval();
    if (every > 0 && s->frame != f0 && s->frame % every == 0) write_report(*s);
}
}  // namespace gpu_native
#endif  // BOTW_NATIVE_GPU
