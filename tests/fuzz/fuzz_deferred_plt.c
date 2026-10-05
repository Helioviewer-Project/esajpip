/* fuzz_deferred_plt: compare eager served-profile codestream validation
 * with structural traversal plus explicit deferred PLT consumption. This
 * exercises malformed PLT boundaries, pause/resume cursor state, and the
 * requirement that callers finish tile and codestream validation. The cursor
 * reads alternately from the input and from a copy of it at another address,
 * as a server that unmaps and maps a file again between reads, so that it
 * cannot depend on the buffer it began a segment with.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_reader.h"

static void check_stream(const uint8_t *data, size_t start, size_t end) {
    hv_codestream cs;
    hv_item item;
    hv_plt_reader plt;
    const char *eager;
    const char *deferred = NULL;
    size_t eager_at = 0;
    size_t deferred_at = 0;
    size_t tile_at = 0;
    unsigned reads = 0;
    uint8_t *copy = malloc(end ? end : 1);
    int status;

    if (copy == NULL)
        abort();
    memcpy(copy, data, end);

    eager = hv_codestream_check(data, start, end, HV_READ_JPIP, &eager_at);
    hv_plt_init(&plt, HV_PLT_PROFILE);
    status = hv_codestream_open(&cs, data, start, end, HV_READ_JPIP_INDEX);
    while (status == 0 && (status = hv_codestream_next(&cs, &item)) == 1) {
        if (item.kind == HV_TILE_PART)
            tile_at = item.start;
        if (item.plt != NULL) {
            uint64_t value;
            int read;

            deferred = hv_plt_begin(&plt, item.plt);
            if (deferred == NULL) {
                do
                    read = hv_plt_read(&plt, reads++ & 1 ? copy : data, &value);
                while (read == 1);
                deferred = plt.error;
            }
            deferred_at = item.start;
        } else if (item.kind == HV_TILE_DATA) {
            deferred = hv_plt_end_tile(&plt, item.end - item.start);
            deferred_at = tile_at;
        } else if (item.kind == HV_END) {
            deferred = hv_plt_end(&plt, hv_rule_packets(hv_codestream_siz(&cs),
                  &hv_codestream_cod(&cs)->sgcod, &hv_codestream_cod(&cs)->spcod));
            deferred_at = item.start;
        }
        if (deferred != NULL)
            break;
        status = 0;
    }
    if (status < 0) {
        deferred = cs.error;
        deferred_at = cs.error_at;
    }
    if (!((eager == NULL && deferred == NULL) ||
          (eager != NULL && deferred != NULL && strcmp(eager, deferred) == 0 &&
           eager_at == deferred_at)))
        abort();
    hv_codestream_close(&cs);
    free(copy);
}

static void check_container_streams(const uint8_t *data, size_t size) {
    hv_boxes boxes;
    hv_box box;
    const char *error;
    size_t at;

    hv_boxes_file(&boxes, data, size);
    while (hv_boxes_next(&boxes, &box, &error, &at) == 1) {
        if (box.type == HV_BOX_JP2C)
            check_stream(data, box.payload, box.end);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= 2 && data[0] == 0xff && data[1] == 0x4f)
        check_stream(data, 0, size);
    check_container_streams(data, size);
    return 0;
}
