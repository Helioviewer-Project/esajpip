/* hvc_reconstruct.c: see hvc_reconstruct.h. */
#include "hvc_reconstruct.h"

#include <string.h>

#include "hvc_frame.h"
#include "jpeg2000/hv_error.h"

typedef struct {
    uint8_t *out;
    size_t capacity, size;
} output;

static void put(output *o, const void *bytes, size_t n) {
    if (o->size <= o->capacity && n <= o->capacity - o->size)
        memcpy(o->out + o->size, bytes, n);
    o->size += n;
}

size_t hvc_reconstruct(const hvc_cache *cache, uint64_t codestream, uint8_t *out,
                       size_t capacity, char *error, size_t error_size) {
    static const uint8_t tile_part[] = {
        0xFF, 0x90, 0, 10, 0, 0, 0, 0, 0, 0, 0, 1,     /* SOT: tile 0, to EOC, part 0 of 1 */
        0xFF, 0x93};                                    /* SOD */
    static const uint8_t empty[] = {0x00, 0xFF, 0x92};  /* empty packet, EPH */
    static const uint8_t eoc[] = {0xFF, 0xD9};
    const hvc_bin *tile_header = hvc_cache_find(cache, HVC_BIN_TILE_HEADER, codestream, 0);
    output o = {out, out != NULL ? capacity : 0, 0};
    const hvc_frame *frame;
    uint64_t precincts, id, layer;

    frame = hvc_frame_get(cache, codestream, error, error_size);
    if (frame == NULL) return 0;
    if (tile_header != NULL && tile_header->length != 0) {
        hv_fail(error, error_size, "tile header data-bin is not empty");
        return 0;
    }
    put(&o, frame->header, frame->header_size);
    precincts = frame->precinct_end[frame->resolutions - 1];

    /* A precinct's bin identifier is c + s * components, s its number among
     * the precincts of its component, from the lowest resolution, in raster
     * order (T.808 A.3.2.1). With one precinct grid for all components,
     * increasing identifiers are the RPCL order. */
    put(&o, tile_part, sizeof tile_part);
    for (id = 0; id < precincts; id++) {
        const hvc_bin *bin = hvc_cache_find(cache, HVC_BIN_PRECINCT, codestream, id);
        if (bin != NULL && bin->complete) {
            put(&o, bin->data, bin->length);
        } else {
            int received = bin != NULL ? bin->layers : 0;
            if (received != 0) put(&o, bin->data, bin->packet_bytes);
            for (layer = (uint64_t)received; layer < (uint64_t)frame->layers; layer++)
                put(&o, empty, frame->eph ? 3 : 1);
        }
    }
    put(&o, eoc, sizeof eoc);
    return o.size;
}

int hvc_reconstruct_confirm(hvc_cache *cache, uint64_t codestream, int reduce, int layers,
                             char *error, size_t error_size) {
    const hvc_frame *frame = hvc_frame_get(cache, codestream, error, error_size);
    uint64_t id, end;
    if (frame == NULL) return -1;
    if (reduce < 0 || reduce >= frame->resolutions || layers < 1 || layers > frame->layers) {
        hv_fail(error, error_size, "invalid resolution or quality-layer count");
        return -1;
    }
    end = frame->precinct_end[frame->resolutions - 1 - reduce];
    for (id = 0; id < end; id++) {
        const hvc_bin *bin = hvc_cache_find(cache, HVC_BIN_PRECINCT, codestream, id);
        if (bin == NULL || (!bin->complete &&
            (bin->length == 0 || layers == frame->layers))) {
            hv_fail(error, error_size, "precinct data-bin %llu was not delivered",
                    (unsigned long long)id);
            return -1;
        }
    }
    for (id = 0; id < end; id++) {
        hvc_bin *bin = (hvc_bin *) hvc_cache_find(cache, HVC_BIN_PRECINCT, codestream, id);
        if (layers > bin->layers) {
            bin->layers = layers;
            bin->packet_bytes = bin->length;
        }
    }
    return 0;
}

int hvc_reconstruct_status(const hvc_cache *cache, uint64_t codestream, hvc_frame_status *status,
                           char *error, size_t error_size) {
    const hvc_bin *main_header = hvc_cache_find(cache, HVC_BIN_MAIN_HEADER, codestream, 0);
    const hvc_frame *frame;
    uint64_t id = 0;
    int r, full = 1;

    memset(status, 0, sizeof *status);
    if (main_header == NULL || !main_header->complete)
        return 0;
    frame = hvc_frame_get(cache, codestream, error, error_size);
    if (frame == NULL) return -1;
    status->width = frame->width;
    status->height = frame->height;
    status->components = frame->components;
    status->resolutions = frame->resolutions;
    status->layers = frame->layers;
    for (r = 0; r < frame->resolutions; r++) {
        uint64_t end = frame->precinct_end[r];
        int quality = frame->layers;
        while (id < end) {
            const hvc_bin *bin = hvc_cache_find(cache, HVC_BIN_PRECINCT, codestream, id);
            int layers = bin == NULL ? 0 : bin->complete ? frame->layers : bin->layers;
            if (layers < quality) quality = layers;
            id++;
        }
        status->quality[r] = quality;
        full = full && quality == frame->layers;
        if (full) status->complete++;
    }
    return 0;
}
