/* hv_reconstruct.h: one codestream of a store, written as a JPEG 2000
 * codestream a decoder reads.
 *
 * The server delivers a codestream as data-bins: its main header, and for
 * each precinct one bin holding that precinct's packets, layer after layer.
 * This writes them as a codestream again: the main header, one tile-part,
 * and the precinct bins in resolution, position, component order. That is
 * the order of the bin identifiers and the RPCL progression, so the COD is
 * rewritten to RPCL and no packet is parsed. A precinct the client holds
 * nothing of gets one empty packet per layer; a decoder then reads the
 * image up to the resolution that was requested.
 *
 * For codestreams of the served profile (JPIP_PROFILE.md): one tile, zero
 * origins, unit sampling, no COC or POC. Every precinct bin held must be
 * complete or have whole packets confirmed by hv_reconstruct_confirm.
 * Unknown partial precincts are refused. */
#ifndef HV_RECONSTRUCT_H
#define HV_RECONSTRUCT_H

#include <stddef.h>
#include <stdint.h>

#include "hv_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of codestream `codestream` of `cache` as a JPEG 2000 codestream,
 * which is written to out when capacity holds it (so out NULL and capacity
 * 0 ask for the size). 0, with a message in error, if it cannot be
 * written. Frame information is prepared once and retained in the cache.
 * Receiving, reconstruction and status calls must be serialized. */
size_t hv_reconstruct(const hv_cache *cache, uint64_t codestream, uint8_t *out,
                      size_t capacity, char *error, size_t error_size);

/* Records delivery of a whole-frame window through `layers` quality layers,
 * without the `reduce` highest resolutions. Call only after applying an entire
 * response ending WINDOW_DONE or IMAGE_DONE for that exact window, with no byte
 * limit. Calls and response ingestion must be serialized. Each covered bin's
 * current byte length is a whole-packet boundary. Returns 0, or -1 with error;
 * a rejected confirmation changes no quality records. */
int hv_reconstruct_confirm(hv_cache *cache, uint64_t codestream, int reduce, int layers,
                            char *error, size_t error_size);

/* A codestream of a store, from its main header: the image's size and
 * components, its resolution levels, and how many of them, from the
 * lowest, are complete at full quality. `quality` also describes confirmed
 * previews: the minimum whole layers across the precinct bins of each
 * resolution. All 0 until the main header is complete. */
typedef struct {
    uint32_t width, height;
    int components;
    int resolutions;
    int complete;
    int layers;          /* total source quality layers */
    int quality[33];     /* whole layers per resolution, lowest first */
} hv_status;

/* 0, or -1 with a message in error for a main header hv_reconstruct
 * refuses. */
int hv_reconstruct_status(const hv_cache *cache, uint64_t codestream, hv_status *status,
                          char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
