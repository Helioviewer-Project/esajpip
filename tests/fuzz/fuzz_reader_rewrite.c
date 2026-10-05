/* fuzz_reader_rewrite: container reader and writer stability for JP2/JPX
 * structures. Successful rewrites must be readable again. Byte-for-byte
 * comparable rewrites must not change the input, and normalized rewrites
 * must be stable after one pass.
 */
#include "jpeg2000/hv_served.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_rewrite.h"

static void check_rewrite_stability(const uint8_t *data, size_t size) {
    hv_out once, twice;
    hv_rewrite_result first, second;

    hv_out_init(&once);
    hv_out_init(&twice);
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);

    if (hv_rewrite(data, size, 0, HV_READ_VALIDATE, &once, &first) == 0) {
        if (first.uncomparable == NULL &&
            (once.size != size || (size != 0 && memcmp(once.data, data, size) != 0)))
            abort();
        if (hv_rewrite(once.data, once.size, 0, HV_READ_VALIDATE, &twice, &second) != 0)
            abort();
        if (twice.size != once.size ||
            (once.size != 0 && memcmp(twice.data, once.data, once.size) != 0))
            abort();
    }

    hv_out_free(&twice);
    hv_out_free(&once);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hv_box jp2c;
    hv_served_sources jpx;
    size_t at = 0;

    /* General access reads only framing and file type; body interpretation
     * remains with the operation that requests it. */
    {
        hv_boxes boxes;
        hv_ftyp type;
        const char *error;
        int status;
        if (hv_container_open(data, size, &boxes, &type, &at) == NULL) {
            while ((status = hv_boxes_next(&boxes, &jp2c, &error, &at)) == 1) {
                if (jp2c.type == HV_BOX_FTBL) {
                    hv_fragments fragments;
                    Fragment fragment;
                    if (hv_fragments_open(data, &jp2c, &fragments, &at) == NULL) {
                        for (size_t i = 0; i < fragments.count; i++)
                            (void)hv_fragment_read(&fragments, i, &fragment, &at);
                    }
                } else if (jp2c.type == HV_BOX_DTBL) {
                    hv_boxes references;
                    hv_box url;
                    size_t count, length;
                    const uint8_t *loc;
                    if (hv_references_open(data, &jp2c, &references, &count, &at) == NULL) {
                        while (hv_boxes_next(&references, &url, &error, &at) == 1)
                            (void)hv_url_read(data, &url, &loc, &length, &at);
                    }
                }
                if (jp2c.start > jp2c.payload || jp2c.payload > jp2c.end ||
                    jp2c.end > size)
                    abort();
            }
            if ((status == 0 && boxes.pos != size) || (status < 0 && at > size))
                abort();
        }
    }
    check_rewrite_stability(data, size);
    if (hv_served_jp2(data, size, &jp2c, &at) == NULL) {
        (void)hv_check_jp2h(data, size, &at);
        (void)hv_codestream_check(data, jp2c.payload, jp2c.end, HV_READ_JPIP, &at);
    }
    if (hv_served_jpx(data, size, &jpx, &at) == NULL) {
        (void)hv_check_jpx_headers(data, size, &at);
        for (size_t i = 0; i < jpx.count && jpx.jp2c != NULL; i++)
            (void)hv_codestream_check(data, jpx.jp2c[i].payload, jpx.jp2c[i].end,
                                      HV_READ_JPIP, &at);
        hv_served_sources_free(&jpx);
    }
    return 0;
}
