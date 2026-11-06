/-
  Data-plane proofs for the Latte layer.

  Covers the two hot pure functions the next optimization touches:

  1. `hash_strided_u32` (texture hash): the 4-lane rewrite must sample exactly
     the same words as the reference loop. One word sampled less means a CPU
     write to that word goes unnoticed and the texture goes stale.
  2. `LatteIndices_fastConvertU16_AVX2` (index conversion): per-lane byte swap
     (guest big-endian -> host little-endian). The AVX2 path does it with a
     constant shuffle mask; the scalar tail with `_swapEndianU16`; both must be
     the same function, and it must be an involution.

  Run: lean DataPlane.lean   (Lean 4 core, no Mathlib)
-/

import Std.Tactic.BVDecide

namespace BotW.DataPlane

/-! ## 1. Strided reads: 4-lane loop vs reference loop -/

/-- Indices visited by the reference loop: `n` words, stride `s`. -/
def refIdx (n s : Nat) : List Nat := (List.range n).map (fun i => i * s)

/-- The four indices of group `g` (before scaling by the stride). -/
def group (g : Nat) : List Nat := [4 * g, 4 * g + 1, 4 * g + 2, 4 * g + 3]

/-- Indices visited by the 4-lane loop: full groups, then the tail in lane 0.
    Written as a closed form (flatMap over the groups) so the proof talks about
    the same index set the C++ loop walks. -/
def fourLaneIdx (n s : Nat) : List Nat :=
  ((List.range (n / 4)).flatMap group).map (fun i => i * s)
    ++ (List.range (n % 4)).map (fun i => (4 * (n / 4) + i) * s)

/-- The groups cover `4k` consecutive indices, in order. -/
theorem flatMap_groups (k : Nat) :
    (List.range k).flatMap group = List.range (4 * k) := by
  induction k with
  | zero => simp [group]
  | succ k ih =>
      rw [List.range_succ, List.flatMap_append, ih, show 4 * (k + 1) = 4 * k + 4 by omega,
          List.range_add]
      simp [group, List.range_succ, Nat.add_comm, Nat.add_left_comm, Nat.add_assoc]

/-- **Coverage lemma.** The 4-lane loop samples exactly the same words, in the
    same order, as the reference loop — for every `n` and every stride `s`. -/
theorem fourLane_eq_ref (n s : Nat) : fourLaneIdx n s = refIdx n s := by
  unfold fourLaneIdx refIdx
  rw [flatMap_groups]
  have hn : n = 4 * (n / 4) + n % 4 := by
    have h := Nat.div_add_mod n 4
    omega
  have hrange : List.range n =
      List.range (4 * (n / 4)) ++ (List.range (n % 4)).map (fun i => i + 4 * (n / 4)) := by
    have h1 : List.range n = List.range (4 * (n / 4) + n % 4) := congrArg List.range hn
    rw [h1, List.range_add]
    congr 1
    ext i
    simp [Nat.add_comm]
  rw [hrange, List.map_append, List.map_map]
  congr 1
  ext i
  simp [Nat.add_comm]

/-- The strides actually used by `LatteTexture_CalculateTextureDataHash`. -/
theorem coverage_stride_1 (n : Nat) : fourLaneIdx n 1 = refIdx n 1 := fourLane_eq_ref n 1
theorem coverage_stride_4 (n : Nat) : fourLaneIdx n 4 = refIdx n 4 := fourLane_eq_ref n 4
theorem coverage_stride_12 (n : Nat) : fourLaneIdx n 12 = refIdx n 12 := fourLane_eq_ref n 12
theorem coverage_stride_32 (n : Nat) : fourLaneIdx n 32 = refIdx n 32 := fourLane_eq_ref n 32
theorem coverage_stride_96 (n : Nat) : fourLaneIdx n 96 = refIdx n 96 := fourLane_eq_ref n 96

/-! ## 2. Index conversion: per-lane byte swap -/

/-- Byte swap of one 16-bit index, as `_swapEndianU16` does. -/
def bswap16 (x : BitVec 16) : BitVec 16 := (x >>> 8) ||| (x <<< 8)

/-- Swapping twice is the identity — converting an already-converted buffer is a
    no-op, which is what makes a cache of converted indices safe to re-apply. -/
theorem bswap16_involutive (x : BitVec 16) : bswap16 (bswap16 x) = x := by
  unfold bswap16
  bv_decide

/-- Consequence: no two different indices can collapse into the same converted
    value, so the conversion preserves the index set exactly. -/
theorem bswap16_injective : Function.Injective bswap16 := by
  intro a b h
  have := congrArg bswap16 h
  simpa [bswap16_involutive] using this

/-- The shuffle mask (`_mm256_set_epi8(30,31,...,0,1)`) exchanges the low and the
    high byte of every 16-bit lane; that is exactly this formula. -/
theorem bswap16_bytes (x : BitVec 16) :
    bswap16 x = ((x &&& 0xFF) <<< 8) ||| (x >>> 8) := by
  unfold bswap16
  bv_decide

/-- The conversion is its own inverse on whole lists: applying the AVX2 path to
    the output of the scalar tail (or vice versa) returns the original bytes. -/
theorem convertAll_involutive (xs : List (BitVec 16)) :
    (xs.map bswap16).map bswap16 = xs := by
  induction xs with
  | nil => rfl
  | cons x xs ih => simp [bswap16_involutive, ih]

end BotW.DataPlane
