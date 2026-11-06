// Compact-codegen fast paths. Included only by generated translation units that
// contain hot guest functions (see tools/recompile.py), so changing this header does not
// force a rebuild of the cold 900+ TUs.
//
// Every routine here must return exactly the same bits as the reference routine in ppc.h for
// every input. Proofs/tests: verify/lean (Lean bv_decide), Z3, exhaustive 2^32
// + difftest). Requirements: MXCSR.DAZ = 0 and MXCSR.RC = round-to-nearest on guest threads
// (the x86-64 defaults; checked by rt_compact_fp_mxcsr_ok()).
#pragma once

#include "ppc.h"

extern "C" double ppc_f32_to_f64_slow(uint32_t v);
extern "C" uint32_t ppc_f64_to_f32_slow(double d);
extern "C" void psq_load_q(PPCContext* c, uint32_t ea, int w, int gqr_index, int frd);
extern "C" void psq_store_q(PPCContext* c, uint32_t ea, int w, int gqr_index, int frs);

PPC_INLINE double f32_to_f64_fast(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    double d = (double)f;  // exact for zero, denormal (DAZ=0), normal and infinity
    if (__builtin_expect(d != d, 0)) return ppc_f32_to_f64_slow(v);  // NaN: keep the payload un-quieted
    return d;
}

PPC_INLINE uint32_t f64_to_f32_fast(double d) {
    uint64_t x = dbits(d);
    uint32_t exp = (x >> 52) & 0x7FF;
    if (__builtin_expect(exp > 896 || (x << 1) == 0, 1))
        return (uint32_t)(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
    return ppc_f64_to_f32_slow(d);  // single-denormal range: same algorithm, out of line
}

PPC_INLINE double ftz_single_fast(double d) {
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    if (__builtin_expect(((u & 0x7FFFFFFFu) - 1u) < 0x007FFFFFu, 0)) u &= 0x80000000u;  // subnormal -> signed 0
    memcpy(&f, &u, 4);
    return f;
}

// psq with a compile-time GQR index: types 0..3 are plain singles (scale unused) -> inline;
// quantized types 4..7 (rare in the hot set: 0 of 36k samples) -> out of line.
PPC_INLINE void psq_load_fast(PPCContext* c, uint8_t* M, uint32_t ea, int w, int i, int frd) {
    if (__builtin_expect(!(c->gqr[i] & 0x00040000u), 1)) {
        c->f[frd] = f32_to_f64_fast(ld32_(M, ea));
        c->ps1[frd] = w ? 1.0 : f32_to_f64_fast(ld32_(M, ea + 4));
    } else {
        psq_load_q(c, ea, w, i, frd);
    }
}

PPC_INLINE void psq_store_fast(PPCContext* c, uint8_t* M, uint32_t ea, int w, int i, int frs) {
    if (__builtin_expect(!(c->gqr[i] & 4u), 1)) {
        st32_(M, ea, f64_to_f32_fast(c->f[frs]));
        if (!w) st32_(M, ea + 4, f64_to_f32_fast(c->ps1[frs]));
    } else {
        psq_store_q(c, ea, w, i, frs);
    }
}

// fcmpu: same CR/FPSCR bits as set_cr_f, CR field written as one 32-bit store.
PPC_INLINE void set_cr_f_fast(PPCContext* c, int f, double a, double b) {
    uint32_t lt = a < b, gt = a > b, eq = a == b, un = !(lt | gt | eq);
    uint32_t w = lt | gt << 8 | eq << 16 | un << 24;
    memcpy(&c->cr[4 * f], &w, 4);
    c->fpscr = (c->fpscr & 0xFFFF0FFFu) | ((lt << 3 | gt << 2 | eq << 1 | un) << 12);
}
