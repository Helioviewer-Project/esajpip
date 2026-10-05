/* test_profile: the reader's checks against the corpus in
 * tests/vectors/j2k, and unit checks of the reader and rules.
 *
 * Served profile. The reader and the model's harness share the rules
 * (jpeg2000/hv_rules.c), so the reader's profile checks (hv_check_served:
 * hv_served_jp2 or hv_served_jpx, then every codestream with HV_READ_JPIP,
 * embedded or in the linked files) must accept a vector exactly when the
 * manifest labels it profile-valid, and (e) reject it by the rule the
 * manifest's profile_reason names, where it names one (not `decode`).
 *
 * Header boxes. hv_check_headers (hv_check_jp2h or hv_check_jpx_headers,
 * by the ftyp brand) checks the standard layer, but only part of it: the
 * box tree, the header boxes, the JPX count rules, the fragments in the
 * file, and each codestream's SIZ against its header. The contract, taken
 * from the manifest's labels and the checks' own answers, not from vector
 * names:
 *   (a) they accept every standard-valid vector;
 *   (b) a vector they reject is standard-invalid, by the rule they name
 *       (any error where the manifest's reason is "decode");
 *   (c) a header rule they name for one vector they name for every vector
 *       the manifest gives that reason (a codestream rule they name is the
 *       reader's error in a main header, which (f) checks);
 *   (d) they reject every vector that is standard-invalid but
 *       profile-valid: the profile keeps the header boxes and the JP2
 *       file's other boxes opaque, so those are the rules of the header
 *       checks, except for the codestream rules the profile does not
 *       apply, which (f) checks (lenient).
 *
 * Codestreams. (f) hv_codestream_check without flags (the standard
 * layer) accepts every jp2c of a standard-valid vector, and rejects one
 * by the manifest's reason when that is a codestream rule
 * (codestream_rule). Header checks interpret SIZ, JP2 MCT and framing only;
 * rejection of an opaque body by (f) need not reject its header boxes.
 *
 * The corpus is checked with the vector directory as the root that linked
 * files must lie in (hv_check_served).
 *
 *   test_profile <vector directory> */
#define _XOPEN_SOURCE 700       /* mkdtemp, mkfifo, symlink, realpath */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1       /* mkdtemp: Darwin hides it under _XOPEN_SOURCE */
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpeg2000/hv_geometry.h"

#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_served.h"

static int failures;

static void check(int ok, const char *what, const char *detail) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
        failures++;
    }
}

/* NULL if the JP2 file passes hv_served_jp2 and its codestream reads
 * through with `flags`, otherwise the error and its offset. */
static const char *profile_error(const uint8_t *buf, size_t size, unsigned flags) {
    static char message[256];
    hv_box jp2c;
    size_t at = 0;
    const char *error = hv_served_jp2(buf, size, &jp2c, &at);
    if (error == NULL)
        error = hv_codestream_check(buf, jp2c.payload, jp2c.end, flags, &at);
    if (error == NULL)
        return NULL;
    snprintf(message, sizeof message, "%s at %zu", error, at);
    return message;
}

static uint8_t *vector(const char *dir, const char *name, size_t *size) {
    char path[4096];
    uint8_t *buf;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((buf = hv_load_file(path, SIZE_MAX, size)) == NULL)
        check(0, "cannot read", path);
    return buf;
}

/* A malloc'd copy of s (strdup is not ISO C). */
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = malloc(n);
    return c ? memcpy(c, s, n) : NULL;
}

/* One manifest row, and what the header checks said of it. */
typedef struct {
    char *name, *standard, *profile, *reason, *field;
    const char *header;         /* the header checks' error, or NULL */
    size_t at;
} row;

/* The first embedded codestream the reader rejects at the standard layer
 * (hv_codestream_check, flags 0), or NULL; a framing error of the boxes
 * too. */
static const char *standard_codestreams(const uint8_t *buf, size_t size, size_t *at) {
    hv_boxes it;
    hv_box box;
    const char *error = NULL;
    int status;
    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &error, at)) == 1)
        if (box.type == HV_BOX_JP2C &&
            (error = hv_codestream_check(buf, box.payload, box.end, HV_READ_VALIDATE, at)) != NULL)
            return error;
    return status < 0 ? error : NULL;
}

/* A rule of the codestream (T.800 Annex A), by its name. */
static int codestream_rule(const char *reason) {
    static const char *const prefixes[] = {"siz.", "cod.", "coc.", "qcd.", "qcc.", "rgn.",
                                           "poc.", "tlm.", "plm.", "plt.", "ppm.", "ppt.",
                                           "crg.", "sot.", "sop.", "tile.", "codestream.",
                                           "main."};
    size_t i;
    for (i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (strncmp(reason, prefixes[i], strlen(prefixes[i])) == 0)
            return 1;
    return 0;
}

/* The profile's leniencies that the header checks do not read, which (d)
 * leaves out: the codestream's rules (f), by the reason or, for a type the
 * codestream breaks, by the field. */
static int lenient(const row *r) {
    return codestream_rule(r->reason) ||
           (strcmp(r->reason, "decode") == 0 && codestream_rule(r->field));
}

/* (a) to (d) above, over every row. Returns the rows the header checks
 * reject by the manifest's reason, "decode" included. */
static int check_header_contract(const row *rows, size_t n) {
    char detail[8192];
    size_t i, j;
    int rejected = 0;

    for (i = 0; i < n; i++) {
        const row *r = &rows[i];
        int decode = strcmp(r->reason, "decode") == 0, named = 1;
        snprintf(detail, sizeof detail, "%s: header checks %s at %zu, manifest %s/%s (%s)",
                 r->name, r->header ? r->header : "valid", r->at, r->standard, r->profile,
                 r->reason);
        if (strcmp(r->standard, "valid") == 0) {
            check(r->header == NULL, "(a) header checks accept standard-valid", detail);
            continue;
        }
        if (r->header != NULL) {
            check(decode || strcmp(r->header, r->reason) == 0, "(b) header rule name", detail);
            rejected += decode || strcmp(r->header, r->reason) == 0;
        }
        /* (c): r's reason, if the checks name it for another vector. */
        for (j = 0; j < n && !decode && r->header == NULL && !codestream_rule(r->reason); j++)
            if (rows[j].header != NULL && strcmp(rows[j].header, r->reason) == 0 &&
                strcmp(rows[j].reason, r->reason) == 0) {
                named = 0;
                break;
            }
        check(named, "(c) header rule fires for every vector of its reason", detail);
        if (strcmp(r->profile, "valid") == 0 && !lenient(r))
            check(r->header != NULL, "(d) header checks reject standard-only invalid", detail);
    }
    return rejected;
}

/* Consume deferred PLTs through the public cursor and compare the complete
 * result, including diagnostic offsets, with eager profile validation. */
static void check_deferred(const uint8_t *buf, size_t size, const char *name) {
    hv_boxes boxes;
    hv_box box;
    const char *error;
    size_t at;
    hv_boxes_file(&boxes, buf, size);
    while (hv_boxes_next(&boxes, &box, &error, &at) == 1) {
        hv_codestream cs;
        hv_item item;
        hv_plt_reader plt;
        const char *eager, *deferred = NULL;
        size_t eager_at = 0, deferred_at = 0, tile_at = 0;
        int status;
        if (box.type != HV_BOX_JP2C) continue;
        eager = hv_codestream_check(buf, box.payload, box.end, HV_READ_JPIP, &eager_at);
        hv_plt_init(&plt, HV_PLT_PROFILE);
        status = hv_codestream_open(&cs, buf, box.payload, box.end, HV_READ_JPIP_INDEX);
        while (status == 0 && (status = hv_codestream_next(&cs, &item)) == 1) {
            if (item.kind == HV_TILE_PART) tile_at = item.start;
            if (item.plt != NULL) {
                uint64_t value;
                deferred = hv_plt_begin(&plt, item.plt);
                if (deferred == NULL) {
                    while (hv_plt_read(&plt, buf, &value) == 1) ;
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
            if (deferred != NULL) break;
            status = 0;
        }
        if (status < 0) {
            deferred = cs.error;
            deferred_at = cs.error_at;
        }
        check((eager == NULL && deferred == NULL) ||
              (eager != NULL && deferred != NULL && strcmp(eager, deferred) == 0 &&
               eager_at == deferred_at), "deferred profile matches eager", name);
        hv_codestream_close(&cs);
    }
}

/* The corpus: every row of the manifest. */
static void check_corpus(const char *dir) {
    char path[4096], line[8192];
    FILE *manifest;
    row *rows = NULL;
    size_t nrows = 0, cap = 0, i;
    int jp2 = 0, jpx = 0, valid = 0, header_invalid;

    snprintf(path, sizeof path, "%s/manifest.tsv", dir);
    if ((manifest = fopen(path, "r")) == NULL) {
        check(0, "cannot read", path);
        return;
    }
    check(fgets(line, sizeof line, manifest) != NULL &&
          strcmp(line, "file\tkind\tstandard\tprofile\treason\tfield\tnote\tcompanions\t"
                       "profile_reason\n") == 0,
          "manifest header", NULL);
    while (fgets(line, sizeof line, manifest) != NULL) {
        char *field[9], *p = line, detail[8192];
        const char *error;
        uint8_t *buf;
        size_t size;
        hv_served served;
        row *r;
        int k, expected, is_jpx;

        line[strcspn(line, "\n")] = 0;
        for (k = 0; k < 9 && p != NULL; k++) {
            field[k] = p;
            if ((p = strchr(p, '\t')) != NULL)
                *p++ = 0;
        }
        if (k < 9 || p != NULL) {
            check(0, "manifest row", line);
            continue;
        }
        expected = strcmp(field[3], "valid") == 0;
        is_jpx = strcmp(field[1], "jpx") == 0;
        if ((buf = vector(dir, field[0], &size)) == NULL)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, field[0]);
        check_deferred(buf, size, field[0]);
        error = hv_check_served(path, buf, size, is_jpx, dir, &served);
        jpx += is_jpx;
        jp2 += !is_jpx;
        snprintf(detail, sizeof detail, "%s: reader %s at %zu%s%s, manifest %s (%s)", field[0],
                 error ? error : "valid", served.at, served.linked_path[0] ? " of " : "",
                 served.linked_path, field[3], field[8]);
        check((error == NULL) == expected, "profile label", detail);
        /* (e): a rule the profile layer names, the reader names too. */
        if (error != NULL && strchr(field[8], '.') != NULL)
            check(strcmp(error, field[8]) == 0, "(e) profile rule name", detail);
        /* (f): the codestreams at the standard layer, against the
         * standard label and a codestream rule the manifest names. */
        {
            size_t at = 0;
            const char *standard = standard_codestreams(buf, size, &at);
            snprintf(detail, sizeof detail, "%s: codestream check %s at %zu, manifest %s (%s)",
                     field[0], standard ? standard : "valid", at, field[2], field[4]);
            if (strcmp(field[2], "valid") == 0)
                check(standard == NULL, "(f) codestreams accepted at the standard layer", detail);
            else if (codestream_rule(field[4]))
                check(standard != NULL && strcmp(standard, field[4]) == 0,
                      "(f) codestream rule name at the standard layer", detail);
        }
        valid += expected;

        if (nrows == cap) {
            row *grown = realloc(rows, (cap = cap ? 2 * cap : 256) * sizeof *rows);
            if (grown == NULL) {
                check(0, "out of memory", NULL);
                free(buf);
                break;
            }
            rows = grown;
        }
        r = &rows[nrows++];
        r->name = copy(field[0]);
        r->standard = copy(field[2]);
        r->profile = copy(field[3]);
        r->reason = copy(field[4]);
        r->field = copy(field[5]);
        if (!r->name || !r->standard || !r->profile || !r->reason || !r->field) {
            check(0, "out of memory", NULL);
            nrows--;
            free(buf);
            break;
        }
        r->at = 0;
        r->header = hv_check_headers(buf, size, &r->at);
        free(buf);
    }
    fclose(manifest);
    header_invalid = check_header_contract(rows, nrows);
    printf("%d JP2 and %d JPX vectors, %d profile-valid, %d invalid by the header checks\n", jp2,
           jpx, valid, header_invalid);
    check(jp2 > 0 && jpx > 0 && valid > 0 && header_invalid > 0,
          "corpus has JP2 and JPX vectors, some valid, some with invalid headers", NULL);
    for (i = 0; i < nrows; i++) {
        free(rows[i].name);
        free(rows[i].standard);
        free(rows[i].profile);
        free(rows[i].reason);
        free(rows[i].field);
    }
    free(rows);
}

/* The reader's names at the standard layer (flags 0) and for hv_transcode's
 * input (HV_READ_PADDED), where the manifest's reason is `decode`
 * or the profile's: misplaced codes, and PLT padding per tile-part. */
static void check_standard_names(const char *dir) {
    static const struct {
        const char *name;
        unsigned flags;
        const char *error;
    } cases[] = {
        {"jp2-main-ppt.jp2", 0, "main.marker-code"},
        {"jp2-tile-tlm.jp2", 0, "tile.marker-code"},
        {"jp2-precincts-rule-plt.padding-position-43.jp2", HV_READ_PADDED,
         "plt.padding-position"},
    };
    size_t i, size;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint8_t *buf = vector(dir, cases[i].name, &size);
        const char *error;
        if (buf == NULL)
            continue;
        error = profile_error(buf, size, cases[i].flags);
        check(error != NULL && strncmp(error, cases[i].error, strlen(cases[i].error)) == 0,
              cases[i].name, error ? error : "accepted");
        free(buf);
    }
}

/* HV_READ_PACKETS_JPIP: the main header only. */
static void check_headers_scope(const char *dir) {
    static const struct {
        const char *name;
        const char *error;      /* expected with HV_READ_PACKETS_JPIP; NULL: accepted */
        const char *profile;    /* expected with HV_READ_JPIP; NULL: any error */
    } cases[] = {
        /* The corpus labels SOP `decode` (Cod-Profile): the reader names
         * the rule. */
        {"jp2-cod.scod.sopMarkers-1.jp2", NULL, "cod.sop-markers"},
        {"jp2-rule-codestream.no-plt-4.jp2", NULL, NULL},
        {"jp2-siz.xtsiz-3.jp2", "siz.single-tile", NULL},
        {"jp2-siz.xosiz-1.jp2", "siz.zero-origin", NULL},
        {"jp2-rule-main.packet-headers-moved-48.jp2",
         "main.marker-code", NULL},
        {"jp2-main-ff30-length.jp2", "main.marker-code", NULL},
        {"jp2-main-ppt.jp2", "main.marker-code", NULL},
    };
    size_t i, size;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint8_t *buf = vector(dir, cases[i].name, &size);
        const char *error;
        if (buf == NULL)
            continue;
        error = profile_error(buf, size, HV_READ_PACKETS_JPIP);
        if (cases[i].error == NULL)
            check(error == NULL, cases[i].name, error);
        else
            check(error != NULL && strncmp(error, cases[i].error, strlen(cases[i].error)) == 0,
                  cases[i].name, error ? error : "accepted");
        error = profile_error(buf, size, HV_READ_JPIP);
        check(error != NULL && (cases[i].profile == NULL ||
                                strncmp(error, cases[i].profile, strlen(cases[i].profile)) == 0),
              cases[i].name, error ? error : "accepted with HV_READ_JPIP");
        free(buf);
    }
}

/* hv_served_jp2 on inputs that are not JP2 files. */
static void check_file_rules(const char *dir) {
    size_t size, at, cs;
    hv_box jp2c;
    uint8_t *buf = vector(dir, "jp2.jp2", &size);
    const char *error;
    if (buf == NULL)
        return;
    check(hv_served_jp2(buf, size, &jp2c, &at) == NULL, "jp2.jp2", NULL);
    /* The size rule is checked before the buffer is read. */
    error = hv_served_jp2(buf, (size_t)INT_MAX + 1, &jp2c, &at);
    check(error != NULL && strcmp(error, "file.size-limit") == 0, "INT_MAX + 1 bytes", error);
    /* The raw codestream of jp2.jp2. */
    hv_served_jp2(buf, size, &jp2c, &at);
    cs = jp2c.payload;
    error = hv_served_jp2(buf + cs, jp2c.end - cs, &jp2c, &at);
    check(error != NULL && strcmp(error, "file.signature") == 0, "raw codestream", error);
    free(buf);
}

/* hv_served_jpx's rule names where a later check could claim the file. */
static void check_jpx_rules(const char *dir) {
    static const struct {
        const char *name, *error;
    } cases[] = {
        {"jpx-linked-rule-jpx.one-dtbl-11.jpx", "jpx.one-dtbl"},
        {"jpx-nested-jpch.jpx", "box.nested-top-level"},
    };
    size_t i, size, at;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint8_t *buf = vector(dir, cases[i].name, &size);
        const char *error;
        hv_served_sources jpx;
        if (buf == NULL)
            continue;
        error = hv_served_jpx(buf, size, &jpx, &at);
        check(error != NULL && strcmp(error, cases[i].error) == 0, cases[i].name,
              error ? error : "accepted");
        if (error == NULL)
            hv_served_sources_free(&jpx);
        free(buf);
    }
}

/* The box of `type` that starts at `at` among the top-level boxes of buf
 * and, when `parent` is nonzero, the children of the top-level `parent`
 * boxes. */
static int box_at(const uint8_t *buf, size_t size, uint32_t parent, uint32_t type, size_t at) {
    hv_boxes it, children;
    hv_box box, child;
    const char *error;
    size_t where;
    hv_boxes_file(&it, buf, size);
    while (hv_boxes_next(&it, &box, &error, &where) == 1) {
        if (box.type == type && box.start == at)
            return 1;
        if (parent == 0 || box.type != parent)
            continue;
        hv_boxes_children(&children, buf, &box);
        while (hv_boxes_next(&children, &child, &error, &where) == 1)
            if (child.type == type && child.start == at)
                return 1;
    }
    return 0;
}

/* Where the reader's errors point. */
static void check_offsets(const char *dir) {
    static const char *const second[][2] = {
        {"jp2-rule-codestream.one-cod-before-sot-0.jp2", "codestream.one-cod-before-sot"},
        {"jp2-rule-codestream.one-qcd-before-sot-1.jp2", "codestream.one-qcd-before-sot"},
    };
    size_t size, at = 0, i;
    uint8_t *buf;
    const char *error;
    hv_served_sources jpx;

    /* A second COD or QCD in the main header (COD QCD COD, or COD QCD
     * QCD): at that segment, which follows the first QCD, not at the SOT. */
    for (i = 0; i < sizeof second / sizeof *second; i++) {
        hv_box jp2c;
        hv_codestream cs;
        hv_item item;
        size_t qcd_end = 0;
        if ((buf = vector(dir, second[i][0], &size)) == NULL)
            continue;
        if (hv_served_jp2(buf, size, &jp2c, &at) != NULL ||
            hv_codestream_open(&cs, buf, jp2c.payload, jp2c.end, HV_READ_VALIDATE) != 0) {
            check(0, second[i][0], "does not open");
            free(buf);
            continue;
        }
        while (hv_codestream_next(&cs, &item) == 1)
            if (item.code == HV_QCD && qcd_end == 0)
                qcd_end = item.end;
        check(cs.error != NULL && strcmp(cs.error, second[i][1]) == 0 && cs.error_at == qcd_end,
              second[i][0], cs.error ? cs.error : "accepted");
        hv_codestream_close(&cs);
        free(buf);
    }

    /* A header rule comparing boxes: at the header box. */
    if ((buf = vector(dir, "jp2-header-ihdr.width-20.jp2", &size)) != NULL) {
        error = hv_check_jp2h(buf, size, &at);
        check(error != NULL && strcmp(error, "ihdr.width") == 0 &&
              box_at(buf, size, 0, HV_BOX_JP2H, at), "ihdr.width at jp2h", error);
        free(buf);
    }
    /* A fragment's DR: at its flst box. */
    if ((buf = vector(dir, "jpx-linked-rule-flst.dr-range-3.jpx", &size)) != NULL) {
        error = hv_served_jpx(buf, size, &jpx, &at);
        check(error != NULL && strcmp(error, "flst.dr-range") == 0 &&
              box_at(buf, size, HV_BOX_FTBL, HV_BOX_FLST, at), "flst.dr-range at flst", error);
        if (error == NULL)
            hv_served_sources_free(&jpx);
        free(buf);
    }
}

/* A second jp2h in a JPX file (T.801 M.11.5): jpx.one-jp2h. */
static void check_second_jp2h(const char *dir) {
    size_t size, at = 0;
    uint8_t *buf = vector(dir, "jpx-embedded.jpx", &size), *two;
    const char *error;
    hv_boxes it;
    hv_box box, jp2h = {0};
    int found = 0;

    if (buf == NULL)
        return;
    hv_boxes_file(&it, buf, size);
    while (!found && hv_boxes_next(&it, &box, &error, &at) == 1)
        if (box.type == HV_BOX_JP2H) {
            jp2h = box;
            found = 1;
        }
    check(found, "jpx-embedded.jpx has a jp2h", NULL);
    if (found && (two = malloc(size + (jp2h.end - jp2h.start))) != NULL) {
        size_t n = jp2h.end - jp2h.start;
        memcpy(two, buf, jp2h.end);
        memcpy(two + jp2h.end, buf + jp2h.start, n);
        memcpy(two + jp2h.end + n, buf + jp2h.end, size - jp2h.end);
        error = hv_check_jpx_headers(two, size + n, &at);
        check(error != NULL && strcmp(error, "jpx.one-jp2h") == 0, "two jp2h in a JPX file",
              error ? error : "accepted");
        free(two);
    }
    free(buf);
}

/* The accessors and items over a codestream's life. */
static void check_codestream_lifetime(const char *dir) {
    size_t size;
    uint8_t *buf = vector(dir, "jp2.jp2", &size);
    hv_codestream cs;
    hv_item item;
    hv_box jp2c;
    size_t at, data = 0, plts = 0;
    uint64_t plt_sum = 0;
    int status, sot_ok = 1, siz_ok = 0, plt_ok = 1;

    /* start after end: refused, and nothing to read. */
    check(hv_codestream_open(&cs, (const uint8_t *)"", 1, 0, HV_READ_VALIDATE) != 0 && cs.error != NULL &&
          hv_codestream_siz(&cs) == NULL && hv_codestream_next(&cs, &item) < 0,
          "codestream start after end", cs.error);
    hv_codestream_close(&cs);
    if (buf == NULL)
        return;
    if (hv_served_jp2(buf, size, &jp2c, &at) != NULL) {
        check(0, "jp2.jp2", "hv_served_jp2");
        free(buf);
        return;
    }
    check(hv_codestream_open(&cs, buf, jp2c.payload, jp2c.end, HV_READ_JPIP) == 0 &&
          hv_codestream_siz(&cs) != NULL && hv_codestream_cod(&cs) == NULL &&
          hv_codestream_qcd(&cs) == NULL, "accessors after open", cs.error);
    while ((status = hv_codestream_next(&cs, &item)) == 1) {
        int tile = item.kind == HV_TILE_PART || item.kind == HV_TILE_SEGMENT ||
                   item.kind == HV_TILE_DATA;
        sot_ok &= tile ? item.sot.lsot != 0 : item.sot.lsot == 0 && item.sot.psot == 0;
        if (item.siz != NULL)
            siz_ok = item.siz == hv_codestream_siz(&cs) &&
                     item.siz->ncomponents == item.siz->fixed->csiz;
        if (item.plt != NULL) {
            /* The entries, in place after Zplt, to the end of the segment. */
            size_t pos = item.plt->start;
            uint64_t v;
            while (hv_plt_next(buf, &pos, item.plt->end, &v) == 1)
                plt_sum += v;
            plts++;
            plt_ok &= pos == item.end && item.plt->end == item.end &&
                      item.plt->start == item.start + 5;
        }
        if (item.kind == HV_TILE_DATA)
            data += item.end - item.start;
    }
    check(status == 0, "jp2.jp2 reads through", cs.error);
    check(sot_ok, "hv_item.sot only on tile-part items", NULL);
    check(siz_ok, "hv_item.siz: the accessor's, with Csiz components", NULL);
    check(plts > 0 && plt_ok && plt_sum == data, "hv_item.plt: the entries cover the data",
          NULL);
    check(hv_codestream_cod(&cs) != NULL && hv_codestream_qcd(&cs) == NULL,
          "profile exposes COD and keeps QCD opaque", NULL);
    hv_codestream_close(&cs);
    check(hv_codestream_siz(&cs) == NULL && hv_codestream_cod(&cs) == NULL &&
          hv_codestream_qcd(&cs) == NULL, "accessors after close", NULL);
    free(buf);
}

/* Opaque profile bodies still have checked segment boundaries. */
static void check_opaque_main(const char *dir) {
    size_t size, at = 0, qstart = 0, qend = 0;
    uint8_t *buf = vector(dir, "jp2.jp2", &size);
    hv_box box;
    hv_codestream cs;
    hv_item item;
    if (buf == NULL) return;
    if (hv_served_jp2(buf, size, &box, &at) != NULL) {
        check(0, "opaque bodies base", NULL);
        free(buf);
        return;
    }
    hv_codestream_open(&cs, buf, box.payload, box.end, HV_READ_VALIDATE);
    while (hv_codestream_next(&cs, &item) == 1)
        if (item.code == HV_QCD) { qstart = item.start; qend = item.end; }
    hv_codestream_close(&cs);
    check(qend > qstart + 4, "opaque QCD base", NULL);
    if (qend > qstart + 4) {
        hv_read_mode modes[] = {HV_READ_VALIDATE, HV_READ_PADDED, HV_READ_PACKETS,
                                HV_READ_PACKETS_JPIP, HV_READ_JPIP, HV_READ_JPIP_INDEX};
        size_t i;
        /* Remove the QCD body; insert COM with no registration or text. */
        size_t n = box.end - box.payload - (qend - qstart - 4) + 4;
        uint8_t *data = malloc(box.end - box.payload + 4);
        size_t q = qstart - box.payload;
        memcpy(data, buf + box.payload, q + 4);
        data[q + 2] = 0; data[q + 3] = 2;
        memcpy(data + q + 4, "\xff\x64\x00\x02", 4);
        memcpy(data + q + 8, buf + qend, box.end - qend);
        for (i = 0; i < sizeof modes / sizeof *modes; i++) {
            int status, opaque = 1, seen = 0;
            hv_codestream_open(&cs, data, 0, n, modes[i]);
            while ((status = hv_codestream_next(&cs, &item)) == 1)
                if (item.code == HV_QCD || item.code == HV_COM) {
                    seen++;
                    opaque &= item.qcd == NULL && item.com == NULL;
                }
            check(i < 2 ? status < 0 && cs.error != NULL &&
                  strcmp(cs.error, "invalid QCD") == 0 : status == 0 && seen == 2 && opaque &&
                  hv_codestream_qcd(&cs) == NULL, "opaque QCD/COM scope", cs.error);
            hv_codestream_close(&cs);
        }
        /* Restore QCD so both validation modes reach invalid COM. */
        n = box.end - box.payload + 4;
        memcpy(data, buf + box.payload, q);
        memcpy(data + q, "\xff\x64\x00\x02", 4);
        memcpy(data + q + 4, buf + qstart, box.end - qstart);
        for (i = 0; i < 2; i++) {
            int status;
            hv_codestream_open(&cs, data, 0, n, modes[i]);
            while ((status = hv_codestream_next(&cs, &item)) == 1) ;
            check(status < 0 && cs.error != NULL && strcmp(cs.error, "invalid COM") == 0,
                  "COM remains interpreted by validation modes", cs.error);
            hv_codestream_close(&cs);
        }
        data[q + 3] = 1;
        hv_codestream_open(&cs, data, 0, n, HV_READ_JPIP);
        while (hv_codestream_next(&cs, &item) == 1) ;
        check(cs.error != NULL, "opaque COM rejects invalid segment length", NULL);
        hv_codestream_close(&cs);
        free(data);
    }
    free(buf);
}

/* hv_plt_next on entries the corpus does not hold: the end, a value in
 * ten bytes, one in eleven (the standard layer's: Table A.36 sets no
 * limit, the profile's Iplt ten bytes), and the entries it refuses
 * (truncated, above 64 bits), which leave *pos where it was. */
static void check_plt_next(void) {
    static const uint8_t two[] = {0x81, 0x00};
    static const uint8_t truncated[] = {0x81};
    static const uint8_t top[] = {0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    static const uint8_t over[] = {0x82, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    static const uint8_t eleven[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                     0x81, 0x00};
    size_t pos = 0;
    uint64_t v = 0;

    check(hv_plt_next(two, &pos, sizeof two, &v) == 1 && v == 128 && pos == 2 &&
          hv_plt_next(two, &pos, sizeof two, &v) == 0 && pos == 2, "PLT entry of two bytes",
          NULL);
    pos = 0;
    check(hv_plt_next(top, &pos, sizeof top, &v) == 1 && v == (uint64_t)1 << 63 &&
          pos == sizeof top, "PLT entry of 2^63 in ten bytes", NULL);
    pos = 0;
    check(hv_plt_next(truncated, &pos, sizeof truncated, &v) == -1 && pos == 0,
          "truncated PLT entry", NULL);
    check(hv_plt_next(over, &pos, sizeof over, &v) == -1 && pos == 0,
          "PLT entry above 64 bits", NULL);
    check(hv_plt_next(eleven, &pos, sizeof eleven, &v) == 1 && v == 128 && pos == sizeof eleven,
          "PLT entry of eleven bytes", NULL);
    pos = 0;
    check(hv_rule_packet_length(eleven, &pos, sizeof eleven, 1, &v) != NULL && pos == 0,
          "PLT entry of eleven bytes in the profile", NULL);
}

static void check_link_paths(void) {
    static const struct {
        const char *loc, *expected;
    } valid[] = {
        {"file://%2Ftmp/a.jp2", "/tmp/a.jp2"},
        {"file://a%20b.jp2", "d/a b.jp2"},
        {"file://localhost/a.jp2", "d/localhost/a.jp2"},
    };
    static const struct {
        const char *loc, *error;
    } invalid[] = {
        {"file://%00.jp2", "url.percent-encoding"},
        {"file://%4.jp2", "url.percent-encoding"},
        {"file://", "url.length"},
        {"http://a.jp2", "url.file-scheme"},
    };
    hv_served_source link = {0};
    char out[64];
    const char *error;
    size_t i;

    for (i = 0; i < sizeof valid / sizeof *valid; i++) {
        link.loc = (const uint8_t *)valid[i].loc;
        link.loc_size = strlen(valid[i].loc);
        check(hv_served_path(&link, "d/x.jpx", out, sizeof out) == NULL &&
              strcmp(out, valid[i].expected) == 0, "link path", valid[i].loc);
    }
    link.loc = (const uint8_t *)valid[1].loc;
    link.loc_size = strlen(valid[1].loc);
    check(hv_served_path(&link, NULL, out, sizeof out) == NULL && strcmp(out, "a b.jp2") == 0,
          "link path without a JPX path", valid[1].loc);
    for (i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        link.loc = (const uint8_t *)invalid[i].loc;
        link.loc_size = strlen(invalid[i].loc);
        memset(out, 'X', sizeof out);
        error = hv_served_path(&link, "d/x.jpx", out, sizeof out);
        check(error != NULL && strcmp(error, invalid[i].error) == 0 && out[0] == 0,
              "invalid link path", invalid[i].loc);
    }
    link.loc = (const uint8_t *)valid[1].loc;
    link.loc_size = strlen(valid[1].loc);
    memset(out, 'X', sizeof out);
    check(hv_served_path(&link, "d/x.jpx", out, 4) != NULL && out[0] == 0,
          "link path too long", valid[1].loc);
}

/* hv_rule_url on LOC with its NUL. */
static const char *url_rule(const char *loc) {
    return hv_rule_url(0, 0, (const uint8_t *)loc, strlen(loc) + 1);
}

static void check_url_rule(void) {
    static const struct {
        const char *loc, *error;
    } cases[] = {
        {"file://a.jp2", NULL},
        {"file://a%20b.jp2", NULL},
        {"file://", "url.length"},
        {"http://a.jp2", "url.file-scheme"},
        {"file://a%2.jp2", "url.percent-encoding"},
        {"file://a%00.jp2", "url.percent-encoding"},
        {"file://a.jpx", "url.jp2-target"},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *error = url_rule(cases[i].loc);
        check(cases[i].error ? error != NULL && strcmp(error, cases[i].error) == 0
                             : error == NULL, cases[i].loc, error ? error : "accepted");
    }
}

static void check_long_url(void) {
    uint8_t loc[1026];
    memset(loc, 'a', sizeof loc);
    memcpy(loc, "file://", HV_FILE_SCHEME_LENGTH);
    memcpy(loc + 1019, ".jp2", 4);
    loc[1023] = 0;
    check(hv_rule_url(0, 0, loc, 1024) == NULL, "1,023-character URL", NULL);
    memcpy(loc + 1020, ".jp2", 4);
    loc[1024] = 0;
    check(hv_rule_url(0, 0, loc, 1025) == NULL, "1,024-character URL", NULL);
    memcpy(loc + 1021, ".jp2", 4);
    loc[1025] = 0;
    check(hv_rule_url(0, 0, loc, 1026) == NULL, "1,025-character URL", NULL);
}

static void check_jpx_name(void) {
    check(hv_is_jpx_name("a.jpx") && hv_is_jpx_name("a.JPX") && hv_is_jpx_name("d/a.Jpx") &&
          !hv_is_jpx_name("a.jp2") && !hv_is_jpx_name("jpx") && !hv_is_jpx_name(""),
          "JPX file names", NULL);
}

/* One cdef set of entries against hv_rule_cdef, pairwise (up to
 * HV_CDEF_PAIRWISE) or sorted in scratch. */
static const char *cdef(hv_header *h, const CdefEntry *entries, size_t n) {
    static uint64_t scratch[65535];
    return hv_rule_cdef(h, entries, n, NULL, scratch);
}

static void expect_rule(const char *what, const char *error, const char *want) {
    check(want == NULL ? error == NULL : error != NULL && strcmp(error, want) == 0, what,
          error != NULL ? error : "accepted");
}

/* hv_rules.c functions whose inputs the corpus cannot reach. */
static void check_rules(void) {
    static hv_header h;
    static CdefEntry entries[65535];
    static const size_t sizes[] = {3, HV_CDEF_PAIRWISE, HV_CDEF_PAIRWISE + 1, 65535};
    hv_box_tree tree;
    size_t i, k;

    /* cdef, at the sizes compared pairwise and sorted: channel i the
     * colour i + 1, each described once; a pair twice; one channel
     * described twice; unspecified Typ or Asoc; opacity channels. */
    for (k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k], last = n - 1;
        hv_header_init(&h, HV_BOX_JP2H, 0);
        h.cdef = 1;
        for (i = 0; i < n; i++) {
            entries[i].cn = i;
            entries[i].typ = 0;
            entries[i].asoc = i + 1 < 65535 ? i + 1 : 0;
        }
        expect_rule("distinct cdef entries", cdef(&h, entries, n), NULL);
        check(h.cdef_cn == last, "cdef's largest Cn", NULL);
        entries[last].asoc = entries[1].asoc;
        expect_rule("two channels with one pair", cdef(&h, entries, n), "cdef.pairs");
        entries[last].cn = 1;
        expect_rule("a channel described twice", cdef(&h, entries, n), NULL);
        entries[last].cn = last;
        entries[last].typ = 65535;
        expect_rule("unspecified Typ repeats", cdef(&h, entries, n), NULL);
        entries[last].typ = 0;
        entries[last].asoc = entries[0].asoc = 65535;
        expect_rule("unspecified Asoc repeats", cdef(&h, entries, n), NULL);
        entries[0].asoc = 1;
        /* Opacity: channel last for the whole image, and another. */
        entries[last].typ = 1;
        entries[last].asoc = 0;
        expect_rule("an opacity for the whole image", cdef(&h, entries, n), NULL);
        entries[1].typ = 2;
        expect_rule("two opacities for the whole image, one for colour 2",
                    cdef(&h, entries, n), "cdef.opacity");
        entries[last].asoc = 5;
        entries[1].asoc = 7;
        expect_rule("opacities for two colours", cdef(&h, entries, n), NULL);
        entries[1].asoc = 5;
        expect_rule("two opacities for colour 5", cdef(&h, entries, n), "cdef.opacity");
        /* only the first cdef gives the largest Cn */
        h.cdef = 2;
        h.cdef_cn = 0;
        entries[1].typ = 0;
        entries[1].asoc = 2;
        check(cdef(&h, entries, n) == NULL && h.cdef_cn == 0, "a second cdef's Cn", NULL);
    }

    /* hv_header_init clears what an earlier header left. */
    h.cmap_count = h.cdef_cn = h.npc = 1;
    h.resc = h.resd = h.cmap_palette = 1;
    hv_header_init(&h, HV_BOX_JPCH, 1);
    check(h.cmap_count == 0 && h.cdef_cn == 0 && h.npc == 0 && h.resc == 0 && h.resd == 0 &&
          h.cmap_palette == 0 && h.children == 0 && h.parent == HV_BOX_JPCH && h.jpx == 1 &&
          h.jp2 == 0, "hv_header_init", NULL);

    /* Where jp2h is. */
    memset(&tree, 0, sizeof tree);
    tree.jp2h = 1;
    expect_rule("JP2 without jp2c", hv_rule_jp2h_place(&tree, 0, 0), "jp2.one-codestream");
    tree.jp2h = 2;
    expect_rule("JPX with two jp2h", hv_rule_jp2h_place(&tree, 1, 0), "jpx.one-jp2h");
    tree.jp2h = 0;
    expect_rule("JPX without jp2h", hv_rule_jp2h_place(&tree, 1, 0), NULL);
    tree.jp2h = 1;
    tree.jp2h_late_baseline = 1;
    expect_rule("JPX, jp2h late", hv_rule_jp2h_place(&tree, 1, 0), NULL);
    expect_rule("baseline JPX, jp2h late", hv_rule_jp2h_place(&tree, 1, 1), "jp2h.position");

    check(hv_rule_tile_part_count(HV_PROFILE_TILE_PARTS) == NULL &&
          hv_rule_tile_part_count(HV_PROFILE_TILE_PARTS + 1) != NULL, "tile-part limit", NULL);
}

/* hv_load_file reads regular files only, without blocking; hv_path_within
 * resolves symbolic links and ".."; hv_check_served with a root refuses a
 * link outside it, and a linked file over INT_MAX bytes before reading it.
 * In a temporary directory: a FIFO, an empty file a.jp2, sub/, sub/link, a
 * symbolic link to ../a.jp2, and jpx-linked-frame1.jp2, a sparse file of
 * INT_MAX + 1 bytes. */
static void check_files(const char *dir) {
    char tmp[] = "/tmp/test_profile.XXXXXX", fifo[64], file[64], sub[64], link[64],
         dotdot[96], missing[64], big[64], path[4096];
    char *resolved = NULL, *real = NULL;
    const char *error;
    hv_served served;
    uint8_t *buf;
    size_t size = 1;
    FILE *f;

    if (mkdtemp(tmp) == NULL) {
        check(0, "mkdtemp", strerror(errno));
        return;
    }
    snprintf(fifo, sizeof fifo, "%s/fifo", tmp);
    snprintf(file, sizeof file, "%s/a.jp2", tmp);
    snprintf(sub, sizeof sub, "%s/sub", tmp);
    snprintf(link, sizeof link, "%s/sub/link", tmp);
    snprintf(dotdot, sizeof dotdot, "%s/sub/../a.jp2", tmp);
    snprintf(missing, sizeof missing, "%s/missing.jp2", tmp);
    snprintf(big, sizeof big, "%s/jpx-linked-frame1.jp2", tmp);
    if (mkfifo(fifo, 0600) != 0 || (f = fopen(file, "wb")) == NULL || fclose(f) != 0 ||
        mkdir(sub, 0700) != 0 || symlink("../a.jp2", link) != 0 ||
        (f = fopen(big, "wb")) == NULL || fclose(f) != 0 ||
        truncate(big, (off_t)INT_MAX + 1) != 0) {
        check(0, "test files", strerror(errno));
        goto done;
    }
    errno = 0;
    check(hv_load_file(fifo, SIZE_MAX, &size) == NULL && errno == EINVAL,
          "hv_load_file of a FIFO", NULL);
    errno = 0;
    check(hv_load_file(tmp, SIZE_MAX, &size) == NULL && errno == EINVAL,
          "hv_load_file of a directory", NULL);
    buf = hv_load_file(file, 0, &size);
    check(buf != NULL && size == 0, "hv_load_file of an empty file", NULL);
    free(buf);
    errno = 0;
    check(hv_load_file(big, INT_MAX, &size) == NULL && errno == EFBIG,
          "hv_load_file of a file over its limit", NULL);

    real = realpath(file, NULL);
    check(hv_path_within(file, tmp, &resolved) == 1 && resolved != NULL && real != NULL &&
          strcmp(resolved, real) == 0, "a file inside its directory", resolved);
    free(resolved);
    check(hv_path_within(dotdot, tmp, NULL) == 1, "a path with .. inside the root", NULL);
    check(hv_path_within(file, sub, &resolved) == 0 && resolved == NULL,
          "a file outside the root", NULL);
    check(hv_path_within(link, sub, NULL) == 0, "a link out of the root", NULL);
    check(hv_path_within(link, tmp, NULL) == 1, "a link inside the root", NULL);
    check(hv_path_within(tmp, tmp, NULL) == 0, "the root itself", NULL);
    check(hv_path_within(file, "/", NULL) == 1, "the root /", NULL);
    errno = 0;
    check(hv_path_within(missing, tmp, NULL) == -1 && errno == ENOENT, "a missing file", NULL);

    /* The linked corpus base: its links are inside the vector directory,
     * outside the temporary one. */
    snprintf(path, sizeof path, "%s/jpx-linked.jpx", dir);
    if ((buf = hv_load_file(path, SIZE_MAX, &size)) == NULL) {
        check(0, "cannot read", path);
        goto done;
    }
    error = hv_check_served(path, buf, size, 1, dir, &served);
    check(error == NULL && served.linked == 2, "links inside the root", error);
    error = hv_check_served(path, buf, size, 1, tmp, &served);
    check(error != NULL && strcmp(error, "linked file outside the root directory") == 0 &&
          served.linked == 0, "links outside the root", error);
    error = hv_check_served(path, buf, size, 1, missing, &served);
    check(error != NULL && strcmp(error, "root directory cannot be resolved") == 0,
          "a root that does not exist", error);
    /* The same file in the temporary directory, where its first link names
     * the large file. */
    snprintf(path, sizeof path, "%s/jpx-linked.jpx", tmp);
    error = hv_check_served(path, buf, size, 1, NULL, &served);
    check(error != NULL && strcmp(error, "file.size-limit") == 0 && served.at_linked &&
          served.at == 0 && strstr(served.linked_path, "jpx-linked-frame1.jp2") != NULL,
          "a linked file over INT_MAX bytes", error);
    free(buf);

done:
    free(real);
    unlink(big);
    unlink(link);
    rmdir(sub);
    unlink(file);
    unlink(fifo);
    rmdir(tmp);
}

/* hv_geometry_init checks the SIZ and COD it is given and lays out no more
 * than its limits allow; within them, hv_geometry_packets agrees with the
 * rules' count (hv_rule_packets). From the codestream of the base
 * jp2-precincts.jp2, which has a decomposition level. */
static void check_geometry(const char *dir) {
    static const hv_geometry_limits roomy = {65536, 1000000, NULL};
    hv_codestream cs;
    hv_geometry g;
    hv_packet *packets = NULL;
    hv_box jp2c;
    hv_siz siz;
    SizFixed fixed;
    Cod cod;
    uint8_t *buf;
    size_t size = 0, at, count = 0;
    char error[256];
    int status;

    if ((buf = vector(dir, "jp2-precincts.jp2", &size)) == NULL)
        return;
    if (hv_served_jp2(buf, size, &jp2c, &at) != NULL ||
        hv_codestream_open(&cs, buf, jp2c.payload, jp2c.end, HV_READ_VALIDATE) != 0) {
        check(0, "jp2-precincts.jp2 opens", cs.error);
        free(buf);
        return;
    }
    {
        hv_item item;
        while (hv_codestream_cod(&cs) == NULL && hv_codestream_next(&cs, &item) == 1)
            ;
    }
    if (hv_codestream_cod(&cs) == NULL) {
        check(0, "jp2-precincts.jp2 has a COD", cs.error);
        hv_codestream_close(&cs);
        free(buf);
        return;
    }
    siz = *hv_codestream_siz(&cs);
    fixed = *siz.fixed;
    siz.fixed = &fixed;
    cod = *hv_codestream_cod(&cs);

    status = hv_geometry_init(&g, &siz, &cod, 0, &roomy, error, sizeof error);
    check(status == 0, "geometry of jp2-precincts.jp2", error);
    if (status == 0) {
        status = hv_geometry_packets(&g, &packets, &count, error, sizeof error);
        check(status == 0 && count == hv_rule_packets(&siz, &cod.sgcod, &cod.spcod),
              "packets of jp2-precincts.jp2 as the rules count them", error);
        free(packets);
    }
    hv_geometry_free(&g);

#define EXPECT_GEOMETRY(what, limits, message)                                  \
    check(hv_geometry_init(&g, &siz, &cod, 0, (limits), error, sizeof error) != 0 && \
          strcmp(error, (message)) == 0, (what), error);                        \
    hv_geometry_free(&g)
    {
        hv_geometry_limits few = {1, 1000000, NULL}, packets = {65536, 0, NULL},
                           data = {65536, 0, "tile data"};
        EXPECT_GEOMETRY("resolutions above the limit", &few,
                        "component and resolution count exceeds supported limit (1)");
        EXPECT_GEOMETRY("packets above the limit", &packets,
                        "packet count exceeds supported limit (0)");
        EXPECT_GEOMETRY("packets above the tile data", &data, "packet count exceeds tile data");
    }
    fixed.xtsiz = 0;
    EXPECT_GEOMETRY("Xtsiz 0", &roomy, "invalid SIZ");
    fixed.xtsiz = siz.fixed->xsiz;
    fixed.xosiz = fixed.xsiz;
    EXPECT_GEOMETRY("image origin at its end", &roomy, "siz.origin-inside");
    fixed = *hv_codestream_siz(&cs)->fixed;
    cod.sgcod.layers = 0;
    EXPECT_GEOMETRY("no layer", &roomy, "invalid COD");
    cod = *hv_codestream_cod(&cs);
    if (cod.spcod.levels > 0) {
        int r;
        cod.scod.customPrecincts = TRUE;
        cod.spcod.precincts.nCount = (int)cod.spcod.levels + 1;
        for (r = 0; r <= (int)cod.spcod.levels; r++)
            cod.spcod.precincts.arr[r].ppx = cod.spcod.precincts.arr[r].ppy = 7;
        cod.spcod.precincts.arr[1].ppx = 0;
        EXPECT_GEOMETRY("PPx 0 at resolution 1", &roomy, "cod.precincts-higher-zero");
        cod.spcod.precincts.nCount--;
        EXPECT_GEOMETRY("a precinct size short", &roomy, "cod.precincts-count");
    } else {
        check(0, "jp2-precincts.jp2 has decomposition levels", NULL);
    }
#undef EXPECT_GEOMETRY
    hv_codestream_close(&cs);
    free(buf);
}

/* hv_geometry_init's limits in its messages, with thousands separators. */
static void check_geometry_messages(void) {
    static SizFixed fixed;
    static Component components[3];
    static Cod cod;
    hv_geometry_limits limits = {2, 1000000, NULL};
    hv_siz siz = {&fixed, components, 3};
    hv_geometry g;
    char error[256];
    int i;

    fixed.xsiz = fixed.ysiz = fixed.xtsiz = fixed.ytsiz = 8;
    fixed.csiz = 3;
    for (i = 0; i < 3; i++) {
        components[i].depthMinus1 = 7;
        components[i].xrsiz = components[i].yrsiz = 1;
    }
    cod.sgcod.layers = 1;
    cod.spcod.levels = 5;
    check(hv_geometry_init(&g, &siz, &cod, 0, &limits, error, sizeof error) != 0 &&
          strcmp(error, "component and resolution count exceeds supported limit (2)") == 0,
          "resolution limit 2", error);
    hv_geometry_free(&g);
    limits.resolutions = 1234567;
    limits.packets = 1234;
    fixed.xsiz = fixed.ysiz = fixed.xtsiz = fixed.ytsiz = 1u << 20;
    cod.sgcod.layers = 2000;
    check(hv_geometry_init(&g, &siz, &cod, 0, &limits, error, sizeof error) != 0 &&
          strcmp(error, "packet count exceeds supported limit (1,234)") == 0,
          "packet limit 1,234", error);
    hv_geometry_free(&g);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_profile <vector directory>\n");
        return 2;
    }
    check_corpus(argv[1]);
    check_headers_scope(argv[1]);
    check_standard_names(argv[1]);
    check_file_rules(argv[1]);
    check_jpx_rules(argv[1]);
    check_offsets(argv[1]);
    check_second_jp2h(argv[1]);
    check_codestream_lifetime(argv[1]);
    check_opaque_main(argv[1]);
    check_plt_next();
    check_link_paths();
    check_url_rule();
    check_long_url();
    check_jpx_name();
    check_rules();
    check_files(argv[1]);
    check_geometry(argv[1]);
    check_geometry_messages();
    printf(failures ? "%d failures\n" : "all checks passed\n", failures);
    return failures != 0;
}
