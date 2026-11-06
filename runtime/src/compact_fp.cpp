// Out-of-line slow paths for the compact codegen (see runtime/include/compact_fp.h).
#include "ppc.h"

extern "C" __attribute__((noinline, cold)) double ppc_f32_to_f64_slow(uint32_t v) {
    return f32_to_f64(v);
}

extern "C" __attribute__((noinline, cold)) uint32_t ppc_f64_to_f32_slow(double d) {
    return f64_to_f32(d);
}

extern "C" __attribute__((noinline, cold)) void psq_load_q(PPCContext* c, uint32_t ea, int w,
                                                          int i, int frd) {
    psq_load(c, g_mem, ea, w, c->gqr[i], frd);
}

extern "C" __attribute__((noinline, cold)) void psq_store_q(PPCContext* c, uint32_t ea, int w,
                                                           int i, int frs) {
    psq_store(c, g_mem, ea, w, c->gqr[i], frs);
}

// The *_fast inline paths assume MXCSR.DAZ=0 (denormal inputs are not zeroed) and
// round-to-nearest; both are the x86-64 defaults. Used by the CI/debug check.
#include <xmmintrin.h>
extern "C" bool rt_compact_fp_mxcsr_ok(void) { return (_mm_getcsr() & 0x6040u) == 0; }
