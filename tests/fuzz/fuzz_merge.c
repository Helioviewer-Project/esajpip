/* fuzz_merge: a libFuzzer target for hv_merge_buffers. The input is two JP2
 * files: a 4-byte big-endian length L, the first L bytes (modulo what
 * remains), then the second file. They are merged in that order, so an input
 * with a palette and one without exercise the generated cmap.
 *
 * Each input is merged four times: embedded and linked, each in default and
 * in validation mode. Every accepted merge must have a valid container and
 * header boxes (hv_served_jpx, hv_check_jpx_headers). An embedded one holds
 * the inputs' codestreams, discovered by hv_read_jp2; in validation mode
 * the input containers pass hv_served_jp2 and codestreams pass HV_READ_JPIP. A
 * linked one holds one link per input, in order, that resolves
 * (hv_served_path) to that input's file and names exactly its codestream; in
 * validation mode the linked file passes hv_served_link.
 *
 * The linked JPX file records the inputs' paths, so the two inputs are
 * written to files in a directory of their own, made once and removed at
 * exit.
 *
 *   cmake -S . -B fuzz -DCMAKE_C_COMPILER=clang -DESAJPIP_SANITIZE=ON -DESAJPIP_FUZZ=ON
 *   cmake --build fuzz --target fuzz_merge
 *   fuzz/tests/fuzz/fuzz_merge -max_len=262144 corpus/ */
#define _XOPEN_SOURCE 700   /* mkdtemp, realpath; replay.c defines it too */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "jpeg2000/hv_served.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jpeg2000/hv_reader.h"
#include "merge/merge.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* The merged file, read back from the stream. */
static uint8_t *read_back(FILE *f, size_t *size) {
    long n;
    uint8_t *buf;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (buf = malloc(n ? (size_t)n : 1)) == NULL)
        return NULL;
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        return NULL;
    }
    *size = (size_t)n;
    return buf;
}

/* The inputs' files: merge_dir/first.jp2 and merge_dir/second.jp2. */
static char merge_dir[PATH_MAX], merge_paths[2][PATH_MAX + 16];

static void remove_merge_dir(void) {
    unlink(merge_paths[0]);
    unlink(merge_paths[1]);
    rmdir(merge_dir);
}

static int make_merge_dir(void) {
    const char *tmp = getenv("TMPDIR");
    if (merge_dir[0] != 0)
        return 0;
    snprintf(merge_dir, sizeof merge_dir, "%s/fuzz_merge.XXXXXX",
             tmp != NULL && tmp[0] != 0 ? tmp : "/tmp");
    if (mkdtemp(merge_dir) == NULL) {
        merge_dir[0] = 0;
        return -1;
    }
    snprintf(merge_paths[0], sizeof merge_paths[0], "%s/first.jp2", merge_dir);
    snprintf(merge_paths[1], sizeof merge_paths[1], "%s/second.jp2", merge_dir);
    atexit(remove_merge_dir);
    return 0;
}

static int write_input(const char *path, const uint8_t *buf, size_t size) {
    FILE *f = fopen(path, "wb");
    int status;
    if (f == NULL)
        return -1;
    status = (size == 0 || fwrite(buf, 1, size, f) == size) ? 0 : -1;
    if (fclose(f) != 0)
        status = -1;
    return status;
}

/* A linked merge: link i names input i's file and exactly its codestream. */
static void check_links(const hv_served_sources *j, const hv_merge_input *in, int validate) {
    char path[PATH_MAX + 16], *real, *expected;
    hv_box jp2c;
    size_t i, at;
    if (j->links == NULL || j->count != 2)
        abort();
    for (i = 0; i < 2; i++) {
        if (hv_served_path(&j->links[i], NULL, path, sizeof path) != NULL ||
            (real = realpath(path, NULL)) == NULL)
            abort();
        if ((expected = realpath(in[i].path, NULL)) == NULL || strcmp(real, expected) != 0)
            abort();
        free(expected);
        free(real);
        if (hv_served_jp2(in[i].buf, in[i].size, &jp2c, &at) != NULL ||
            j->links[i].offset != jp2c.payload || j->links[i].length != jp2c.end - jp2c.payload)
            abort();
        if (validate && hv_served_link(in[i].buf, in[i].size, &j->links[i], &at) != NULL)
            abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hv_merge_input in[2];
    char error[512];
    FILE *f;
    uint8_t *jpx;
    size_t n, at, i;
    int links, validate;
    hv_served_sources j;

    if (size < 4)
        return 0;
    if (make_merge_dir() != 0) {
        perror("fuzz_merge: create input directory");
        abort();
    }
    n = ((size_t)data[0] << 24 | (size_t)data[1] << 16 | (size_t)data[2] << 8 | data[3]) %
        (size - 4 + 1);
    in[0].path = merge_paths[0];
    in[0].buf = data + 4;
    in[0].size = n;
    in[1].path = merge_paths[1];
    in[1].buf = data + 4 + n;
    in[1].size = size - 4 - n;
    if (write_input(in[0].path, in[0].buf, in[0].size) != 0 ||
        write_input(in[1].path, in[1].buf, in[1].size) != 0) {
        perror("fuzz_merge: write input files");
        abort();
    }
    for (links = 0; links < 2; links++)
        for (validate = 0; validate < 2; validate++) {
            if ((f = tmpfile()) == NULL) {
                perror("fuzz_merge: create output file");
                abort();
            }
            if (hv_merge_buffers(in, 2, links, validate ? HV_OUTPUT_JPIP : HV_OUTPUT_JPEG2000, f, error, sizeof error) == 0) {
                if ((jpx = read_back(f, &n)) == NULL ||
                    hv_check_jpx_headers(jpx, n, &at) != NULL ||
                    hv_served_jpx(jpx, n, &j, &at) != NULL)
                    abort();
                if (links) {
                    check_links(&j, in, validate);
                } else {
                    if (j.jp2c == NULL || j.count != 2)
                        abort();
                    for (i = 0; i < j.count; i++) {
                        hv_box source;
                        size_t length = j.jp2c[i].end - j.jp2c[i].payload;
                        const char *rule = validate
                            ? hv_served_jp2(in[i].buf, in[i].size, &source, &at)
                            : hv_read_jp2(in[i].buf, in[i].size, &source, &at);
                        if (rule != NULL ||
                            source.end - source.payload != length ||
                            memcmp(jpx + j.jp2c[i].payload, in[i].buf + source.payload,
                                   length) != 0)
                            abort();
                        if (validate && hv_codestream_check(jpx, j.jp2c[i].payload,
                                                            j.jp2c[i].end, HV_READ_JPIP,
                                                            &at) != NULL)
                            abort();
                    }
                }
                hv_served_sources_free(&j);
                free(jpx);
            }
            if (ferror(f)) {
                perror("fuzz_merge: output stream");
                abort();
            }
            if (fclose(f) != 0) {
                perror("fuzz_merge: close output file");
                abort();
            }
        }
    return 0;
}
