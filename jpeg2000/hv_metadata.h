/* Shared JP2/JPX XML associations. */
#ifndef HV_SHARED_METADATA_H
#define HV_SHARED_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "hv_presentation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_metadata_document hv_metadata_document;
typedef struct {
    size_t codestream_count;
    hv_metadata_document *frames;
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

/* First XML associated with the codestream, including its JPCH, or the first
 * unassociated file-level XML if none is associated. Number lists at the start
 * of associations apply to descendants; grouping boxes are transparent.
 * Multiple documents are reduced to the first in file/traversal order.
 * Layer indices are independent of codestream indices. Returns 0 with borrowed
 * bytes (NULL when absent), or -1 with cleared outputs on an invalid index. */
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

#ifdef __cplusplus
}
#endif

#endif
