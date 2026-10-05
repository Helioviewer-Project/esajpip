/* transcode_file.c: hv_transcode_file, see transcode.h. */
#include "jpeg2000/hv_served.h"
#include <limits.h>
#include <string.h>

#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_reader.h"
#include "transcode.h"

/* ------------------------------------------------------------------------
 * Boxes
 * ------------------------------------------------------------------------ */

/* Transcodes a JP2 file that passed container and JP2 header/tree checks
 * into out. The boxes come out in the order they went in, so a box with
 * LBox = 0, the last one, is
 * still last and is copied as read; so are the children of a superbox. The
 * codestream, and a top-level XML box cut at a NUL, get explicit lengths. */
static int transcode_boxes(const uint8_t *buf, size_t size, int ppx, int ppy, hv_output_profile profile, hv_out *out,
                           char *error, size_t error_size) {
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at, start, copied;
    int status;

    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &message, &at)) == 1) {
        if (box.type == HV_BOX_JP2C) {
            /* The transcoded codestream goes straight into its box. */
            if (hv_begin_box(out, HV_BOX_JP2C, 0, &start) != 0 ||
                hv_transcode_codestream(buf, box.payload, box.end, ppx, ppy, profile,
                                        out, error, error_size) != 0 ||
                hv_end_box(out, start) != 0)
                return -1;
            continue;
        }
        if (box.type == HV_BOX_XML) {
            /* An XML box holds a well-formed XML document (T.800 I.7.1),
             * which has no NUL (XML 1.0, Char); old Kakadu, in IDL, ended
             * its XML boxes with one. The box ends before its first NUL;
             * one without is copied as read, below. */
            const uint8_t *nul = memchr(buf + box.payload, 0, box.end - box.payload);
            if (nul != NULL) {
                size_t n = (size_t)(nul - (buf + box.payload));
                if (hv_write_box_header(out, HV_BOX_XML, n) != 0 ||
                    hv_write_bytes(out, buf + box.payload, n) != 0)
                    return -1;
                continue;
            }
        }
        copied = out->size;
        if (hv_write_bytes(out, buf + box.start, box.end - box.start) != 0)
            return -1;
        /* The input reader ignores these fields; the JP2 writer emits zero. */
        if (box.type == HV_BOX_FTYP)
            memset(out->data + copied + box.payload - box.start + 4, 0, 4);
        else if (box.type == HV_BOX_JP2H) {
            hv_boxes children;
            hv_box child;
            hv_boxes_children(&children, buf, &box);
            while (hv_boxes_next(&children, &child, &message, &at) == 1)
                if (child.type == HV_BOX_COLR)
                    memset(out->data + copied + child.payload - box.start + 1, 0, 2);
        }
    }
    /* The container reader already checked these box extents. */
    return status < 0 ? hv_fail(error, error_size, "%s at %zu", message, at) : 0;
}

int hv_transcode_file(const uint8_t *buf, size_t size, int ppx, int ppy, hv_output_profile profile, hv_out *out,
                      char *error, size_t error_size) {
    size_t out_start = out->size, at;
    const char *rule;
    hv_box jp2c;
    int status;
    hv_header header;

    error[0] = 0;
    if (out->error != NULL)                     /* an earlier write failed */
        return hv_fail(error, error_size, "%s", out->error);
    if (profile != HV_OUTPUT_JPEG2000 && profile != HV_OUTPUT_JPIP)
        return hv_fail(error, error_size, "invalid output profile");
    /* Header consistency is checked here; packet-operation requirements
     * are checked once by the transcoder below. */
    rule = profile == HV_OUTPUT_JPIP ? hv_served_jp2(buf, size, &jp2c, &at)
                                     : hv_read_jp2(buf, size, &jp2c, &at);
    if (rule != NULL ||
        (rule = hv_read_jp2h(buf, size, &header, &at)) != NULL)
        return hv_fail(error, error_size, "%s at %zu", rule, at);
    status = transcode_boxes(buf, size, ppx, ppy, profile, out, error, error_size);
    if (status == 0 && out->size - out_start > INT_MAX)
        status = hv_fail(error, error_size, "file.size-limit: output larger than INT_MAX bytes");
    if (status != 0) {
        if (error[0] == 0)
            hv_fail(error, error_size, "%s", out->error ? out->error : "out of memory");
        hv_out_rewind(out, out_start);
    }
    return status;
}
