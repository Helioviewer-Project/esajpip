/* hv_metadata.h: what the metadata data-bins of a store say about the target.
 *
 * Metadata bin 0 holds file boxes and JPIP placeholders. Metadata boxes may
 * remain inline or have their contents in referenced bins. */
#ifndef HV_METADATA_H
#define HV_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "hv_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_metadata_frame hv_metadata_frame;
typedef struct {
    size_t count;
    hv_metadata_frame *frames;
} hv_metadata;

/* Indexes complete metadata bins once. Start with {0}; close before reopening.
 * Returns 0, or -1 with a message and an empty index. The index borrows the
 * complete bins' bytes, which stay fixed until hv_cache_release; adding other
 * bins does not invalidate it. Close the index before releasing the store. */
int hv_metadata_open(const hv_cache *cache, hv_metadata *metadata,
                     char *error, size_t error_size);
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
