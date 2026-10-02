// Faster forms of the SSE intrinsics the recompiled game uses, for AArch64.
//
// The recompiled code is built with -ffp-model=strict, which the guest's FP
// environment needs: it switches flush-to-zero and rounding with `msr fpcr`
// between FP instructions, and strict FP keeps every operation on its side of
// each write. The price is that clang lowers vector FP *compares* as constrained
// intrinsics and the AArch64 backend scalarizes those: SIMDe's cmpeq_ps was
// 22 instructions where one fcmeq does it, min/max 23-27 where three do, and
// every VMX compare, clamp and float-to-int conversion in skinning, particles
// and physics paid for it.
//
// The forms below issue the one NEON instruction directly. The asm is volatile
// so that it stays where it is relative to the `msr fpcr` writes (those are
// volatile asm too, and volatile asm is never reordered against volatile asm),
// which is all the strict model was protecting. The results are the hardware's
// own, identical to what the constrained scalar code produced.
//
// Included at the end of the guest precompiled header (renut_guest_pch.h), after
// the SDK's headers, so only the generated code that follows is affected.
#pragma once

#if defined(__aarch64__) && defined(__ARM_NEON)

#include <arm_neon.h>

// The vector tables lvlx/lvrx/lvsl and every lvx/stvx byte swap read, as
// read-only data local to each translation unit. The SDK keeps them in mutable,
// exported arrays: every access went through the GOT, was reloaded after each
// store (the compiler cannot rule out that a store changed them), and the
// shuffle mask was masked again each time. As constants the address is a
// PC-relative load that is hoisted out of the function once, and the mask's
// own `and` folds away. Copied from rex/ppc/intrinsics.h by CMake.
#include "renut_vector_tables.inc"
#define VectorMaskL renut_VectorMaskL
#define VectorMaskR renut_VectorMaskR
#define VectorShiftTableL renut_VectorShiftTableL
#define VectorShiftTableR renut_VectorShiftTableR

#define RENUT_NEON_COMPARE(name, insn)                                                               \
  static inline __attribute__((always_inline)) uint32x4_t name(float32x4_t a, float32x4_t b) {       \
    uint32x4_t r;                                                                                    \
    __asm__ __volatile__(insn " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b));                   \
    return r;                                                                                        \
  }
RENUT_NEON_COMPARE(renut_fcmeq, "fcmeq")
RENUT_NEON_COMPARE(renut_fcmgt, "fcmgt")
RENUT_NEON_COMPARE(renut_fcmge, "fcmge")
#undef RENUT_NEON_COMPARE

// Compares give all ones where the relation holds and all zeros where it does
// not, and NaN makes every ordered relation false, in SSE and NEON alike.
static inline __attribute__((always_inline)) simde__m128 renut_cmpeq_ps(simde__m128 a, simde__m128 b) {
  return vreinterpretq_f32_u32(renut_fcmeq(a, b));
}
static inline __attribute__((always_inline)) simde__m128 renut_cmpgt_ps(simde__m128 a, simde__m128 b) {
  return vreinterpretq_f32_u32(renut_fcmgt(a, b));
}
static inline __attribute__((always_inline)) simde__m128 renut_cmpge_ps(simde__m128 a, simde__m128 b) {
  return vreinterpretq_f32_u32(renut_fcmge(a, b));
}
static inline __attribute__((always_inline)) simde__m128 renut_cmplt_ps(simde__m128 a, simde__m128 b) {
  return vreinterpretq_f32_u32(renut_fcmgt(b, a));
}
static inline __attribute__((always_inline)) simde__m128 renut_cmple_ps(simde__m128 a, simde__m128 b) {
  return vreinterpretq_f32_u32(renut_fcmge(b, a));
}
// SSE's maxps/minps return the second operand when either is NaN or the two
// are equal (so +0 and -0 give the second), which is "a > b ? a : b" exactly.
static inline __attribute__((always_inline)) simde__m128 renut_max_ps(simde__m128 a, simde__m128 b) {
  return vbslq_f32(renut_fcmgt(a, b), a, b);
}
static inline __attribute__((always_inline)) simde__m128 renut_min_ps(simde__m128 a, simde__m128 b) {
  return vbslq_f32(renut_fcmgt(b, a), a, b);
}
// vctsxs / vctuxs with no scaling: truncate to signed / unsigned 32 bits,
// saturating, NaN to zero. That is what fcvtzs and fcvtzu do, where the x86
// forms in rex/ppc/intrinsics.h spend two dozen instructions fixing up cvttps.
// The generated code names them both bare and as rex::ppc::simde_mm_vct*, so
// they live in that namespace and are brought into the global one as well.
namespace rex::ppc {
static inline __attribute__((always_inline)) simde__m128i renut_vctsxs(simde__m128 a) {
  int32x4_t r;
  __asm__ __volatile__("fcvtzs %0.4s, %1.4s" : "=w"(r) : "w"(a));
  return vreinterpretq_s64_s32(r);
}
static inline __attribute__((always_inline)) simde__m128i renut_vctuxs(simde__m128 a) {
  uint32x4_t r;
  __asm__ __volatile__("fcvtzu %0.4s, %1.4s" : "=w"(r) : "w"(a));
  return vreinterpretq_s64_u32(r);
}
}  // namespace rex::ppc
using rex::ppc::renut_vctsxs;
using rex::ppc::renut_vctuxs;

#define simde_mm_cmpeq_ps(a, b) renut_cmpeq_ps((a), (b))
#define simde_mm_cmpgt_ps(a, b) renut_cmpgt_ps((a), (b))
#define simde_mm_cmpge_ps(a, b) renut_cmpge_ps((a), (b))
#define simde_mm_cmplt_ps(a, b) renut_cmplt_ps((a), (b))
#define simde_mm_cmple_ps(a, b) renut_cmple_ps((a), (b))
#define simde_mm_max_ps(a, b) renut_max_ps((a), (b))
#define simde_mm_min_ps(a, b) renut_min_ps((a), (b))
#define simde_mm_vctsxs(a) renut_vctsxs((a))
#define simde_mm_vctuxs(a) renut_vctuxs((a))

#endif  // __aarch64__ && __ARM_NEON
