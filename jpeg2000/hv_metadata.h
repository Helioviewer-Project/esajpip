/* Shared JP2/JPX XML associations and codestream palettes. */
#ifndef HV_SHARED_METADATA_H
#define HV_SHARED_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "hv_presentation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_metadata_frame hv_metadata_frame;
typedef struct hv_metadata_document hv_metadata_document;
typedef struct {
    size_t codestream_count;
    hv_metadata_frame *frames;
    size_t layer_count;
    hv_metadata_document *layers;
    const uint8_t *file_xml;
    size_t file_xml_size;
} hv_metadata;

/* Borrowed metadata index over bounded file boxes. codestream_count and
 * layer_count are the caller's discovered counts (0 skips layer indexing).
 * Start with {0}; close before reopening and releasing the underlying bytes.
 * resolve optionally replaces transport placeholders with complete contents:
 * 1 means interpret, 0 means skip, -1 means error. NULL reads plain file boxes.
 * Returns 0, or -1 with an empty index. */
typedef int (*hv_metadata_resolve)(const void *context, uint32_t *type,
                                  const uint8_t **payload, size_t *size,
                                  char *error, size_t error_size);
int hv_metadata_read(const uint8_t *data, size_t size, size_t codestream_count, size_t layer_count,
                     hv_metadata_resolve resolve, const void *context,
                     hv_metadata *metadata, char *error, size_t error_size);
void hv_metadata_close(hv_metadata *metadata);

/* The first XML associated with the codestream or the same-numbered layer
 * in this client's movie profile. Number lists at the start of associations
 * apply to their descendants; grouping boxes are transparent. Inline boxes
 * and original-content placeholders use the same traversal, bounded by
 * HV_BOX_DEPTH_MAX. If no associated XML is found, use the first unassociated
 * file-level XML. Multiple XML documents are reduced to the first in box order.
 * Returns 0 with borrowed bytes and their size, or *xml NULL if absent;
 * -1 with a message if the frame index is out of range. */
int hv_metadata_xml(const hv_metadata *metadata, uint64_t codestream, const uint8_t **xml, size_t *size,
                    char *error, size_t error_size);

/* Codestream association only, including XML in that JPCH, with the same
 * file-level fallback. Layer indices are independent of codestream indices. */
int hv_metadata_codestream_xml(const hv_metadata *metadata, uint64_t codestream,
                               const uint8_t **xml, size_t *size, char *error, size_t error_size);

/* First XML in file/traversal order associated with the layer (including its
 * JPLH) or any of its registered codestreams. File-level XML is used only when
 * none of those entities has XML. registration=NULL queries the layer alone.
 * A registration entry is a codestream index, never a layer index. Returns 0
 * with borrowed bytes (NULL when absent), or -1 with cleared outputs on error. */
int hv_metadata_layer_xml(const hv_metadata *metadata, size_t layer,
                          const hv_registration *registration, const uint8_t **xml,
                          size_t *size, char *error, size_t error_size);

/* The color table of a codestream: the palette (pclr, T.800 I.5.3.4) that
 * its component mapping (cmap, I.5.3.5) applies to a component, from the
 * file's JP2 Header box or, in a JPX file, box by box from the
 * codestream's own header box (jpch) where that has one (T.801 M.11.6).
 * A decoded sample of the component is then an index into the table.
 *
 * Returns the number of entries (1 to 1,024), 0 when the codestream has no
 * color table, or -1 with a message in error for an invalid index or boxes that
 * are not a palette. *channels gets the values of an entry: the channels
 * the mapping makes of the component, in its order (3 for red, green,
 * blue). table, when capacity holds entries * *channels bytes, gets them
 * entry by entry, each value scaled to 8 bits; at most 1,024 * 255 bytes. */
enum { HV_PALETTE_MAX = 1024 * 255 };
int hv_metadata_palette(const hv_metadata *metadata, uint64_t codestream, int *channels, uint8_t *table,
                        size_t capacity, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
