/* Supported JP2/JPX colour-channel instructions for a host decoder. */
#ifndef HV_RENDER_H
#define HV_RENDER_H
#include "hv_presentation.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t component;           /* decoder output component, after its transforms */
    int palette_column;         /* -1 direct, otherwise column in palette */
} hv_render_channel;
typedef struct {
    size_t codestream;
    uint32_t colour_space;      /* 16 sRGB, 17 grayscale */
    size_t channel_count;
    hv_render_channel channel[3]; /* colour order, independently of source order */
    hv_palette palette;        /* zero counts when no selected channel uses it */
} hv_render;

/* Resolve supported single-codestream, identity-registered grayscale/RGB
 * layers. output_components is the host decoder's output count for the sole
 * registered codestream, not the SIZ coded-component count. Colour alternatives
 * use highest supported signed precedence, then file order; JP2 ignores
 * precedence/approximation. Unknown colour methods remain unselected.
 * Selected channels may be direct or palette-mapped. Opacity, nonidentity
 * registration, multiple sources and pixel-format overrides are unsupported.
 * This supplies channel instructions, not decoded geometry/pixels: the host
 * must check selected output sample grids and decode capabilities before use.
 * Palette bytes borrow the presentation input until it is released. No allocation;
 * output unchanged on failure. Source byte access is independent of this call. */
const char *hv_render_read(const uint8_t *buf, const hv_presentation *presentation,
                           size_t layer, size_t output_components,
                           hv_render *render, size_t *at);
#ifdef __cplusplus
}
#endif
#endif
