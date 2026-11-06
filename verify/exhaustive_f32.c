// Exhaustive check over all 2^32 single-precision bit patterns:
//  (1) f32_to_f64_fast(v) == f32_to_f64(v)   (Cemu ConvertToDoubleNoFTZ, ppc.h v0.1.2)
//  (2) ftz_single_fast post-processing == ftz_single post-processing (fpclassify/copysignf)
// Build: gcc -O2 -fno-strict-aliasing -o ex exhaustive_f32.c -lm ; run with nice. Needs MXCSR DAZ=0 (default).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static inline uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double bitsd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static double f32_to_f64(uint32_t v) {  // verbatim from runtime/include/ppc.h
    uint64_t x = v; uint64_t exp = (x >> 23) & 0xFF; uint64_t frac = x & 0x007FFFFF;
    if (exp > 0 && exp < 255) { uint64_t y = !(exp >> 7); uint64_t z = y << 61 | y << 60 | y << 59;
        return bitsd(((x & 0xC0000000) << 32) | z | ((x & 0x3FFFFFFF) << 29)); }
    else if (exp == 0 && frac != 0) { exp = 1023 - 126; do { frac <<= 1; exp -= 1; } while ((frac & 0x00800000) == 0);
        return bitsd(((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007FFFFF) << 29)); }
    uint64_t y = exp >> 7; uint64_t z = y << 61 | y << 60 | y << 59;
    return bitsd(((x & 0xC0000000) << 32) | z | ((x & 0x3FFFFFFF) << 29));
}
__attribute__((noinline)) static double slow(uint32_t v) { return f32_to_f64(v); }
static inline double f32_to_f64_fast(uint32_t v) {
    float f; memcpy(&f, &v, 4); double d = (double)f;
    if (__builtin_expect(d != d, 0)) return slow(v);
    return d;
}
static inline uint32_t ftz_orig(uint32_t u) { float f; memcpy(&f, &u, 4); if (fpclassify(f) == FP_SUBNORMAL) f = copysignf(0.0f, f); memcpy(&u, &f, 4); return u; }
static inline uint32_t ftz_fast(uint32_t u) { if (((u & 0x7FFFFFFFu) - 1u) < 0x007FFFFFu) u &= 0x80000000u; return u; }
int main(void) {
    uint64_t bad1 = 0, bad2 = 0, nan_slow = 0;
    for (uint64_t i = 0; i <= 0xFFFFFFFFull; i++) {
        uint32_t v = (uint32_t)i;
        if (dbits(f32_to_f64_fast(v)) != dbits(f32_to_f64(v))) { if (bad1 < 5) printf("f32_to_f64 mismatch %08x\n", v); bad1++; }
        if (ftz_fast(v) != ftz_orig(v)) { if (bad2 < 5) printf("ftz mismatch %08x\n", v); bad2++; }
        float f; memcpy(&f, &v, 4); if (f != f) nan_slow++;
    }
    printf("checked 2^32 patterns: f32_to_f64 mismatches=%llu, ftz mismatches=%llu, NaN inputs (slow path)=%llu\n",
           (unsigned long long)bad1, (unsigned long long)bad2, (unsigned long long)nan_slow);
    return (bad1 || bad2) ? 1 : 0;
}
