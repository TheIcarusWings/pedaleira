#pragma once
// amp/FastMath.h: reciprocal and reciprocal square root with about 23 bits of
// precision, without the latency of a divide or a square root.
//
// The waveshapers in this folder are built from x / sqrt(1 + x^2) style
// algebraic curves, and a divide or sqrt chain per sample per stage is what
// limits their speed at an oversampled rate. On x86 with SSE the hardware
// estimate instructions (about 12 bits) are refined with one Newton-Raphson
// step (Newton, "Method of Fluxions", the standard iteration
// y' = y (1.5 - 0.5 x y^2) for 1/sqrt(x) and y' = y (2 - x y) for 1/x). Other
// targets use the exact operations. Inputs must be positive and finite for
// invSqrt, non-zero for recip.
//
// Plain C++ and std only (plus the SSE intrinsics header where available).
#include <cmath>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define AMP_FASTMATH_SSE 1
#endif

namespace amp {

/** 1 / sqrt(x) for x > 0. */
inline float invSqrt(float x) noexcept {
#ifdef AMP_FASTMATH_SSE
  const float y = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
  return y * (1.5f - 0.5f * x * y * y);
#else
  return 1.0f / std::sqrt(x);
#endif
}

/** 1 / x for x != 0. */
inline float recip(float x) noexcept {
#ifdef AMP_FASTMATH_SSE
  const float y = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
  return y * (2.0f - x * y);
#else
  return 1.0f / x;
#endif
}

}  // namespace amp
