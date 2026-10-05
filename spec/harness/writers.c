/* writers.c — the writer of ../../jpeg2000 judged by the model: every file the
 * library writes from the corpus is labeled at both layers (label.c), with
 * the whole-file decoders instead of the reader that ../../jpeg2000/test uses.
 *
 * Usage:  writers <corpus-directory> [<seed> <mutants-per-vector> <scratch-directory>]
 *
 * For every vector of the corpus's manifest:
 *   - hv_rewrite, where the reader reads it through (flags 0, or
 *     HV_READ_PADDED): where the vector is in the writer's form,
 *     the rewrite is the vector byte for byte (jpeg2000/test/test_rewrite.c
 *     checks that too); otherwise (hv_rewrite_result.uncomparable) it has
 *     labels no worse than the vector's, layer by layer, and is in the
 *     writer's form: hv_rewrite gives it back byte for byte.
 *   - hv_transcode_file, where it accepts a .jp2 (64 x 64 and 128 x 128
 *     precincts): the output is valid at layer 2, and at layer 1 where the
 *     vector is (the transcoder keeps the header boxes and main-header
 *     segments it does not rewrite), and in the writer's form: hv_rewrite
 *     gives it back byte for byte.
 *   - hv_merge_buffers of jp2.jp2 and the vector, where it accepts a .jp2,
 *     with the codestreams embedded and linked: the output is valid at each
 *     layer where the vector is (hv_merge copies the codestreams; the
 *     linked output is checked against the two inputs' codestreams,
 *     measured here, as companions), and in the writer's form where the
 *     vector is.
 * With a seed, each vector is also mutated at random that many times, each
 * mutant checked in the same way (a differential fuzz of the writer; the
 * linked merge writes the mutant to the scratch directory).
 *
 * Build: spec/check-model.sh compiles this file with label.c,
 * crossfield.c, mapping.c, ../../jpeg2000's sources, ../../transcode's and
 * ../../merge's library sources and the generated C of the four modules;
 * its command is the reference. */
#define _XOPEN_SOURCE 700

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_rewrite.h"
#include "jpeg2000/hv_served.h"
#include "label.h"
#include "merge/merge.h"
#include "transcode/transcode.h"

static int failures, checked, rewritten, transcoded, merged, companion_labels;

static void fail(const char *name, const char *what, const char *detail) {
    printf("FAIL %s: %s%s%s\n", name, what, detail ? ": " : "", detail ? detail : "");
    failures++;
}

static int same_bytes(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static const char *describe(Label l, char *out, size_t size) {
    snprintf(out, size, "%s/%s", l.std_ok ? "valid" : l.std_reason,
             l.prof_ok ? "valid" : l.prof_reason);
    return out;
}

/* A file lib wrote must be in the writer's form: hv_rewrite gives it back
 * byte for byte. */
static void check_own_form(const char *name, const char *what, const uint8_t *buf,
                           size_t size, unsigned flags) {
    hv_out out;
    hv_rewrite_result r;
    char detail[512];
    hv_out_init(&out);
    if (hv_rewrite(buf, size, 0, flags, &out, &r) != 0) {
        snprintf(detail, sizeof detail, "%s at %zu", r.error, r.at);
        fail(name, what, detail);
    } else if (r.uncomparable != NULL) {
        fail(name, what, r.uncomparable);
    } else if (!same_bytes(out.data, out.size, buf, size)) {
        fail(name, what, "hv_rewrite does not give it back byte for byte");
    }
    hv_out_free(&out);
}

/* A written file: valid at layer 1 where its input is, and at layer 2
 * always, or with `as_input` where its input is. */
static void check_valid(const char *name, const char *what, const uint8_t *buf, size_t size,
                        cf_kind kind, Label input, int as_input) {
    char detail[512];
    Label l = label(buf, size, kind);
    checked++;
    if ((input.std_ok && !l.std_ok) || (!l.prof_ok && (!as_input || input.prof_ok)))
        fail(name, what, describe(l, detail, sizeof detail));
}

/* Nonzero when the input is in the writer's form. */
static int check_rewrite(const char *name, const uint8_t *buf, size_t size, cf_kind kind,
                         Label before) {
    hv_out out;
    hv_rewrite_result r;
    char detail[768], was[256], now[256];
    unsigned flags = 0;
    hv_out_init(&out);
    if (hv_rewrite(buf, size, 0, flags, &out, &r) != 0) {
        hv_out_free(&out);
        hv_out_init(&out);
        flags = HV_READ_PADDED;
        if (hv_rewrite(buf, size, 0, flags, &out, &r) != 0) {
            if (out.error != NULL)
                fail(name, "rewrite: writer error", r.error);
            hv_out_free(&out);
            return 0;
        }
    }
    rewritten++;
    if (r.uncomparable == NULL) {
        if (!same_bytes(out.data, out.size, buf, size))
            fail(name, "rewrite", "differs from the input, which is in the writer's form");
    } else {
        Label after = label(out.data, out.size, kind);
        checked++;
        if ((before.std_ok && !after.std_ok) || (before.prof_ok && !after.prof_ok)) {
            snprintf(detail, sizeof detail, "%s: %s, the input %s", r.uncomparable,
                     describe(after, now, sizeof now), describe(before, was, sizeof was));
            fail(name, "rewrite labels worse than the input's", detail);
        }
        check_own_form(name, "rewrite", out.data, out.size, flags);
    }
    hv_out_free(&out);
    return r.uncomparable == NULL;
}

static void check_transcode(const char *name, const uint8_t *buf, size_t size, Label input) {
    static const int precincts[] = {6, 7};
    char error[256], what[64];
    size_t i;
    for (i = 0; i < sizeof precincts / sizeof *precincts; i++) {
        hv_out out;
        hv_out_init(&out);
        if (hv_transcode_file(buf, size, precincts[i], precincts[i], HV_OUTPUT_JPIP, &out, error,
                              sizeof error) == 0) {
            transcoded++;
            snprintf(what, sizeof what, "transcode %d", 1 << precincts[i]);
            check_valid(name, what, out.data, out.size, CF_JP2, input, 0);
            check_own_form(name, what, out.data, out.size, 0);
        }
        hv_out_free(&out);
    }
}

/* The codestream box's payload of a .jp2, found by its top-level boxes
 * (short headers; the files here have them). 0 if there is none. */
static int jp2c_extent(const uint8_t *buf, size_t size, uint64_t *offset, uint32_t *length) {
    size_t pos = 0;
    while (size - pos >= 8) {
        uint32_t lbox = (uint32_t) buf[pos] << 24 | (uint32_t) buf[pos + 1] << 16 |
                        (uint32_t) buf[pos + 2] << 8 | buf[pos + 3];
        uint32_t tbox = (uint32_t) buf[pos + 4] << 24 | (uint32_t) buf[pos + 5] << 16 |
                        (uint32_t) buf[pos + 6] << 8 | buf[pos + 7];
        if (lbox < 8 || lbox > size - pos)
            return 0;
        if (tbox == HV_BOX_JP2C) {
            *offset = pos + 8;
            *length = lbox - 8;
            return 1;
        }
        pos += lbox;
    }
    return 0;
}

/* A vector's companions (the manifest's column), as the corpus names them:
 * file://./<name>, with their codestreams measured. The URLs are kept in
 * urls, which the caller frees. */
static void register_companions(const char *dir, char *column, char **urls, int max) {
    char *name, *end;
    int n = 0;
    label_companions_clear();
    for (name = column; *name && strcmp(name, "-") != 0 && n < max; name = end) {
        char path[8192];
        uint8_t *buf;
        size_t size = 0, len;
        uint64_t offset;
        uint32_t length;
        end = name + strcspn(name, ",");
        len = (size_t) (end - name);
        if (*end == ',')
            *end++ = 0;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if ((buf = hv_load_file(path, SIZE_MAX, &size)) == NULL || !jp2c_extent(buf, size, &offset, &length) ||
            (urls[n] = malloc(len + sizeof "file://./")) == NULL) {
            fail(name, "companion", "cannot read its codestream");
            free(buf);
            continue;
        }
        snprintf(urls[n], len + sizeof "file://./", "file://./%s", name);
        label_companion(urls[n++], offset, length);
        free(buf);
    }
}

/* The bytes of f, from its start. */
static uint8_t *slurp(FILE *f, size_t *size) {
    long n;
    uint8_t *buf;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (buf = malloc((size_t) n + 1)) == NULL)
        return NULL;
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        return NULL;
    }
    *size = (size_t) n;
    return buf;
}

/* The URL of the k-th url box of a linked JPX (their order is the inputs'
 * in hv_merge), NUL-terminated in buf. */
static const char *url_loc(const uint8_t *buf, size_t size, int k) {
    size_t pos;
    for (pos = 0; pos + 12 <= size; pos++)
        if (memcmp(buf + pos + 4, "url ", 4) == 0 && k-- == 0)
            return (const char *) buf + pos + 12;
    return NULL;
}

static void check_merge(const char *name, const char *first_path, const uint8_t *first,
                        size_t first_size, const char *path, const uint8_t *buf, size_t size,
                        Label input, int own_form) {
    hv_merge_input in[2];
    char error[512];
    int links;
    in[0].path = first_path;
    in[0].buf = first;
    in[0].size = first_size;
    in[1].path = path;
    in[1].buf = buf;
    in[1].size = size;
    for (links = 0; links <= 1; links++) {
        const char *what = links ? "merge, linked" : "merge, embedded";
        FILE *f = tmpfile();
        uint8_t *jpx;
        size_t jpx_size = 0;
        if (f == NULL) {
            fail(name, what, "no temporary file");
            return;
        }
        if (hv_merge_buffers(in, 2, links, HV_OUTPUT_JPIP, f, error, sizeof error) != 0) {
            fclose(f);
            return;                     /* not an input hv_merge takes */
        }
        if ((jpx = slurp(f, &jpx_size)) == NULL) {
            fclose(f);
            fail(name, what, "cannot read the output back");
            return;
        }
        fclose(f);
        merged++;
        label_companions_clear();
        if (links) {
            int i;
            for (i = 0; i < 2; i++) {
                const char *url = url_loc(jpx, jpx_size, i), *base = strrchr(in[i].path, '/');
                uint64_t offset;
                uint32_t length;
                size_t n = strlen(url ? url : ""), b = strlen(base ? base : "");
                if (url == NULL || base == NULL || n < b || strcmp(url + n - b, base) != 0 ||
                    !jp2c_extent(in[i].buf, in[i].size, &offset, &length)) {
                    fail(name, what, "no url naming the input in input order");
                    label_companions_clear();
                    free(jpx);
                    return;
                }
                label_companion(url, offset, length);
            }
        }
        check_valid(name, what, jpx, jpx_size, CF_JPX, input, 1);
        label_companions_clear();
        if (own_form)
            check_own_form(name, what, jpx, jpx_size, 0);
        free(jpx);
    }
}

static uint64_t rng_state;

static uint32_t rnd(void) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t) (rng_state >> 33);
}

/* One random change to m[0, n), whose capacity is n + 1: a flipped bit, a
 * random, 0x00 or 0xFF byte, a byte or a 32-bit field one up or down, a
 * byte deleted or inserted, or the file cut short. The new size. */
static size_t mutate(uint8_t *m, size_t n) {
    size_t at = n ? rnd() % n : 0;
    uint32_t v;
    switch (rnd() % 8) {
    case 0: if (n) m[at] ^= (uint8_t) (1u << (rnd() % 8)); break;
    case 1: if (n) m[at] = (uint8_t) rnd(); break;
    case 2: if (n) m[at] = rnd() % 2 ? 0x00 : 0xFF; break;
    case 3: if (n) m[at] = (uint8_t) (m[at] + (rnd() % 2 ? 1 : -1)); break;
    case 4: if (n) { memmove(m + at, m + at + 1, n - at - 1); n--; } break;
    case 5: memmove(m + at + 1, m + at, n - at); m[at] = (uint8_t) rnd(); n++; break;
    case 6: n = at; break;
    default:
        if (at + 4 <= n) {
            v = (uint32_t) m[at] << 24 | (uint32_t) m[at + 1] << 16 | (uint32_t) m[at + 2] << 8 |
                m[at + 3];
            v += rnd() % 2 ? 1 : (uint32_t) -1;
            m[at] = (uint8_t) (v >> 24);
            m[at + 1] = (uint8_t) (v >> 16);
            m[at + 2] = (uint8_t) (v >> 8);
            m[at + 3] = (uint8_t) v;
        }
        break;
    }
    return n;
}

static void check_file(const char *name, const char *path, const uint8_t *buf, size_t size,
                       const char *first_path, const uint8_t *first, size_t first_size) {
    int jpx = hv_is_jpx_name(name), own_form;
    Label input = label(buf, size, jpx ? CF_JPX : CF_JP2);
    companion_labels += jpx && input.prof_ok;
    own_form = check_rewrite(name, buf, size, jpx ? CF_JPX : CF_JP2, input);
    if (!jpx) {
        check_transcode(name, buf, size, input);
        check_merge(name, first_path, first, first_size, path, buf, size, input, own_form);
    }
}

int main(int argc, char **argv) {
    char path[8192], first_path[8192], line[4096];
    FILE *manifest;
    uint8_t *first;
    size_t first_size = 0;
    unsigned long seed = 0, per = 0, i;
    int header = 1, vectors = 0;

    if (argc != 2 && argc != 5) {
        fprintf(stderr, "usage: writers <corpus-directory> [<seed> <mutants-per-vector> "
                        "<scratch-directory>]\n");
        return 2;
    }
    if (argc == 5) {
        seed = strtoul(argv[2], NULL, 10);
        per = strtoul(argv[3], NULL, 10);
    }
    label_init();
    snprintf(first_path, sizeof first_path, "%s/jp2.jp2", argv[1]);
    if ((first = hv_load_file(first_path, SIZE_MAX, &first_size)) == NULL) {
        printf("FAIL cannot read %s\n", first_path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/manifest.tsv", argv[1]);
    if ((manifest = fopen(path, "r")) == NULL) {
        printf("FAIL cannot read %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, manifest) != NULL) {
        char *tab = strchr(line, '\t'), *field = tab, *urls[16] = {NULL};
        uint8_t *buf;
        size_t size = 0;
        int k;
        if (header || tab == NULL) {
            header = 0;
            continue;
        }
        *tab = 0;
        for (k = 1; k < 7 && field != NULL; k++)        /* to the companions column */
            field = strchr(field + 1, '\t');
        if (field != NULL) {
            field[strcspn(field + 1, "\t\n") + 1] = 0;
            register_companions(argv[1], field + 1, urls, 16);
        }
        snprintf(path, sizeof path, "%s/%s", argv[1], line);
        if ((buf = hv_load_file(path, SIZE_MAX, &size)) == NULL) {
            fail(line, "cannot read", NULL);
            continue;
        }
        vectors++;
        check_file(line, path, buf, size, first_path, first, first_size);
        rng_state = seed * 1000003ull + (uint64_t) vectors;
        for (i = 0; i < per; i++) {
            char name[4096 + 32], mutant_path[8192];
            uint8_t *m = malloc(size + 2);
            size_t n = size;
            FILE *f;
            int before = failures;
            if (m == NULL) {
                fail(line, "out of memory", NULL);
                break;
            }
            memcpy(m, buf, size);
            n = mutate(m, n);
            snprintf(name, sizeof name, "%s#%lu", line, i);
            /* The linked merge records the mutant's path: write it there. */
            snprintf(mutant_path, sizeof mutant_path, "%s/mutant.jp2", argv[4]);
            if ((f = fopen(mutant_path, "wb")) == NULL || fwrite(m, 1, n, f) != n ||
                fclose(f) != 0) {
                fail(name, "cannot write", mutant_path);
                free(m);
                break;
            }
            check_file(name, mutant_path, m, n, first_path, first, first_size);
            if (failures > before) {
                /* Keep the mutant that failed. */
                char kept[8192 + 64];
                snprintf(kept, sizeof kept, "%s/failed-%s-%lu", argv[4], line, i);
                rename(mutant_path, kept);
            }
            free(m);
        }
        free(buf);
        label_companions_clear();
        for (k = 0; k < 16; k++)
            free(urls[k]);
    }
    fclose(manifest);
    free(first);
    printf("writers: %d vectors%s: %d rewritten, %d transcoded, %d merged; %d written "
           "files labeled (%d profile-valid JPX inputs)\n", vectors,
           per ? " and their mutants" : "", rewritten, transcoded, merged, checked,
           companion_labels);
    if (failures != 0) {
        printf("writers: %d failures\n", failures);
        return 1;
    }
    printf("writers: every written file has the labels expected\n");
    return 0;
}
