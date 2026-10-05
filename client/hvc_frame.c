#include "hvc_frame.h"

#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_grid.h"
#include "jpeg2000/hv_reader.h"

void hvc_frame_free(hvc_frame *frame) {
    if (frame != NULL) free(frame->header);
    free(frame);
}

const hvc_frame *hvc_frame_get(const hvc_cache *cache, uint64_t codestream,
                               char *error, size_t error_size) {
    hvc_bin *bin = (hvc_bin *) hvc_cache_find(cache, HVC_BIN_MAIN_HEADER, codestream, 0);
    hv_codestream cs;
    hv_item item;
    hvc_frame *frame;
    const Cod *cod;
    const SizFixed *siz;
    uint64_t packets, end = 0;
    int r;

    if (bin == NULL || !bin->complete) {
        hv_fail(error, error_size, "main header data-bin is %s",
                bin == NULL ? "missing" : "incomplete");
        return NULL;
    }
    if (bin->frame != NULL) return bin->frame;
    frame = (hvc_frame *) calloc(1, sizeof *frame);
    if (frame == NULL) {
        hv_fail(error, error_size, "out of memory for frame information");
        return NULL;
    }
    if (hv_codestream_open(&cs, bin->data, 0, bin->length, HV_READ_JPIP) != 0) {
        hv_fail(error, error_size, "main header: %s", cs.error);
        goto fail;
    }
    frame->header = (uint8_t *) malloc(bin->length);
    if (frame->header == NULL) {
        hv_fail(error, error_size, "out of memory for frame header");
        goto fail;
    }
    memcpy(frame->header, bin->data, 2);
    frame->header_size = 2;
    do {
        size_t at = frame->header_size;
        int status = hv_codestream_next(&cs, &item);
        if (status != 1 || item.kind != HV_SEGMENT) {
            hv_fail(error, error_size, "main header: %s", status < 0 ? cs.error :
                    "main header data-bin holds more than the header");
            goto fail;
        }
        if (item.code == 0xFF55 || item.code == 0xFF57) continue;
        memcpy(frame->header + at, bin->data + item.start, item.end - item.start);
        frame->header_size += item.end - item.start;
        if (item.code == 0xFF52) frame->header[at + 5] = 2; /* RPCL */
    } while (item.end < bin->length);
    cod = hv_codestream_cod(&cs);
    if (cod == NULL) {
        hv_fail(error, error_size, "main header has no COD");
        goto fail;
    }
    packets = hv_rule_packets(hv_codestream_siz(&cs), &cod->sgcod, &cod->spcod);
    if (packets == 0) {
        hv_fail(error, error_size, "main header: more than 2,147,483,647 packets");
        goto fail;
    }
    siz = hv_codestream_siz(&cs)->fixed;
    frame->width = siz->xsiz;
    frame->height = siz->ysiz;
    frame->components = siz->csiz;
    frame->resolutions = cod->spcod.levels + 1;
    frame->layers = cod->sgcod.layers;
    frame->eph = cod->scod.ephMarkers;
    for (r = 0; r < frame->resolutions; ++r) {
        unsigned level = (unsigned)(frame->resolutions - 1 - r);
        int custom = cod->spcod.precincts.nCount != 0;
        int ppx = custom ? cod->spcod.precincts.arr[r].ppx : 15;
        int ppy = custom ? cod->spcod.precincts.arr[r].ppy : 15;
        uint64_t w = hv_resolution_coord(frame->width, level);
        uint64_t h = hv_resolution_coord(frame->height, level);
        end += hv_precinct_count(0, w, (uint64_t)1 << ppx) *
               hv_precinct_count(0, h, (uint64_t)1 << ppy) * siz->csiz;
        frame->precinct_end[r] = end;
    }
    hv_codestream_close(&cs);
    bin->frame = frame;
    return frame;
fail:
    hv_codestream_close(&cs);
    hvc_frame_free(frame);
    return NULL;
}
