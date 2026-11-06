/-
  BotW-Native compact codegen — bit-exactness lemmas (Lean 4, core only).

  Mirrors the Z3 proof script with `bv_decide`, and adds the pieces the
  compact generator needs to be trusted: every lemma is a pure bitvector
  statement saying the fast path produces the *same bits* as the reference
  routine in runtime/include/ppc.h for every input.

  Run:  lean BotWVerify.lean      (Lean >= 4.32, no Mathlib)
-/

import Std.Tactic.BVDecide

set_option maxHeartbeats 4000000

namespace BotW

/-! ## 1. psq GQR type guards

`psq_load_fast` / `psq_store_fast` take the inline single path when the GQR type
field is 0..3 and go out of line otherwise. The reference tests bit 18 (loads) and
bit 2 (stores) of the GQR word; both must be exactly the "type < 4" test. -/

theorem psq_l_guard (g : BitVec 32) :
    ((g >>> 16) &&& 7) < 4 ↔ (g &&& 0x00040000) = 0 := by
  bv_decide

theorem psq_st_guard (g : BitVec 32) :
    (g &&& 7) < 4 ↔ (g &&& 0x00000004) = 0 := by
  bv_decide

/-! ## 2. f64 → f32 fast-path guard

The fast path is the original first branch, reached earlier; the condition must
be unchanged. `(x <<< 1) = 0` is the cheap "±0" test, the reference writes
`(x &&& ~0x8000000000000000) = 0`. -/

theorem f64_to_f32_guard (x : BitVec 64) :
    let exp := (x >>> 52) &&& 0x7FF
    (896 < exp ∨ (x <<< 1) = 0) ↔ (896 < exp ∨ (x &&& 0x7FFFFFFFFFFFFFFF) = 0) := by
  dsimp only
  bv_decide

/-! ## 3. ftz subnormal test

`fpclassify(f) == FP_SUBNORMAL` is "exponent all zero, mantissa non-zero". The
fast path uses `(u &&& 0x7FFFFFFF) - 1 < 0x007FFFFF`, which must agree on every
32-bit pattern — sign bit, NaN and infinity included. -/

def isSubnormalBits (u : BitVec 32) : Prop :=
  (u &&& 0x7F800000) = 0 ∧ (u &&& 0x007FFFFF) ≠ 0

theorem ftz_subnormal_test (u : BitVec 32) :
    isSubnormalBits u ↔ ((u &&& 0x7FFFFFFF) - 1) < 0x007FFFFF := by
  unfold isSubnormalBits
  bv_decide

/-! ## 4. CR field packing (`set_cr_f_fast`)

The reference writes four bytes `cr[4f+0..3] = lt, gt, eq, un`; the fast path
writes one little-endian `uint32_t` at `cr[4f]`. Same bytes, and the FPSCR.FPCC
mask is the same expression. -/

def crPacked (lt gt eq un : BitVec 8) : BitVec 32 :=
  lt.zeroExtend 32 ||| (gt.zeroExtend 32 <<< 8) ||| (eq.zeroExtend 32 <<< 16) |||
    (un.zeroExtend 32 <<< 24)

theorem cr_pack_bytes (lt gt eq un : BitVec 8) :
    (crPacked lt gt eq un).extractLsb' 0 8 = lt ∧
    (crPacked lt gt eq un).extractLsb' 8 8 = gt ∧
    (crPacked lt gt eq un).extractLsb' 16 8 = eq ∧
    (crPacked lt gt eq un).extractLsb' 24 8 = un := by
  unfold crPacked
  refine ⟨?_, ?_, ?_, ?_⟩ <;> bv_decide

/-- The `&&& 0x0000F000` in the fast path is redundant: the nibble shifted into
    place is already zero above bit 15. -/
theorem fpscr_pack (fpscr : BitVec 32) (n : BitVec 4) :
    (fpscr &&& 0xFFFF0FFF) ||| ((n.zeroExtend 32 <<< 12) &&& 0x0000F000)
      = (fpscr &&& 0xFFFF0FFF) ||| (n.zeroExtend 32 <<< 12) := by
  bv_decide

/-! ## 5. f32 → f64: the Cemu bit algorithm re-biases exactly like IEEE

The reference routine rebuilds the double by hand. The fast path just does
`(double)f`, which is exact for every non-NaN input (NaN goes to the slow path so
the payload stays un-quieted). To tie them together we model both in bits.

`denormResult` is the shared subnormal normalization (identical on both sides, so
that case is definitional); the interesting content is the re-bias of ordinary
normals, proved below for the whole 32-bit space. -/

def denormResult (x : BitVec 64) : BitVec 64 :=
  let out (f e : BitVec 64) : BitVec 64 :=
    ((x &&& 0x80000000) <<< 32) ||| (e <<< 52) ||| ((f &&& 0x007FFFFF) <<< 29)
  let rec go : Nat → BitVec 64 → BitVec 64 → BitVec 64
    | 0, f, e => out f e
    | n + 1, f, e => if f &&& 0x00800000 = 0 then go n (f <<< 1) (e - 1) else out f e
  go 24 (x &&& 0x007FFFFF) (1023 - 126)

def cemuF32ToF64 (v : BitVec 32) : BitVec 64 :=
  let x : BitVec 64 := v.zeroExtend 64
  let exp := (x >>> 23) &&& 0xFF
  let frac := x &&& 0x007FFFFF
  if exp > 0 && exp < 255 then
    let y : BitVec 64 := if exp >>> 7 = 0 then 1 else 0
    let z := (y <<< 61) ||| (y <<< 60) ||| (y <<< 59)
    ((x &&& 0xC0000000) <<< 32) ||| z ||| ((x &&& 0x3FFFFFFF) <<< 29)
  else if exp = 0 && frac ≠ 0 then
    denormResult x
  else
    let y : BitVec 64 := exp >>> 7
    let z := (y <<< 61) ||| (y <<< 60) ||| (y <<< 59)
    ((x &&& 0xC0000000) <<< 32) ||| z ||| ((x &&& 0x3FFFFFFF) <<< 29)

/-- IEEE-754 binary32 → binary64. Exact: every float is representable as a
    double, so rounding is the identity. -/
def ieeeF32ToF64 (v : BitVec 32) : BitVec 64 :=
  let x : BitVec 64 := v.zeroExtend 64
  let s := (v >>> 31) &&& 1
  let e := (x >>> 23) &&& 0xFF
  let f := x &&& 0x007FFFFF
  let sign : BitVec 64 := s.zeroExtend 64 <<< 63
  if e = 0 then
    if f = 0 then sign else denormResult x
  else if e = 255 then
    sign ||| (0x7FF <<< 52) ||| (f <<< 29)
  else
    sign ||| ((e + 896) <<< 52) ||| (f <<< 29)

/-- Ordinary normals: the hand-written re-bias equals the IEEE one. -/
theorem f32_to_f64_normal (v : BitVec 32)
    (h1 : 0 < ((v >>> 23) &&& 0xFF)) (h2 : ((v >>> 23) &&& 0xFF) < 255) :
    let x : BitVec 64 := v.zeroExtend 64
    let e := (x >>> 23) &&& 0xFF
    let y : BitVec 64 := if e >>> 7 = 0 then 1 else 0
    let z := (y <<< 61) ||| (y <<< 60) ||| (y <<< 59)
    ((x &&& 0xC0000000) <<< 32) ||| z ||| ((x &&& 0x3FFFFFFF) <<< 29)
      = (((v >>> 31) &&& 1).zeroExtend 64 <<< 63) ||| ((e + 896) <<< 52)
        ||| ((x &&& 0x007FFFFF) <<< 29) := by
  dsimp only
  bv_decide

/-- Zeros: both sides produce the sign bit alone. -/
theorem f32_to_f64_zero (v : BitVec 32)
    (he : (v >>> 23) &&& 0xFF = 0) (hf : v &&& 0x007FFFFF = 0) :
    cemuF32ToF64 v = ieeeF32ToF64 v := by
  have h0 : (v.zeroExtend 64 >>> 23) &&& 0xFF = 0 := by bv_decide
  have hf' : (v.zeroExtend 64 &&& 0x007FFFFF) = 0 := by bv_decide
  simp [cemuF32ToF64, ieeeF32ToF64, h0, hf']
  bv_decide

/-- Infinities: exponent 255 with a zero mantissa. -/
theorem f32_to_f64_inf (v : BitVec 32)
    (he : (v >>> 23) &&& 0xFF = 255) (hf : v &&& 0x007FFFFF = 0) :
    cemuF32ToF64 v = ieeeF32ToF64 v := by
  have h0 : (v.zeroExtend 64 >>> 23) &&& 0xFF = 255 := by bv_decide
  have hf' : (v.zeroExtend 64 &&& 0x007FFFFF) = 0 := by bv_decide
  simp [cemuF32ToF64, ieeeF32ToF64, h0, hf']
  bv_decide

/-- Subnormals: both models call the same normalization, so the result is
    definitional once the arguments are shown equal. -/
theorem f32_to_f64_denorm (v : BitVec 32)
    (he : (v >>> 23) &&& 0xFF = 0) (hf : v &&& 0x007FFFFF ≠ 0) :
    cemuF32ToF64 v = ieeeF32ToF64 v := by
  have h0 : (v.setWidth 64 >>> 23) &&& 0xFF = 0 := by bv_decide
  have hf' : (v.setWidth 64 &&& 0x007FFFFF) ≠ 0 := by bv_decide
  simp only [cemuF32ToF64, ieeeF32ToF64]
  split <;> try rfl
  all_goals bv_decide

/-- Ordinary normals, stated on the definitions themselves. -/
theorem f32_to_f64_normal_branch (v : BitVec 32)
    (he : (v >>> 23) &&& 0xFF ≠ 0) (hn : (v >>> 23) &&& 0xFF ≠ 255) :
    cemuF32ToF64 v = ieeeF32ToF64 v := by
  have h0 : (v.zeroExtend 64 >>> 23) &&& 0xFF ≠ 0 := by bv_decide
  have h255 : (v.zeroExtend 64 >>> 23) &&& 0xFF ≠ 255 := by bv_decide
  have hlt : (v.zeroExtend 64 >>> 23) &&& 0xFF < 255 := by bv_decide
  have hpos : 0 < ((v.zeroExtend 64 >>> 23) &&& 0xFF) := by bv_decide
  simp [cemuF32ToF64, ieeeF32ToF64, h0, h255, hlt, hpos]
  bv_decide

/-- Putting the cases together: for every non-NaN single, the Cemu bit algorithm
    and the IEEE widening conversion agree bit for bit. -/
theorem f32_to_f64_eq (v : BitVec 32)
    (h : ¬((v >>> 23) &&& 0xFF = 255 ∧ (v &&& 0x007FFFFF) ≠ 0)) :
    cemuF32ToF64 v = ieeeF32ToF64 v := by
  by_cases he : (v >>> 23) &&& 0xFF = 0
  · by_cases hf : v &&& 0x007FFFFF = 0
    · exact f32_to_f64_zero v he hf
    · exact f32_to_f64_denorm v he hf
  · by_cases h255 : (v >>> 23) &&& 0xFF = 255
    · have hf : v &&& 0x007FFFFF = 0 :=
        Decidable.byContradiction (fun hne => h ⟨h255, hne⟩)
      exact f32_to_f64_inf v h255 hf
    · exact f32_to_f64_normal_branch v he h255

end BotW
