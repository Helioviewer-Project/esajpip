/* hv_walk: checks JPEG 2000 files with hv_reader and prints one line per
 * file: "valid", "invalid" with the error and its offset, "differs" when
 * the file is valid but -w rewrote it differently, or "ERROR" when it
 * cannot be read. Exit status: 0 if every file is valid (and, with -w,
 * rewritten identically), 1 otherwise, 2 on usage errors.
 *
 *   -v  print every box and codestream item
 *   -p  accept trailing zero PLT entries of each tile-part
 *       (HV_READ_PADDED), as deployed files carry them; not with -P,
 *       whose profile accepts them after the last packet only
 *   -P  check the served profile (hv_check_served): the file rules, and
 *       HV_READ_JPIP for every codestream, embedded or in a linked file
 *   -H  check the box structure and the header boxes at the standard layer
 *       (hv_check_headers: hv_check_jpx_headers for the ftyp brand 'jpx ',
 *       hv_check_jp2h for any other), as the tools do for the files they
 *       write; alone, -H leaves codestream bodies opaque (-P or -w adds
 *       codestream validation)
 *   -R  (-R dir, with -P) linked files must lie inside the directory dir
 *   -w  rewrite the whole file with hv_writer from what the reader decoded
 *       (hv_rewrite), and compare it with the input
 *
 * For -P, a file whose name ends in .jpx, in any case, is a JPX file, as
 * the server decides; any other is a JP2 file, or, if it starts with SOC,
 * a raw codestream. Without -P and -H a raw codestream is read as such; -P
 * and -H check files, and fail it with file.signature. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv_reader.h"
#include "hv_rewrite.h"
#include "hv_served.h"

/* A marker code (T.800 A.1). */
#define MARKER HV_FIXED(MarkerCode)

static int verbose, rewrite, profile, headers;
static const char *root;
static hv_read_mode mode;
static int padding;

static void fourcc(uint32_t t, char out[5]) {
    int i;
    for (i = 0; i < 4; i++) {
        char c = (char)(t >> (24 - 8 * i));
        out[i] = c >= 32 && c < 127 ? c : '.';
    }
    out[4] = 0;
}

typedef struct {
    int boxes, codestreams, tile_parts;
    unsigned long plt_padding;
    const char *error;
    size_t at;
    char message[64];       /* error, when it is formatted here */
} walk_result;

static int walk_codestream(const uint8_t *buf, size_t start, size_t end, walk_result *r) {
    hv_codestream cs;
    hv_item item;
    int status = hv_codestream_open(&cs, buf, start, end, mode);

    if (status == 0) {
        while ((status = hv_codestream_next(&cs, &item)) == 1) {
            static const char *kinds[] = {"segment", "tile-part", "tile-segment", "data", "end"};
            if (item.kind == HV_TILE_PART)
                r->tile_parts++;
            if (item.kind == HV_TILE_DATA)
                r->plt_padding += item.plt_padding;
            if (verbose)
                printf("    %-12s %04X [%zu, %zu)\n", kinds[item.kind], item.code, item.start,
                       item.end);
        }
    }
    if (status == -1) {
        r->error = cs.error;
        r->at = cs.error_at;
    } else {
        r->codestreams++;
    }
    hv_codestream_close(&cs);
    return status < 0 ? -1 : 0;
}

/* header_kind: 0 for codestream inspection, 1 for JP2 headers, 2 for JPX. */
static int walk_boxes(const uint8_t *buf, hv_boxes *it, int depth, int header_kind,
                      walk_result *r) {
    hv_box box;
    int status;
    char name[5];

    while ((status = hv_boxes_next(it, &box, &r->error, &r->at)) == 1) {
        r->boxes++;
        if (verbose) {
            fourcc(box.type, name);
            printf("  %*s%s [%zu, %zu)%s\n", 2 * depth, "", name, box.start, box.end,
                   box.to_end ? " to end" : "");
        }
        if (box.type == HV_BOX_JP2C) {
            if (header_kind) r->codestreams++;
            else if (walk_codestream(buf, box.payload, box.end, r) != 0)
                return -1;
        } else if (hv_is_superbox(box.type) &&
                   (header_kind != 1 || box.type == HV_BOX_JP2H ||
                    box.type == HV_BOX_RES || box.type == HV_BOX_UINF)) {
            hv_boxes children;
            if (depth + 1 > HV_BOX_DEPTH_MAX) {       /* depth 0 is the top level */
                snprintf(r->message, sizeof r->message, "superboxes nested deeper than %d",
                         HV_BOX_DEPTH_MAX);
                r->error = r->message;
                r->at = box.start;
                return -1;
            }
            hv_boxes_children(&children, buf, &box);
            if (walk_boxes(buf, &children, depth + 1, header_kind, r) != 0)
                return -1;
        }
    }
    return status < 0 ? -1 : 0;
}

/* Nonzero when the file starts with SOC: a raw codestream. */
static int raw_codestream(const uint8_t *buf, size_t size) {
    return size >= MARKER && buf[0] == HV_SOC >> 8 && buf[1] == (HV_SOC & 0xFF);
}

static int walk_file(const char *path) {
    uint8_t *buf;
    size_t size = 0, linked = 0, i, n;
    walk_result r;
    hv_served served;
    hv_out out;
    hv_rewrite_result rewritten = {NULL, 0, NULL};
    int status, jpx = hv_is_jpx_name(path), raw;

    memset(&r, 0, sizeof r);
    served.linked_path[0] = 0;
    served.at_linked = 0;
    hv_out_init(&out);
    if ((buf = hv_load_file(path, INT_MAX, &size)) == NULL) {
        if (errno == EFBIG)
            printf("invalid %s: file.size-limit at 0\n", path);
        else
            printf("ERROR   %s: cannot read\n", path);
        return -1;
    }
    raw = raw_codestream(buf, size);
    if (profile) {
        r.error = hv_check_served(path, buf, size, jpx, root, &served);
        r.at = served.at;
        linked = served.linked;
    }
    if (headers && r.error == NULL) {
        if (raw) {
            r.error = "file.signature";
            r.at = 0;
        } else {
            r.error = hv_check_headers(buf, size, &r.at);
        }
    }
    if (r.error != NULL) {
        status = -1;
    } else if (raw) {
        status = walk_codestream(buf, 0, size, &r);
    } else {
        hv_boxes it;
        int header_kind = 0;
        if (headers && !profile && !rewrite) {
            hv_ftyp type;
            r.error = hv_container_open(buf, size, &it, &type, &r.at);
            if (r.error == NULL) header_kind = hv_ftyp_is_jpx(&type) ? 2 : 1;
        }
        hv_boxes_file(&it, buf, size);
        status = r.error ? -1 : walk_boxes(buf, &it, 0, header_kind, &r);
    }
    if (status == 0 && rewrite && hv_rewrite(buf, size, raw, mode, &out, &rewritten) != 0) {
        r.error = rewritten.error;
        r.at = rewritten.at;
        status = -1;
    }
    if (status != 0) {
        if (served.linked_path[0] == 0)
            printf("invalid %s: %s at %zu\n", path, r.error, r.at);
        else if (served.at_linked)
            printf("invalid %s: %s at %zu of %s\n", path, r.error, r.at, served.linked_path);
        else
            printf("invalid %s: %s at %zu (linked file %s)\n", path, r.error, r.at,
                   served.linked_path);
    } else if (rewrite && rewritten.uncomparable == NULL &&
               (out.size != size || (size != 0 && memcmp(out.data, buf, size) != 0))) {
        n = out.size < size ? out.size : size;
        for (i = 0; i < n && out.data[i] == buf[i]; i++)
            ;
        printf("differs %s: valid, but the rewrite differs at %zu\n", path, i);
        status = -1;
    } else {
        printf("valid   %s: %d boxes, %d codestreams, %d tile-parts", path, r.boxes,
               r.codestreams, r.tile_parts);
        if (linked != 0)
            printf(", %zu linked codestreams", linked);
        if (r.plt_padding != 0)
            printf(", %lu zero PLT entries", r.plt_padding);
        if (headers && !profile && !rewrite)
            printf("; headers only");
        if (rewrite && rewritten.uncomparable != NULL)
            printf("; rewrite not compared: %s", rewritten.uncomparable);
        else if (rewrite)
            printf("; rewritten identically");
        printf("\n");
    }
    hv_out_free(&out);
    free(buf);
    return status;
}

int main(int argc, char **argv) {
    int i = 1, bad = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && !argv[i][2]; i++) {
        if (argv[i][1] == 'v')
            verbose = 1;
        else if (argv[i][1] == 'p')
            padding = 1;
        else if (argv[i][1] == 'P') {
            profile = 1;
        }
        else if (argv[i][1] == 'H')
            headers = 1;
        else if (argv[i][1] == 'w')
            rewrite = 1;
        else if (argv[i][1] == 'R' && i + 1 < argc)
            root = argv[++i];
        else
            break;
    }
    if (i == argc || argv[i][0] == '-' || (padding && profile) ||
        (root != NULL && !profile)) {
        fprintf(stderr, "usage: hv_walk [-v] [-p] [-P [-R dir]] [-H] [-w] file...\n");
        return 2;
    }
    mode = profile ? HV_READ_JPIP : padding ? HV_READ_PADDED : HV_READ_VALIDATE;
    for (; i < argc; i++)
        bad |= walk_file(argv[i]) != 0;
    return bad;
}
