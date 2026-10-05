/* hv_transcode: rewrites JPEG 2000 files for JHelioviewer and the JPIP
 * server, as hvJP2K's jp2_transcode does: the codestream in RPCL order with
 * the given precincts and PLT markers, without recompressing it.
 *
 *   hv_transcode [-p W,H] input output
 *
 *   -p W,H  precinct width and height, powers of 2 from 2 to 32,768
 *           (default 128,128)
 *
 * The input must be a JP2 file that the output can serve: within the JPIP
 * served profile (JPIP_PROFILE.md) except for its tile-parts, which are
 * rewritten. JPX files and raw codestreams are rejected. The boxes are
 * kept: the codestream box is transcoded, a top-level XML box ends before
 * its first NUL, and every other box is copied as read (see
 * hv_transcode_file). The input is read into memory, so a file changed meanwhile cannot
 * crash the tool. The output is written to a temporary file next to it and
 * renamed into place, so input and output may be the same file; it gets the
 * input's permissions (without setuid, setgid and sticky bits), and an
 * output that is a symbolic link is written where the link points. Exit
 * status: 0 on success, 1 on error, 2 on usage errors. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tools/hv_file.h"
#include "jpeg2000/hv_served.h"
#include "jpeg2000/hv_writer.h"
#include "transcode.h"

static void usage(void) {
    fprintf(stderr, "usage: hv_transcode [-p W,H] input output\n");
}

/* The exponent of a power of 2 from 2 to 32,768, or -1. */
static int exponent(const char *s, char **end) {
    long v;
    int e = 0;
    errno = 0;
    v = strtol(s, end, 10);
    if (errno != 0 || *end == s || v < 2 || v > 32768 || (v & (v - 1)) != 0)
        return -1;
    while ((1L << e) < v)
        e++;
    return e;
}

/* Replaces the file `path` with out (hv_file), with permissions `mode`. */
static int write_file(const char *path, const hv_out *out, int mode, char *error,
                      size_t error_size) {
    hv_file f;
    if (hv_file_create(&f, path, mode, error, error_size) != 0)
        return -1;
    if (hv_file_write(&f, out->data, out->size, error, error_size) != 0) {
        hv_file_abort(&f);
        return -1;
    }
    return hv_file_commit(&f, error, error_size);
}

int main(int argc, char **argv) {
    int ppx = 7, ppy = 7, status, i;
    const char *input, *output;
    char error[512];
    struct stat st;
    uint8_t *buf;
    size_t size;
    hv_out out;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *end;
            ppx = exponent(argv[++i], &end);
            if (ppx < 0 || *end != ',' || (ppy = exponent(end + 1, &end)) < 0 || *end) {
                fprintf(stderr, "hv_transcode: -p takes W,H: powers of 2 from 2 to 32,768\n");
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (argc - i != 2) {
        usage();
        return 2;
    }
    input = argv[i];
    output = argv[i + 1];

    /* Read, not mapped: a mapped file that another process truncates
     * raises SIGBUS. At most INT_MAX bytes, as the reader takes, checked
     * before reading. The output takes the input's permissions. */
    if ((buf = hv_load_file(input, INT_MAX, &size)) == NULL) {
        if (errno == EFBIG)
            fprintf(stderr, "hv_transcode: %s: file.size-limit: larger than INT_MAX bytes\n",
                    input);
        else if (errno == EINVAL)
            fprintf(stderr, "hv_transcode: %s: not a regular file\n", input);
        else
            fprintf(stderr, "hv_transcode: cannot open %s: %s\n", input, strerror(errno));
        return 1;
    }
    if (stat(input, &st) != 0) {
        fprintf(stderr, "hv_transcode: cannot open %s: %s\n", input, strerror(errno));
        free(buf);
        return 1;
    }

    hv_out_init(&out);
    status = hv_transcode_file(buf, size, ppx, ppy, HV_OUTPUT_JPIP, &out, error, sizeof error);
    free(buf);
    if (status == 0)
        status = write_file(output, &out, (int)(st.st_mode & 0777), error, sizeof error);
    hv_out_free(&out);
    if (status != 0) {
        fprintf(stderr, "hv_transcode: %s: %s\n", input, error);
        return 1;
    }
    return 0;
}
