/* JPIP metadata-bin adapter to the shared JP2/JPX metadata reader. */
#ifndef HVC_METADATA_H
#define HVC_METADATA_H
#include "hvc_cache.h"
#include "jpeg2000/hv_metadata.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Indexes complete metadata bins once. Start with {0}; close before reopening.
 * Returns 0, or -1 with a message and an empty index. The index borrows the
 * complete bins' bytes, which stay fixed until hvc_cache_release; adding other
 * bins does not invalidate it. Close the index before releasing the store. */
int hvc_metadata_open(const hvc_cache *cache, hv_metadata *metadata,
                      char *error, size_t error_size);
/* The served profile retains presentation headers inline and numbers source
 * placeholders in codestream order. Borrows completed bin 0 until cache release. */
int hvc_metadata_presentation(const hvc_cache *cache, const uint8_t **data,
                               hv_presentation *presentation, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
