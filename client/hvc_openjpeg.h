/* hvc_openjpeg.h: a JPEG 2000 codestream decoded to 8-bit samples, with
 * OpenJPEG (vendor/openjpeg).
 *
 * For what hvc_reconstruct writes: a codestream whose higher resolutions may
 * hold empty packets only, so the caller says how many of them to leave
 * out. */
#ifndef HVC_OPENJPEG_H
#define HVC_OPENJPEG_H

#include <stddef.h>
#include <stdint.h>
#include "jpeg2000/hv_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width, height;             /* of the resolution decoded */
    uint32_t full_width, full_height;   /* of the whole image */
    int resolutions;                    /* in the codestream */
    int components;                     /* 1 (gray) or 3 (RGB) */
    uint8_t *pixels;                    /* width * height * components, rows
                                         * from the top, components interleaved */
} hvc_image;

/* Decodes the codestream without its `reduce` highest resolutions (0 for
 * the whole image; the lowest resolution alone if it has no more than
 * that), each of which halves the size. Samples deeper than 8 bits keep
 * their 8 most significant bits. render selects decoder output components
 * in display order; palette channels preserve their shared unsigned index
 * component (at most 8 bits), for the host's palette lookup. Selected sample
 * grids must cover the image without resampling. NULL render gives the first
 * three components as RGB, or the first as gray when there are fewer than three.
 * 0, with image->pixels malloc'd; or -1 with a message in error. */
int hvc_openjpeg_decode(const uint8_t *codestream, size_t size, int reduce, const hv_render *render, hvc_image *image,
                        char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
