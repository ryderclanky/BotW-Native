# Lean proofs for the compact codegen

`BotWVerify.lean` and `DataPlane.lean` (Lean 4 core, no Mathlib) prove with
`bv_decide` that every fast path produced by the compact codegen returns
**exactly the same bits** as the reference routine in `runtime/include/ppc.h`,
and that the rewritten loops sample/convert the same data as the original ones.

| lemma | what it guarantees |
|---|---|
| `psq_l_guard`, `psq_st_guard` | the GQR bit test is exactly "type < 4" |
| `f64_to_f32_guard` | the fast-path condition is the original one (`(x<<1)==0` ⇔ `±0`) |
| `ftz_subnormal_test` | `(u&0x7FFFFFFF)-1 < 0x7FFFFF` ⇔ subnormal, for every 32-bit pattern |
| `cr_pack_bytes` | four byte stores ≡ one 32-bit little-endian store (CR) |
| `fpscr_pack` | the extra `0x0000F000` mask on FPSCR is redundant |
| `f32_to_f64_eq` | the Cemu re-bias algorithm ≡ exact IEEE conversion for every non-NaN single |
| `fourLane_eq_ref` | the 4-lane strided loop reads exactly the same words as the reference loop, for every `n` and stride |
| `bswap16_*` | the index byte swap is an involution and a bijection |

Run: `lake build` (or `lean BotWVerify.lean`). The toolchain is pinned in
`lean-toolchain`.

NaN is excluded on purpose: the fast path branches to the original routine so
the payload is preserved un-quieted. Equality against the real hardware (not
against the model) is covered by `../exhaustive_f32.c`, which checks all 2^32
single-precision patterns in about 18 s:

```sh
gcc -O2 -fno-strict-aliasing -o exhaustive_f32 ../exhaustive_f32.c -lm
./exhaustive_f32
```

A differential test that runs the old and the new generator over randomly
initialised guest states (registers, memory, GQRs) is the third layer; it is
not included here.
