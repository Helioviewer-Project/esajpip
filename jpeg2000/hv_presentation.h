/* Borrowed JP2/JPX presentation, channel and color information. */
#ifndef HV_PRESENTATION_H
#define HV_PRESENTATION_H
#include "hv_rules.h"
#ifdef __cplusplus
extern "C" {
#endif

/* A registration borrows the CREG entries, or represents the implicit
 * identity mapping of layer i to codestream i when entries is NULL. */
typedef struct {
    const uint8_t *entries;
    size_t count, implicit_codestream;
    uint16_t denominator_x, denominator_y;
} hv_registration;

typedef struct {
    size_t codestream;
    uint8_t sampling_x, sampling_y, alignment_x, alignment_y;
} hv_registration_entry;

/* Reads a bounded CREG body: denominator, exact six-byte entries, nonzero
 * sampling and alignment smaller than the denominator. Does not require
 * identity registration or a single source. Requires a box from a bounded
 * iterator. Outputs unchanged on failure. */
const char *hv_registration_open(const uint8_t *buf, const hv_box *box,
                                 hv_registration *registration, size_t *at);
const char *hv_registration_read(const hv_registration *registration, size_t index,
                                 hv_registration_entry *entry);

/* Bounded CMAP/CDEF field access. Views borrow the input; open checks list
 * framing, read returns numeric fields without interpreting their roles or
 * checking referenced components, palette columns or channels. Rendering and
 * conformance checks belong to their consumers. Outputs stay unchanged on error.
 * Pass boxes obtained from the bounded box iterator. */
typedef struct {
    const uint8_t *entries;
    size_t count;
} hv_component_mapping;
typedef struct {
    uint16_t component;
    uint8_t type, column;       /* 0 direct, 1 palette; other types uninterpreted */
} hv_component_mapping_entry;
const char *hv_component_mapping_open(const uint8_t *buf, const hv_box *box,
                                      hv_component_mapping *mapping, size_t *at);
const char *hv_component_mapping_read(const hv_component_mapping *mapping, size_t index,
                                      hv_component_mapping_entry *entry);

typedef struct {
    const uint8_t *entries;
    size_t count;
} hv_channel_definition;
typedef struct {
    uint16_t channel, type, association; /* 0 colour, 1 opacity, 2 premultiplied */
} hv_channel_definition_entry;
const char *hv_channel_definition_open(const uint8_t *buf, const hv_box *box,
                                       hv_channel_definition *definition, size_t *at);
const char *hv_channel_definition_read(const hv_channel_definition *definition, size_t index,
                                       hv_channel_definition_entry *entry);

/* COLR fields with the method-specific remainder borrowed and opaque.
 * Method 1 includes EnumCS; ICC/vendor/unknown methods retain
 * their full body after the three common fields. Precedence is signed.
 * This does not select a colour method or validate its profile/parameters. */
typedef struct {
    uint8_t method, approximation;
    int precedence;
    uint32_t colour_space;       /* only meaningful when method == 1 */
    const uint8_t *data;
    size_t size;
} hv_colour;
const char *hv_colour_read(const uint8_t *buf, const hv_box *box,
                           hv_colour *colour, size_t *at);

/* PCLR samples borrow the bounded input. Opening checks counts, 1..38-bit
 * depths and the exact table extent. Reading returns an exact signed/unsigned
 * integer, without scaling to display bytes or selecting palette columns.
 * Outputs remain unchanged on failure; pass a bounded box descriptor. */
typedef struct {
    size_t entry_count, column_count, row_bytes;
    const uint8_t *depths, *values;
    uint16_t column_offsets[255]; /* row offsets, bounded by 255 five-byte columns */
} hv_palette;
typedef struct {
    int64_t value;
    unsigned bits;
    int is_signed;
} hv_palette_sample;
const char *hv_palette_open(const uint8_t *buf, const hv_box *box,
                            hv_palette *palette, size_t *at);
const char *hv_palette_read(const hv_palette *palette, size_t entry, size_t column,
                            hv_palette_sample *sample);

typedef struct {
    hv_box header;              /* zero type for an implicit layer */
    hv_registration registration;
} hv_layer;

/* Top-level JP2/JPX presentation inventory. Arrays are owned; boxes and
 * registration entries borrow the input until hv_presentation_free. A missing
 * header has type zero. JP2H defaults, JPCH overrides and JPLH layer headers
 * remain separate; their channel/colour bodies are not interpreted here. */
typedef struct {
    hv_box defaults;
    int is_jpx;
    size_t codestreams, layers;
    hv_box *codestream_headers;
    hv_layer *layer;
} hv_presentation;

/* JP2 has one implicit layer. Ordinary JPX uses JPLH order, or one implicit
 * layer per codestream when there are no JPLH boxes. Checks interpreted
 * registration fields/references and bounded header children, leaving other
 * bodies (including composition instructions) opaque. Repeated JPX containers
 * and J2CX are unsupported. This is access, not whole-file conformance or
 * rendering validation. Output unchanged on failure; free after success. */
const char *hv_presentation_open(const uint8_t *buf, size_t size,
                                 hv_presentation *presentation, size_t *at);
/* Detached container headers, with source boxes omitted or replaced by a
 * transport. The caller establishes the logical codestream count. Header and
 * layer interpretation is identical to open; source payloads are not read. */
const char *hv_presentation_open_headers(const uint8_t *buf, size_t size, size_t codestreams,
                                         hv_presentation *presentation, size_t *at);
void hv_presentation_free(hv_presentation *presentation);

/* Effective presentation boxes for one source in a layer's registration.
 * All boxes borrow the inventory's input. Missing boxes have type zero.
 * colour is a CGRP or the default JP2H: iterate its children for all COLR
 * alternatives, retaining their file order. No colour method is selected.
 * palette/mapping belong to the registered codestream, not the layer index.
 * Channel binding is CDEF or OPCT; a local OPCT suppresses default CDEF.
 * pixel_format is retained for admission/interpretation before rendering. */
typedef struct {
    size_t codestream;
    hv_box palette, mapping, colour, channels, opacity, pixel_format;
} hv_presentation_headers;

/* Resolve JPCH and JPLH overrides against JP2H independently. source indexes
 * the layer's registration entries, not the file's codestreams. Only selected
 * box framing and relevant cross-reference targets are read; payloads remain
 * opaque. Relevant cross-references are explicitly unsupported, rather than
 * silently using defaults. No allocation; outputs unchanged on failure. */
const char *hv_presentation_read_headers(const uint8_t *buf,
                                         const hv_presentation *presentation,
                                         size_t layer, size_t source,
                                         hv_presentation_headers *headers, size_t *at);

#ifdef __cplusplus
}
#endif
#endif
