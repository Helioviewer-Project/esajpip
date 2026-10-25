/* AArch64 NEON equivalents for the OpenJPEG DWT's existing SSE/SSE2 kernels.
 * No fused arithmetic: preserve the reference operation order (compile with
 * -ffp-contract=off). Shift counts are constants at every use.
 * Kept local to dwt.c; this does not claim an x86 target to other source files. */
#ifndef HVC_NEON_DWT_SIMD_H
#define HVC_NEON_DWT_SIMD_H
#include <arm_neon.h>
typedef float32x4_t __m128;
typedef int32x4_t __m128i;
#define _mm_add_ps vaddq_f32
#define _mm_mul_ps vmulq_f32
#define _mm_set1_ps vdupq_n_f32
static inline float32x4_t hvc_neon_set_ps(float d, float c, float b, float a)
{
    float values[4] = {a, b, c, d};
    return vld1q_f32(values);
}
#define _mm_set_ps hvc_neon_set_ps
#define _mm_add_epi32 vaddq_s32
#define _mm_sub_epi32 vsubq_s32
#define _mm_srai_epi32 vshrq_n_s32
#define _mm_set1_epi32 vdupq_n_s32
#define _mm_load_si128(p) vld1q_s32((const int32_t *)(p))
#define _mm_loadu_si128(p) vld1q_s32((const int32_t *)(p))
#define _mm_store_si128(p, v) vst1q_s32((int32_t *)(p), (v))
#define _mm_storeu_si128(p, v) vst1q_s32((int32_t *)(p), (v))
#define _mm_unpacklo_epi32 vzip1q_s32
#define _mm_unpackhi_epi32 vzip2q_s32
#endif
