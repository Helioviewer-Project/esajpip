/* hvc_openjpeg.h: a JPEG 2000 codestream decoded to 8-bit samples, with
 * OpenJPEG (vendor/openjpeg).
 *
 * Accepts raw codestreams, including local input and hvc_reconstruct snapshots
 * whose omitted resolutions hold empty packets. For arbitrary local coding,
 * the caller inspects the usable reductions; OpenJPEG may reject a reduction
 * that a main-header count permits because of another component or tile COD/COC. */
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
    uint32_t x, y, grid_width, grid_height; /* cropped output in the reduced grid */
    int resolutions;                    /* minimum selected-component main-header
                                         * count; not the usable local reduction range */
    int components;                     /* 1 (gray) or 3 (RGB) */
    uint8_t *pixels;                    /* width * height * components, rows
                                         * from the top, components interleaved;
                                         * see hvc_decoder_output for RGBA */
} hvc_image;

/* Decodes the codestream without its `reduce` highest resolutions (0 for
 * the whole image; clamped to the selected components' main-header count),
 * each of which halves the size. Tile/component overrides may still reject it.
 * Samples deeper than 8 bits keep their 8 most significant bits.
 * render selects decoder output components
 * in display order; palette channels preserve their shared unsigned index
 * component (at most 8 bits), for the host's palette lookup. Selected sample
 * grids must cover the image without resampling. NULL render gives the first
 * three components as RGB, or the first as gray when there are fewer than three.
 * 0, with image->pixels malloc'd; or -1 with a message in error. */
int hvc_openjpeg_decode(const uint8_t *codestream, size_t size, int reduce, const hv_render *render, hvc_image *image,
                        char *error, size_t error_size);

/* Reusable workspace with 0–8 workers. All calls on a context, including
 * statistics, must be serialized by one coordinator; none may overlap a decode.
 * Context/region pixels are borrowed until the next decode or destroy. */
typedef struct hvc_decoder hvc_decoder;
hvc_decoder *hvc_decoder_create(int threads);
void hvc_decoder_destroy(hvc_decoder *decoder);
void *hvc_decoder_input(hvc_decoder *decoder, size_t size);
size_t hvc_decoder_retained(const hvc_decoder *decoder);
size_t hvc_decoder_coefficient_allocations(const hvc_decoder *decoder);
size_t hvc_decoder_coefficient_hits(const hvc_decoder *decoder);
/* Makes the next decode on this context write its pixels to the caller's
 * buffer, so that image->pixels is that buffer. With rgba, RGB pixels take 4
 * bytes, the last one 255. The decode fails if they need more than capacity. */
void hvc_decoder_output(hvc_decoder *decoder, uint8_t *pixels, size_t capacity, int rgba);
/* After a completed decode: bytes (0), hits (1), misses (2). */
size_t hvc_decoder_codeblock_stat(const hvc_decoder *decoder, unsigned which);
/* Successful input/output buffer growths; coefficient allocations are separate. */
size_t hvc_decoder_growths(const hvc_decoder *decoder);
int hvc_openjpeg_decode_context(hvc_decoder *decoder, const uint8_t *data, size_t size,
    int reduce, const hv_render *render, hvc_image *image, char *error, size_t error_size);

/* Region bounds are [x0,y0,x1,y1), relative to the full image, before reduction.
 * Output x/y locate the crop within grid_width/grid_height. */
int hvc_openjpeg_decode_region(hvc_decoder *decoder, const uint8_t *data, size_t size,
    int reduce, const hv_render *render, const uint32_t region[4], hvc_image *image,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
