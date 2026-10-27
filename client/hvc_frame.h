#ifndef HVC_FRAME_H
#define HVC_FRAME_H

#include "hvc_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Prepared from a complete main-header bin, owned by that bin until cache
 * release. Resolution indices run from lowest to highest. */
typedef struct hvc_frame {
    uint32_t width, height;
    int components, resolutions, layers, eph;
    uint64_t precinct_end[33];
    uint8_t *header;
    size_t header_size;
} hvc_frame;

/* Validate a main header without changing the cache. Caller owns the result. */
hvc_frame *hvc_frame_read(const uint8_t *data, size_t size, char *error, size_t error_size);
const hvc_frame *hvc_frame_get(const hvc_cache *cache, uint64_t codestream,
                               char *error, size_t error_size);
void hvc_frame_free(hvc_frame *frame);

#ifdef __cplusplus
}
#endif
#endif
