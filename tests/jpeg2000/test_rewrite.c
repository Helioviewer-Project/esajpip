/* test_rewrite: the writer against the corpus in tests/vectors/j2k. Every
 * vector the reader reads through (flags 0, or HV_READ_PADDED for
 * one with zero PLT entries) is written again with hv_writer from what
 * the reader decoded (hv_rewrite):
 *   - a vector in the writer's form comes out byte for byte;
 *   - any other (hv_rewrite_result.uncomparable) comes out in the writer's
 *     form: its rewrite reads through with the same flags and rewrites to
 *     itself byte for byte, and it passes every check of the reader the vector
 *     passes (hv_check_served, the header checks).
 * ../../spec/harness/writers.c judges the rewrites with the whole-file
 * model instead of the reader.
 *
 *   test_rewrite <vector directory> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_rewrite.h"
#include "jpeg2000/hv_served.h"

static int failures;

static void fail(const char *name, const char *what) {
    printf("FAIL %s: %s\n", name, what);
    failures++;
}

/* The reader's verdicts on a file: the served profile and the header
 * checks, each nonzero when it passes. */
static void verdicts(const char *path, const uint8_t *buf, size_t size, int jpx, int *served_ok,
                     int *headers_ok) {
    hv_served served;
    size_t at;
    *served_ok = hv_check_served(path, buf, size, jpx, NULL, &served) == NULL;
    *headers_ok = hv_check_headers(buf, size, &at) == NULL;
}

static void check_vector(const char *dir, const char *name, int *same, int *own_form,
                         int *rejected) {
    char path[8192], what[512];
    uint8_t *buf;
    size_t size = 0;
    hv_out out, again;
    hv_rewrite_result r, r2;
    int jpx = hv_is_jpx_name(name), s1, h1, s2, h2;
    unsigned flags = 0;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((buf = hv_load_file(path, SIZE_MAX, &size)) == NULL) {
        fail(name, "cannot read");
        return;
    }
    hv_out_init(&out);
    hv_out_init(&again);
    if (hv_rewrite(buf, size, 0, flags, &out, &r) != 0) {
        hv_out_free(&out);
        hv_out_init(&out);
        flags = HV_READ_PADDED;
        if (hv_rewrite(buf, size, 0, flags, &out, &r) != 0) {
            if (out.error != NULL) {
                snprintf(what, sizeof what, "writer error: %s at %zu", r.error, r.at);
                fail(name, what);
            }
            (*rejected)++;
            goto done;
        }
    }
    if (r.uncomparable == NULL) {
        if (out.size != size || (size != 0 && memcmp(out.data, buf, size) != 0)) {
            size_t i, n = out.size < size ? out.size : size;
            for (i = 0; i < n && out.data[i] == buf[i]; i++)
                ;
            snprintf(what, sizeof what, "rewrite differs at %zu (%zu bytes, input %zu)", i,
                     out.size, size);
            fail(name, what);
        }
        (*same)++;
        goto done;
    }
    (*own_form)++;
    if (hv_rewrite(out.data, out.size, 0, flags, &again, &r2) != 0) {
        snprintf(what, sizeof what, "rewrite (%s) fails: %s at %zu", r.uncomparable, r2.error,
                 r2.at);
        fail(name, what);
        goto done;
    }
    if (r2.uncomparable != NULL || again.size != out.size ||
        (out.size != 0 && memcmp(again.data, out.data, out.size) != 0)) {
        snprintf(what, sizeof what, "rewrite (%s) does not rewrite to itself%s%s",
                 r.uncomparable, r2.uncomparable ? ": " : "",
                 r2.uncomparable ? r2.uncomparable : "");
        fail(name, what);
    }
    verdicts(path, buf, size, jpx, &s1, &h1);
    verdicts(path, out.data, out.size, jpx, &s2, &h2);
    if ((s1 && !s2) || (h1 && !h2)) {
        snprintf(what, sizeof what, "rewrite (%s) fails the %s", r.uncomparable,
                 s1 && !s2 ? "served profile" : "header checks");
        fail(name, what);
    }
done:
    hv_out_free(&out);
    hv_out_free(&again);
    free(buf);
}

/* Failure must preserve the caller's prefix and permit a retry. Deferred
 * PLT cannot provide decoded lengths for canonical rewriting. */
static void check_failure_contract(const char *dir) {
    char path[8192];
    size_t size = 0;
    hv_out out;
    hv_rewrite_result r;
    static const uint8_t prefix[] = {1, 2, 3};
    snprintf(path, sizeof path, "%s/jp2.jp2", dir);
    uint8_t *buf = hv_load_file(path, SIZE_MAX, &size);
    if (buf == NULL) { fail("rewrite contract", "cannot load fixture"); return; }
    hv_out_init(&out);
    if (hv_write_bytes(&out, prefix, sizeof prefix) != 0) {
        fail("rewrite contract", "cannot write prefix");
        goto done;
    }
    static const hv_read_mode incomplete[] = {HV_READ_JPIP_INDEX, HV_READ_PACKETS,
                                              HV_READ_PACKETS_JPIP};
    for (size_t i = 0; i < sizeof incomplete / sizeof *incomplete; i++)
        if (hv_rewrite(buf, size, 0, incomplete[i], &out, &r) == 0 ||
            r.error == NULL || strcmp(r.error, "rewrite requires validation mode") != 0 ||
            out.size != sizeof prefix || memcmp(out.data, prefix, sizeof prefix) != 0)
            fail("rewrite contract", "incomplete interpretation must fail before writing");
    hv_out_rewind(&out, sizeof prefix);
    if (hv_rewrite(buf, size - 1, 0, HV_READ_VALIDATE, &out, &r) == 0 ||
        out.size != sizeof prefix || memcmp(out.data, prefix, sizeof prefix) != 0)
        fail("rewrite contract", "failed rewrite changed prefix or left partial output");
    hv_out_rewind(&out, sizeof prefix);
    if (hv_rewrite(buf, size, 0, HV_READ_VALIDATE, &out, &r) != 0 || out.size != sizeof prefix + size ||
        memcmp(out.data, prefix, sizeof prefix) != 0 ||
        memcmp(out.data + sizeof prefix, buf, size) != 0)
        fail("rewrite contract", "retry did not produce the canonical fixture");
    hv_out_rewind(&out, 0);
    out.error = "earlier write failed";
    if (hv_rewrite(buf, 0, 0, HV_READ_VALIDATE, &out, &r) == 0 || r.error == NULL ||
        strcmp(r.error, out.error) != 0 || out.size != 0)
        fail("rewrite contract", "empty input hid an earlier write failure");
done:
    hv_out_free(&out);
    free(buf);
}

int main(int argc, char **argv) {
    char path[8192], line[4096];
    FILE *manifest;
    int same = 0, own_form = 0, rejected = 0, header = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: test_rewrite <vector directory>\n");
        return 2;
    }
    check_failure_contract(argv[1]);
    snprintf(path, sizeof path, "%s/manifest.tsv", argv[1]);
    if ((manifest = fopen(path, "r")) == NULL) {
        printf("FAIL cannot read %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, manifest) != NULL) {
        char *tab = strchr(line, '\t');
        if (header) {
            header = 0;
            continue;
        }
        if (tab == NULL) {
            fail(line, "manifest row without fields");
            continue;
        }
        *tab = 0;
        check_vector(argv[1], line, &same, &own_form, &rejected);
    }
    fclose(manifest);
    /* The four bases, and most of the corpus, are in the writer's form. */
    if (same < 100)
        fail("corpus", "fewer than 100 vectors rewritten byte for byte");
    printf("%d vectors rewritten byte for byte, %d into the writer's form, %d rejected by "
           "the reader\n", same, own_form, rejected);
    if (failures != 0) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
