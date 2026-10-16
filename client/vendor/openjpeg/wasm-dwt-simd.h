/* WASM SIMD equivalents for the OpenJPEG DWT's existing SSE/SSE2 kernels.
 * No relaxed SIMD or fused arithmetic: preserve the reference operation order.
 * Kept local to dwt.c; this does not claim an x86 target to other source files. */
#ifndef HVC_WASM_DWT_SIMD_H
#define HVC_WASM_DWT_SIMD_H
#include <wasm_simd128.h>
typedef v128_t __m128;
typedef v128_t __m128i;
#define _mm_add_ps wasm_f32x4_add
#define _mm_mul_ps wasm_f32x4_mul
#define _mm_set1_ps wasm_f32x4_splat
#define _mm_set_ps(d,c,b,a) wasm_f32x4_make((a),(b),(c),(d))
#define _mm_add_epi32 wasm_i32x4_add
#define _mm_sub_epi32 wasm_i32x4_sub
#define _mm_srai_epi32 wasm_i32x4_shr
#define _mm_set1_epi32 wasm_i32x4_splat
#define _mm_load_si128 wasm_v128_load
#define _mm_loadu_si128 wasm_v128_load
#define _mm_store_si128 wasm_v128_store
#define _mm_storeu_si128 wasm_v128_store
#define _mm_unpacklo_epi32(a,b) wasm_i32x4_shuffle((a),(b),0,4,1,5)
#define _mm_unpackhi_epi32(a,b) wasm_i32x4_shuffle((a),(b),2,6,3,7)
#endif
