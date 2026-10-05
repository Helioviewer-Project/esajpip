/* test_merge.c: tests of hv_merge_files and hv_merge_buffers (merge.c),
 * run by CTest as "merge" (label "tools").
 *
 * MERGE_FIXTURES holds the inputs and hvJP2K's output for them
 * (fixtures/FIXTURES.md); TRANSCODE_FIXTURES the Kakadu references of the
 * transcoder's tests, which are inputs too. */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "jpeg2000/hv_served.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpeg2000/hv_reader.h"
#include "merge/merge.h"

#ifndef MERGE_FIXTURES
#error "MERGE_FIXTURES must name the fixture directory"
#endif
#ifndef TRANSCODE_FIXTURES
#error "TRANSCODE_FIXTURES must name the transcoder's fixture directory"
#endif

static int failures, checks;

static void check(int ok, const char *format, ...) {
    va_list args;
    checks++;
    if (ok)
        return;
    failures++;
    fprintf(stderr, "FAIL: ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

typedef struct {
    uint8_t *data;
    size_t size;
} bytes;

static void bytes_free(bytes *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

static bytes read_file(const char *path) {
    bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || (b.data = malloc(n ? (size_t)n : 1)) == NULL ||
        fread(b.data, 1, (size_t)n, f) != (size_t)n) {
        check(0, "cannot read %s", path);
        free(b.data);
        b.data = NULL;
    } else {
        b.size = (size_t)n;
    }
    if (f)
        fclose(f);
    return b;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The inputs of the reference merge, in its order (FIXTURES.md). */
static const char *const names[] = {
    MERGE_FIXTURES "/input/swap_000.jp2",
    MERGE_FIXTURES "/input/swap_001.jp2",
    TRANSCODE_FIXTURES "/kakadu/2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2",
    TRANSCODE_FIXTURES "/kakadu/solo_fsi174_127x129_RLCP_PLT.jp2",
    TRANSCODE_FIXTURES "/kakadu/solo_fsi174_509x513_PCRL.jp2",
    TRANSCODE_FIXTURES "/kakadu/solo_fsi174_510x514_LRCP.jp2",
    TRANSCODE_FIXTURES "/kakadu/solo_fsi174_511x513_LRCP_PLT.jp2",
    TRANSCODE_FIXTURES "/kakadu/synthetic_rgb_129x129_CPRL_SOP_EPH.jp2",
};
#define NINPUTS (sizeof names / sizeof *names)

static bytes files[NINPUTS];
static hv_merge_input inputs[NINPUTS];

static void load_inputs(void) {
    size_t i;
    for (i = 0; i < NINPUTS; i++) {
        files[i] = read_file(names[i]);
        inputs[i].path = names[i];
        inputs[i].buf = files[i].data;
        inputs[i].size = files[i].size;
    }
}

/* Merges into memory. 0, or -1 with the message in error. */
static int merge_mode(const hv_merge_input *in, size_t n, int links, int validate, bytes *out, char *error,
                 size_t error_size) {
    FILE *f = tmpfile();
    long size;
    int status;
    out->data = NULL;
    out->size = 0;
    if (f == NULL) {
        snprintf(error, error_size, "no temporary file");
        return -1;
    }
    status = hv_merge_buffers(in, n, links, validate ? HV_OUTPUT_JPIP : HV_OUTPUT_JPEG2000, f, error, error_size);
    if (status == 0 && (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
                        fseek(f, 0, SEEK_SET) != 0 ||
                        (out->data = malloc(size ? (size_t)size : 1)) == NULL ||
                        fread(out->data, 1, (size_t)size, f) != (size_t)size))
        status = -1;
    else if (status == 0)
        out->size = (size_t)size;
    fclose(f);
    return status;
}

static int test_validate = 1;

static int merge(const hv_merge_input *in, size_t n, int links, bytes *out, char *error,
                 size_t error_size) {
    return merge_mode(in, n, links, test_validate, out, error, error_size);
}

/* The JPX file is within the served profile: hv_served_jpx, and every
 * codestream with HV_READ_JPIP, embedded or linked; and its header boxes are
 * valid (hv_check_jpx_headers). */
static void expect_served(const char *name, const bytes *jpx, const hv_merge_input *in) {
    hv_served_sources j;
    size_t at, i;
    const char *error = hv_check_jpx_headers(jpx->data, jpx->size, &at), *file;
    check(error == NULL, "%s: header boxes: %s at %zu", name, error, at);
    file = error = hv_served_jpx(jpx->data, jpx->size, &j, &at);
    check(error == NULL, "%s: %s at %zu", name, error, at);
    for (i = 0; error == NULL && i < j.count; i++) {
        if (j.jp2c != NULL) {
            error = hv_codestream_check(jpx->data, j.jp2c[i].payload, j.jp2c[i].end, HV_READ_JPIP,
                                        &at);
            check(error == NULL, "%s: codestream %zu: %s at %zu", name, i, error, at);
        } else {
            char path[4096], *real = realpath(in[i].path, NULL);
            check(hv_served_path(&j.links[i], NULL, path, sizeof path) == NULL && real != NULL &&
                  strcmp(path, real) == 0, "%s: link %zu does not name %s", name, i, in[i].path);
            error = hv_served_link(in[i].buf, in[i].size, &j.links[i], &at);
            check(error == NULL, "%s: link %zu: %s at %zu", name, i, error, at);
            free(real);
        }
    }
    if (file == NULL)           /* a failing codestream check included */
        hv_served_sources_free(&j);
}

/* The standard features of a Reader Requirements box, in order. */
static int rreq_features(const bytes *jpx, int *features, int max) {
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    int k = 0;
    hv_boxes_file(&it, jpx->data, jpx->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_RREQ) {
            const uint8_t *p = jpx->data + box.payload;
            int ml = p[0], nsf = p[1 + 2 * ml] << 8 | p[2 + 2 * ml], i;
            for (i = 0; i < nsf && k < max; i++)
                features[k++] = p[3 + 2 * ml + i * (2 + ml)] << 8 | p[4 + 2 * ml + i * (2 + ml)];
            break;
        }
    return k;
}

static void expect_features(const char *name, const bytes *jpx, const int *expected, int n) {
    int got[32], k = rreq_features(jpx, got, 32);
    check(k == n && memcmp(got, expected, (size_t)n * sizeof *got) == 0,
          "%s: rreq lists %d features, expected %d", name, k, n);
}

/* ------------------------------------------------------------------------ */

/* hvJP2K's output, byte for byte, and served. */
static void test_reference(void) {
    bytes expected = read_file(MERGE_FIXTURES "/expected/merged.jpx"), got;
    char error[512];
    if (merge(inputs, NINPUTS, 0, &got, error, sizeof error) != 0) {
        check(0, "reference merge: %s", error);
    } else {
        check(got.size == expected.size && memcmp(got.data, expected.data, got.size) == 0,
              "reference merge differs from hvJP2K's");
        expect_served("reference merge", &got, inputs);
    }
    bytes_free(&got);
    bytes_free(&expected);
}

/* The linked merge: served, its links naming the inputs, and box for box
 * the embedded one, with the codestreams as fragment tables, jpx the only
 * compatible brand, feature 15, and a final Data Reference box. */
static void test_links(void) {
    bytes embedded, linked;
    hv_boxes a, b;
    hv_box x, y;
    const char *message;
    char error[512];
    size_t at, n = 0;
    int more_a, more_b;
    static const int features[] = {1, 2, 5, 15};

    if (merge(inputs, NINPUTS, 0, &embedded, error, sizeof error) != 0 ||
        merge(inputs, NINPUTS, 1, &linked, error, sizeof error) != 0) {
        check(0, "linked merge: %s", error);
        return;
    }
    expect_served("linked merge", &linked, inputs);
    expect_features("linked merge", &linked, features, 4);
    hv_boxes_file(&a, embedded.data, embedded.size);
    hv_boxes_file(&b, linked.data, linked.size);
    for (;;) {
        more_a = hv_boxes_next(&a, &x, &message, &at) == 1;
        more_b = hv_boxes_next(&b, &y, &message, &at) == 1;
        if (!more_a)
            break;
        check(more_b, "linked merge: box %zu missing", n);
        if (!more_b)
            break;
        if (x.type == HV_BOX_JP2C) {
            check(y.type == HV_BOX_FTBL, "linked merge: box %zu is not an ftbl", n);
        } else if (x.type == HV_BOX_FTYP) {
            check(y.end - y.payload == 12 && get32(linked.data + y.payload + 8) == HV_BRAND_JPX,
                  "linked merge: ftyp does not list jpx alone");
        } else if (x.type != HV_BOX_RREQ) {
            check(x.end - x.start == y.end - y.start &&
                  memcmp(embedded.data + x.start, linked.data + y.start, x.end - x.start) == 0,
                  "linked merge: box %zu differs from the embedded merge", n);
        }
        n++;
    }
    check(more_b && y.type == HV_BOX_DTBL && hv_boxes_next(&b, &y, &message, &at) == 0,
          "linked merge: does not end with one dtbl");
    bytes_free(&embedded);
    bytes_free(&linked);
}

/* A copy of a JP2 file with n bytes of boxes inserted into its JP2 Header
 * box at offset `where`, or at its end for 0. */
static bytes insert_in_jp2h(const bytes *jp2, size_t where, const uint8_t *boxes, size_t n) {
    bytes out = {malloc(jp2->size + n), jp2->size + n};
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    memcpy(out.data, jp2->data, jp2->size);
    hv_boxes_file(&it, jp2->data, jp2->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_JP2H) {
            if (where == 0)
                where = box.end;
            memcpy(out.data + where, boxes, n);
            memcpy(out.data + where + n, jp2->data + where, jp2->size - where);
            put32(out.data + box.start, (uint32_t)(box.end - box.start + n));
            break;
        }
    return out;
}

/* The same at the end of the JP2 Header box. */
static bytes add_to_jp2h(const bytes *jp2, const uint8_t *boxes, size_t n) {
    return insert_in_jp2h(jp2, 0, boxes, n);
}

static void expect_error(const char *name, const hv_merge_input *in, size_t n, int links,
                         const char *message);

/* hvJP2K's reader_requirements cases: Rsiz 0, 1 and 2, and opacity
 * channels (cdef types 1 and 2) in two linked inputs. */
static void test_rreq(void) {
    static const int only[] = {1}, rsiz2[] = {1, 4}, rsiz0[] = {1, 5}, all[] = {1, 2, 5, 9, 10, 15};
    /* Channel 0 described as opacity and as premultiplied opacity. */
    static const uint8_t cdef[40] = {0, 0, 0, 40, 'c', 'd', 'e', 'f', 0, 5,
                                     0, 0, 0, 0, 0, 1,  /* red */
                                     0, 1, 0, 0, 0, 2,  /* green */
                                     0, 2, 0, 0, 0, 3,  /* blue */
                                     0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0};
    bytes jp2 = {NULL, 0}, opacity, out;
    hv_merge_input in[2];
    hv_box jp2c;
    char error[512];
    size_t at;
    int rsiz;

    if (files[0].data == NULL || hv_served_jp2(files[0].data, files[0].size, &jp2c, &at) != NULL)
        return;
    jp2.data = malloc(files[0].size);
    memcpy(jp2.data, files[0].data, files[0].size);
    jp2.size = files[0].size;
    in[0].path = names[0];
    in[0].buf = jp2.data;
    in[0].size = jp2.size;
    for (rsiz = 0; rsiz < 3; rsiz++) {
        jp2.data[jp2c.payload + 7] = (uint8_t)rsiz;
        if (merge(in, 1, 0, &out, error, sizeof error) != 0) {
            check(0, "Rsiz %d: %s", rsiz, error);
            continue;
        }
        if (rsiz == 0)
            expect_features("Rsiz 0", &out, rsiz0, 2);
        else if (rsiz == 1)
            expect_features("Rsiz 1", &out, only, 1);
        else
            expect_features("Rsiz 2", &out, rsiz2, 2);
        bytes copied;
        check(merge_mode(in, 1, 0, 0, &copied, error, sizeof error) == 0,
              "copying Rsiz %d: %s", rsiz, error);
        if (copied.data != NULL)
            check(copied.size == out.size && memcmp(copied.data, out.data, out.size) == 0,
                  "Rsiz %d reader requirements identical in copying/validation modes", rsiz);
        bytes_free(&copied);
        bytes_free(&out);
    }

    /* Header information accepts broader declarations; RREQ writing cannot. */
    {
        static const unsigned unsupported[] = {3, 0x4000, 0xffff};
        for (size_t i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
            put16(jp2.data + jp2c.payload + 6, unsupported[i]);
            for (int links = 0; links < 2; links++)
                for (int validate = 0; validate < 2; validate++) {
                    check(merge_mode(in, 1, links, validate, &out, error, sizeof error) != 0 &&
                          strstr(error, "for merge Reader Requirements") != NULL,
                          "Rsiz %u is rejected by RREQ admission: %s", unsupported[i], error);
                    bytes_free(&out);
                }
        }
    }

    /* Rsiz bit 15, T.801 extensions: not in a JP2 file (T.800 I.5.4,
     * Table A.10), so not an input. */
    jp2.data[jp2c.payload + 6] = 0x80;
    jp2.data[jp2c.payload + 7] = 0;
    if (merge(in, 1, 0, &out, error, sizeof error) == 0) {
        check(0, "Rsiz 0x8000: merged");
        bytes_free(&out);
    } else {
        check(strstr(error, "jp2.rsiz") != NULL, "Rsiz 0x8000: %s", error);
    }
    jp2.data[jp2c.payload + 6] = 0;

    /* A cdef box at the end of the JP2 Header box. */
    jp2.data[jp2c.payload + 7] = 0;
    opacity = add_to_jp2h(&jp2, cdef, sizeof cdef);
    in[0].buf = opacity.data;
    in[0].size = opacity.size;
    in[1] = in[0];
    if (merge(in, 2, 1, &out, error, sizeof error) != 0) {
        check(0, "opacity: %s", error);
    } else {
        expect_features("opacity, linked", &out, all, 6);
        bytes_free(&out);
    }
    /* A later input without the cdef of the first would inherit it. */
    in[1].buf = jp2.data;
    in[1].size = jp2.size;
    expect_error("no cdef after one", in, 2, 0, "no cdef box, but the first input has one");
    bytes_free(&opacity);
    bytes_free(&jp2);
}

/* An IPR box (T.800 I.6). */
static const uint8_t jp2i_box[14] = {0, 0, 0, 14, 'j', 'p', '2', 'i', '<', 'I', 'P', 'R', '/', '>'};

/* A copy of a JP2 file with its ihdr's IPR 1 and an IPR box after its
 * jp2h (ihdr.ipr). */
static bytes with_ipr(const bytes *jp2) {
    bytes out = {malloc(jp2->size + sizeof jp2i_box), jp2->size + sizeof jp2i_box};
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    hv_boxes_file(&it, jp2->data, jp2->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_JP2H)
            break;
    memcpy(out.data, jp2->data, box.end);
    memcpy(out.data + box.end, jp2i_box, sizeof jp2i_box);
    memcpy(out.data + box.end + sizeof jp2i_box, jp2->data + box.end, jp2->size - box.end);
    out.data[box.payload + 8 + 13] = 1;         /* ihdr IPR, after its box header */
    return out;
}

/* The IPR boxes of an input whose ihdr has IPR 1 go into its jpch, so
 * that the JPX file's IPR 1 has one (jpx.ipr): for the first input, a
 * later one, and both, embedded and linked. */
static void test_ipr(void) {
    static const int cases[3][2] = {{1, 0}, {0, 1}, {1, 1}};
    bytes ipr[2], out;
    hv_merge_input in[2];
    char error[512], name[64];
    int c, links, k;

    if (files[0].data == NULL || files[1].data == NULL)
        return;
    ipr[0] = with_ipr(&files[0]);
    ipr[1] = with_ipr(&files[1]);
    for (c = 0; c < 3; c++)
        for (links = 0; links < 2; links++) {
            hv_boxes it, children;
            hv_box box, child;
            const char *message;
            size_t at;
            int jpch = 0, found[2] = {0, 0};
            for (k = 0; k < 2; k++) {
                in[k] = inputs[k];
                if (cases[c][k]) {
                    in[k].buf = ipr[k].data;
                    in[k].size = ipr[k].size;
                }
            }
            snprintf(name, sizeof name, "IPR in input%s%s%s", cases[c][0] ? " 0" : "",
                     cases[c][1] ? " 1" : "", links ? ", linked" : "");
            if (merge(in, 2, links, &out, error, sizeof error) != 0) {
                check(0, "%s: %s", name, error);
                continue;
            }
            expect_served(name, &out, in);
            hv_boxes_file(&it, out.data, out.size);
            while (hv_boxes_next(&it, &box, &message, &at) == 1) {
                if (box.type != HV_BOX_JPCH || jpch >= 2)
                    continue;
                hv_boxes_children(&children, out.data, &box);
                while (hv_boxes_next(&children, &child, &message, &at) == 1)
                    found[jpch] += child.type == HV_BOX_JP2I &&
                                   child.end - child.start == sizeof jp2i_box &&
                                   memcmp(out.data + child.start, jp2i_box, sizeof jp2i_box) == 0;
                jpch++;
            }
            check(found[0] == cases[c][0] && found[1] == cases[c][1],
                  "%s: IPR boxes in the jpch boxes: %d and %d", name, found[0], found[1]);
            bytes_free(&out);
        }
    bytes_free(&ipr[0]);
    bytes_free(&ipr[1]);
}

/* A copy of a JP2 file whose XML box is moved to the end, where it runs to
 * the end of the file (LBox = 0). */
static bytes xml_to_end(const bytes *jp2) {
    bytes out = {malloc(jp2->size), jp2->size};
    hv_boxes it;
    hv_box box, xml = {0, 0, 0, 0, 0};
    const char *message;
    size_t at, n = 0;
    hv_boxes_file(&it, jp2->data, jp2->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1) {
        if (box.type == HV_BOX_XML && xml.type == 0) {
            xml = box;
            continue;
        }
        memcpy(out.data + n, jp2->data + box.start, box.end - box.start);
        n += box.end - box.start;
    }
    memcpy(out.data + n, jp2->data + xml.start, xml.end - xml.start);
    put32(out.data + n, 0);
    return out;
}

/* An XML box with LBox = 0, in an input merged after another: associated
 * with its codestream, with an explicit length, its contents as read. */
static void test_xml_to_end(void) {
    bytes moved, out;
    hv_merge_input in[2];
    hv_boxes it, children;
    hv_box box, child, xml = {0, 0, 0, 0, 0};
    const char *message;
    char error[512];
    size_t at, payload = 0;
    int links, asocs;

    if (files[0].data == NULL || files[1].data == NULL)
        return;
    moved = xml_to_end(&files[1]);
    hv_boxes_file(&it, files[1].data, files[1].size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_XML) {
            xml = box;
            break;
        }
    check(xml.type != 0, "the fixture has an XML box");
    for (links = 0; links < 2 && xml.type != 0; links++) {
        in[0] = inputs[0];
        in[1] = inputs[1];
        in[1].buf = moved.data;
        in[1].size = moved.size;
        if (merge(in, 2, links, &out, error, sizeof error) != 0) {
            check(0, "XML with LBox = 0%s: %s", links ? ", linked" : "", error);
            continue;
        }
        expect_served("XML with LBox = 0", &out, in);
        asocs = 0;
        hv_boxes_file(&it, out.data, out.size);
        while (hv_boxes_next(&it, &box, &message, &at) == 1)
            if (box.type == HV_BOX_ASOC && asocs++ == 1) {
                hv_boxes_children(&children, out.data, &box);
                while (hv_boxes_next(&children, &child, &message, &at) == 1)
                    if (child.type == HV_BOX_XML)
                        payload = child.end - child.payload;
                check(get32(out.data + child.start) == child.end - child.start &&
                          payload == xml.end - xml.payload &&
                          memcmp(out.data + child.payload, files[1].data + xml.payload,
                                 payload) == 0,
                      "XML with LBox = 0%s: not copied with its length", links ? ", linked" : "");
            }
        check(asocs == 2, "XML with LBox = 0: %d asoc boxes", asocs);
        bytes_free(&out);
    }
    bytes_free(&moved);
}

static void test_headers_mode(void) {
    /* A Resolution box holding a Capture Resolution box of 1/1 per meter. */
    static const uint8_t res[26] = {0, 0, 0, 26, 'r', 'e', 's', ' ',
                                    0, 0, 0, 18, 'r', 'e', 's', 'c',
                                    0, 1, 0, 1, 0, 1, 0, 1, 0, 0};
    bytes bad = {NULL, 0};
    hv_merge_input in[2];
    hv_boxes it;
    hv_box box, colr = {0, 0, 0, 0, 0};
    bytes out = {NULL, 0};
    const char *message;
    char error[512];
    uint8_t *colrs;
    size_t at, k;

    if (files[0].data == NULL || files[2].data == NULL)
        return;
    /* An input whose JP2 Header box its codestream contradicts: NC in the
     * ihdr of a one-component input after one with a palette, whose
     * generated cmap would take NC entries; the reference merge covers the
     * generated cmap itself. */
    bad.data = malloc(files[2].size);
    memcpy(bad.data, files[2].data, files[2].size);
    bad.size = files[2].size;
    hv_boxes_file(&it, bad.data, bad.size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_JP2H)
            break;
    in[0] = inputs[0];
    in[1] = inputs[2];
    in[1].buf = bad.data;
    in[1].size = bad.size;
    put16(bad.data + box.payload + 16, 65535);          /* ihdr NC: above Ihdr's 16,384 */
    expect_error("NC 65,535", in, 2, 0,
                 "ihdr: shorter than its fields, or a field out of range");
    put16(bad.data + box.payload + 16, 16384);          /* for Csiz 1 */
    expect_error("NC 16,384", in, 2, 0, "ihdr.nc");
    bytes_free(&bad);

    /* A cgrp in jp2h, which a JP2 reader ignores: the first input's jp2h
     * is the JPX file's, where it cannot be (cgrp.placement); a later
     * input's is not copied. */
    {
        static const uint8_t cgrp[23] = {0, 0, 0, 23, 'c', 'g', 'r', 'p',
                                         0, 0, 0, 15, 'c', 'o', 'l', 'r',
                                         1, 0, 0, 0, 0, 0, 17};
        bytes with = add_to_jp2h(&files[0], cgrp, sizeof cgrp);
        in[0] = inputs[0];
        in[0].buf = with.data;
        in[0].size = with.size;
        in[1] = inputs[0];
        expect_error("a cgrp in the first jp2h", in, 2, 0, "cgrp.placement at ");
        in[1] = in[0];
        in[0] = inputs[0];
        if (merge(in, 2, 0, &out, error, sizeof error) != 0)
            check(0, "a cgrp in a later jp2h: %s", error);
        else
            expect_served("a cgrp in a later jp2h", &out, in);
        bytes_free(&out);
        bytes_free(&with);
    }

    /* A box a JP2 file does not define, which the JPX file places elsewhere
     * (T.801 M.11.4: cref only in jpch, jplh or asoc): a cref in the first
     * input's jp2h, which is the JPX file's, and in a later input's res,
     * which goes into its jplh. */
    {
        static const uint8_t cref[12] = {0, 0, 0, 12, 'c', 'r', 'e', 'f', 'j', 'p', '2', 'i'};
        static const uint8_t res_cref[38] = {0, 0, 0, 38, 'r', 'e', 's', ' ',
                                             0, 0, 0, 18, 'r', 'e', 's', 'c',
                                             0, 1, 0, 1, 0, 1, 0, 1, 0, 0,
                                             0, 0, 0, 12, 'c', 'r', 'e', 'f',
                                             'j', 'p', '2', 'i'};
        bytes with = add_to_jp2h(&files[0], cref, sizeof cref);
        in[0] = inputs[0];
        in[0].buf = with.data;
        in[0].size = with.size;
        in[1] = inputs[0];
        expect_error("a cref in the first jp2h", in, 2, 0, "in the JPX file's jp2h");
        expect_error("a cref in the first jp2h", in, 2, 0, "cref.placement at ");
        bytes_free(&with);
        with = add_to_jp2h(&files[0], res_cref, sizeof res_cref);
        in[0] = inputs[0];
        in[1].buf = with.data;
        in[1].size = with.size;
        expect_error("a cref in a later res", in, 2, 0, "cref.placement at ");
        expect_error("a cref in a later res", in, 2, 0, "in its jplh");
        bytes_free(&with);
        in[1] = inputs[0];
    }

    /* A later input without the res of the first would inherit it. */
    bad = add_to_jp2h(&files[0], res, sizeof res);
    in[0].buf = bad.data;
    in[0].size = bad.size;
    in[1] = inputs[0];
    expect_error("no res after one", in, 2, 0, "no res box, but the first input has one");
    bytes_free(&bad);

    /* Colour Specification boxes repeated after the input's one, which
     * they must follow directly (jp2h.colr-contiguous). */
    hv_boxes_file(&it, files[0].data, files[0].size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_JP2H)
            break;
    hv_boxes_children(&it, files[0].data, &box);
    while (hv_boxes_next(&it, &colr, &message, &at) == 1)
        if (colr.type == HV_BOX_COLR)
            break;
    if (colr.type != HV_BOX_COLR || (colrs = malloc(16 * (colr.end - colr.start))) == NULL) {
        check(0, "no colr box to repeat");
        return;
    }
    for (k = 0; k < 16; k++)
        memcpy(colrs + k * (colr.end - colr.start), files[0].data + colr.start,
               colr.end - colr.start);
    /* Two with METH 1 are valid in a JP2 file, not in the JPX file's jp2h,
     * which is the first input's. */
    bad = insert_in_jp2h(&files[0], colr.end, colrs, colr.end - colr.start);
    in[0].buf = bad.data;
    in[0].size = bad.size;
    snprintf(error, sizeof error, "colr.one-method at %zu, in the JPX file's jp2h", colr.end);
    expect_error("two enumerated colr boxes, first input", in, 1, 0, error);
    /* A later input's go into a cgrp, which T.801 M.11.7.1 holds to the
     * same rule. */
    in[0] = inputs[0];
    in[1] = in[0];
    in[1].buf = bad.data;
    in[1].size = bad.size;
    snprintf(error, sizeof error, "colr.one-method at %zu, in its jplh's cgrp", colr.end);
    expect_error("two enumerated colr boxes, second input", in, 2, 0, error);
    bytes_free(&bad);
    /* Up to 16 are read (a JP2 file's are METH 1 or 2, so more than two
     * never make a valid cgrp). */
    bad = insert_in_jp2h(&files[0], colr.end, colrs, 15 * (colr.end - colr.start));
    in[1].buf = bad.data;
    in[1].size = bad.size;
    expect_error("16 colr boxes", in, 2, 0, error);
    bytes_free(&bad);
    /* I.5.3.3: the input reader ignores APPROX. The writer normalizes
     * it to 1, so input values 2 and 4 do not change the output profile. */
    bad = insert_in_jp2h(&files[0], colr.end, colrs, 0);
    in[1].buf = bad.data;
    in[1].size = bad.size;
    bad.data[colr.start + HV_BOX_HEADER + 2] = 2;
    if (merge(in, 2, 0, &out, error, sizeof error) != 0)
        check(0, "APPROX 2, second input: %s", error);
    else {
        expect_served("APPROX 2, second input", &out, in);
        bytes_free(&out);
    }
    in[0].buf = bad.data;
    in[0].size = bad.size;
    bad.data[colr.start + HV_BOX_HEADER + 2] = 4;
    if (merge(in, 1, 0, &out, error, sizeof error) != 0)
        check(0, "APPROX 4, first input: %s", error);
    else {
        expect_served("APPROX 4, first input", &out, in);
        bytes_free(&out);
    }
    in[0] = inputs[0];
    bytes_free(&bad);
    bad = insert_in_jp2h(&files[0], colr.end, colrs, 16 * (colr.end - colr.start));
    in[1].buf = bad.data;
    in[1].size = bad.size;
    expect_error("17 colr boxes", in, 2, 0, "more than 16 Colour Specification boxes");
    bytes_free(&bad);
    free(colrs);
}

static void expect_error(const char *name, const hv_merge_input *in, size_t n, int links,
                         const char *message) {
    bytes out;
    char error[512];
    int status = merge(in, n, links, &out, error, sizeof error);
    check(status != 0 && strstr(error, message) != NULL, "%s: \"%s\", expected \"%s\"", name,
          status ? error : "accepted", message);
    bytes_free(&out);
}

/* Inputs the served profile excludes, and what the merge itself needs. */
static void test_errors(void) {
    bytes origin = read_file(TRANSCODE_FIXTURES "/input/synthetic_rgb_129x129_origin129_CPRL.jp2");
    hv_merge_input in = {TRANSCODE_FIXTURES "/input/synthetic_rgb_129x129_origin129_CPRL.jp2",
                         origin.data, origin.size};
    bytes no_jp2h = {NULL, 0};
    hv_boxes it;
    hv_box box;
    const char *message;
    char dir[] = "/tmp/test_merge.XXXXXX", path[256];
    size_t at;
    FILE *f;

    expect_error("no inputs", inputs, 0, 0, "no JP2 input files");
    /* The limits, checked before any input is read. */
    expect_error("65,536 links", inputs, 65536, 1,
                 "a linked JPX file holds at most 65,535 links");
    expect_error("16,777,216 inputs", inputs, 16777216, 0,
                 "a JPX file holds at most 16,777,215 input files");
    expect_error("nonzero origins", &in, 1, 0, "siz.zero-origin");

    /* The first input without its JP2 Header box. */
    no_jp2h.data = malloc(files[0].size);
    hv_boxes_file(&it, files[0].data, files[0].size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == HV_BOX_JP2H) {
            memcpy(no_jp2h.data, files[0].data, box.start);
            memcpy(no_jp2h.data + box.start, files[0].data + box.end, files[0].size - box.end);
            no_jp2h.size = files[0].size - (box.end - box.start);
        }
    in.path = names[0];
    in.buf = no_jp2h.data;
    in.size = no_jp2h.size;
    expect_error("no jp2h", &in, 1, 0, "jp2.one-jp2h");

    /* A link outside the served profile: to a file that is not a .jp2. */
    if (mkdtemp(dir) != NULL) {
        snprintf(path, sizeof path, "%s/frame.j2k", dir);
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(files[0].data, 1, files[0].size, f);
            fclose(f);
            in = inputs[0];
            in.path = path;
            expect_error("link to a .j2k", &in, 1, 1, "url.jp2-target");
            unlink(path);
        }
        rmdir(dir);
    }
    bytes_free(&no_jp2h);
    bytes_free(&origin);
}

/* Links of 1,024 and 1,025 characters: the model's LOC bound of 1,024 is
 * a corpus bound only, the server has none, so both merge into a JPX file
 * within the served profile. */
static void test_long_link_urls(void) {
    char dir[] = "/tmp/test_merge_url.XXXXXX";
    char *root = mkdtemp(dir), *canonical = NULL;
    /* path is at most 1,011 characters, and "/ff.jp2" adds 7 */
    char path[1100] = "", short_path[sizeof path + 7] = "", long_path[sizeof path + 7] = "";
    hv_merge_input in = inputs[0];
    bytes out = {NULL, 0};
    char error[2048];
    FILE *file;
    size_t length;

    if (root == NULL || (canonical = realpath(root, NULL)) == NULL) {
        check(0, "cannot create a link-limit test directory");
        goto done;
    }
    strcpy(path, canonical);
    if (strlen(canonical) >= 1011) {
        check(0, "link-limit test directory is too long");
        goto done;
    }
    while ((length = strlen(path)) < 1011) {
        size_t remaining = 1011 - length;
        size_t chars = remaining - 1 < 100 ? remaining - 1 : 100;
        if (remaining - 1 - chars == 1)
            chars--;
        path[length++] = '/';
        memset(path + length, 'a', chars);
        path[length + chars] = 0;
        if (mkdir(path, 0700) != 0) {
            check(0, "cannot create a link-limit test path");
            goto done;
        }
    }
    snprintf(short_path, sizeof short_path, "%s/f.jp2", path);
    snprintf(long_path, sizeof long_path, "%s/ff.jp2", path);
    file = fopen(short_path, "wb");
    if (file == NULL) {
        check(0, "cannot create a 1,024-character link");
        goto done;
    }
    fclose(file);
    file = fopen(long_path, "wb");
    if (file == NULL) {
        check(0, "cannot create a 1,025-character link");
        goto done;
    }
    fclose(file);

    in.path = short_path;
    if (merge(&in, 1, 1, &out, error, sizeof error) != 0) {
        check(0, "1,024-character link: %s", error);
    } else {
        expect_served("1,024-character link", &out, &in);
    }
    bytes_free(&out);
    in.path = long_path;
    if (merge(&in, 1, 1, &out, error, sizeof error) != 0) {
        check(0, "1,025-character link: %s", error);
    } else {
        expect_served("1,025-character link", &out, &in);
    }
    bytes_free(&out);

done:
    if (short_path[0]) unlink(short_path);
    if (long_path[0]) unlink(long_path);
    if (canonical != NULL) {
        while (strcmp(path, canonical) != 0) {
            char *slash;
            rmdir(path);
            slash = strrchr(path, '/');
            if (slash == NULL) break;
            *slash = 0;
        }
        free(canonical);
    }
    if (root != NULL) rmdir(root);
}

/* Inputs opened on demand: counts what hv_merge_files opens, and can
 * fail a first or second open. */
typedef struct {
    int opens[NINPUTS], open, most;
    size_t fail_at, fail_second_at;  /* NINPUTS: never */
} counting;

static int counting_open(void *context, size_t i, hv_merge_input *in, char *error,
                         size_t error_size) {
    counting *c = context;
    if (i == c->fail_at || (i == c->fail_second_at && c->opens[i] > 0)) {
        snprintf(error, error_size, "cannot open %s", names[i]);
        return -1;
    }
    *in = inputs[i];
    c->opens[i]++;
    if (++c->open > c->most)
        c->most = c->open;
    return 0;
}

static void counting_close(void *context, size_t i, hv_merge_input *in) {
    counting *c = context;
    (void)i;
    (void)in;
    c->open--;
}

static int opening_validate;

static int merge_counting(counting *c, int links, char *error, size_t error_size) {
    hv_merge_inputs from = {counting_open, counting_close, NULL};
    FILE *f = tmpfile();
    int status;
    from.context = c;
    if (f == NULL) {
        snprintf(error, error_size, "no temporary file");
        return -1;
    }
    status = hv_merge_files(&from, NINPUTS, links, opening_validate ? HV_OUTPUT_JPIP : HV_OUTPUT_JPEG2000, f, error, error_size);
    fclose(f);
    return status;
}

static void test_opening_mode(void) {
    counting c;
    char error[512];
    size_t i;
    int links;

    for (i = 0; i < NINPUTS; i++)
        if (files[i].data == NULL)
            return;
    for (links = 0; links < 2; links++) {
        memset(&c, 0, sizeof c);
        c.fail_at = c.fail_second_at = NINPUTS;
        check(merge_counting(&c, links, error, sizeof error) == 0, "on demand: %s", error);
        check(c.most <= 2 && c.open == 0, "on demand: at most %d open, %d left open", c.most,
              c.open);
        for (i = 0; i < NINPUTS; i++)
            check(c.opens[i] == (i == 0 ? 1 : 2), "on demand: input %zu opened %d times", i,
                  c.opens[i]);
    }
    memset(&c, 0, sizeof c);
    c.fail_at = 3;
    c.fail_second_at = NINPUTS;
    check(merge_counting(&c, 0, error, sizeof error) != 0 && strstr(error, "cannot open") != NULL &&
              c.open == 0,
          "failed open: \"%s\", %d left open", error, c.open);
    for (links = 0; links < 2; links++) {
        memset(&c, 0, sizeof c);
        c.fail_at = NINPUTS;
        c.fail_second_at = links ? NINPUTS - 1 : 2;
        check(merge_counting(&c, links, error, sizeof error) != 0 &&
                  strstr(error, "cannot open") != NULL && c.open == 0 &&
                  c.opens[c.fail_second_at] == 1,
              "failed second open, links=%d: \"%s\", %d left open", links, error, c.open);
    }
}

static void test_headers(void) {
    for (test_validate = 0; test_validate < 2; test_validate++)
        test_headers_mode();
    test_validate = 1;
}

static void test_opening(void) {
    for (opening_validate = 0; opening_validate < 2; opening_validate++)
        test_opening_mode();
}

/* A tiled main header passes T.800 checks but not the served profile.
 * Tile data stays untouched: default merging checks only the main header. */
static void test_tiled_rreq(void) {
    static const int rsiz1[] = {1}, rsiz2[] = {1, 4};
    bytes jp2 = read_file(names[2]), out;
    hv_merge_input in = {names[2], jp2.data, jp2.size};
    hv_box jp2c;
    char error[512];
    size_t at;
    int rsiz;
    if (jp2.data == NULL || hv_served_jp2(jp2.data, jp2.size, &jp2c, &at) != NULL) {
        check(0, "cannot prepare tiled Rsiz test");
        bytes_free(&jp2);
        return;
    }
    for (rsiz = 1; rsiz <= 2; rsiz++) {
        unsigned tile_size = rsiz == 1 ? 128 : 1024;
        put16(jp2.data + jp2c.payload + 6, (unsigned)rsiz);
        put32(jp2.data + jp2c.payload + 24, tile_size);
        put32(jp2.data + jp2c.payload + 28, tile_size);
        if (merge_mode(&in, 1, 0, 0, &out, error, sizeof error) != 0) {
            check(0, "tiled Rsiz %d: %s", rsiz, error);
        } else {
            expect_features("tiled Rsiz", &out, rsiz == 1 ? rsiz1 : rsiz2,
                            rsiz == 1 ? 1 : 2);
        }
        bytes_free(&out);
        check(merge_mode(&in, 1, 0, 1, &out, error, sizeof error) != 0 &&
              strstr(error, "siz.single-tile") != NULL,
              "validation rejects tiled Rsiz %d: %s", rsiz, error);
        bytes_free(&out);
    }
    bytes_free(&jp2);
}

static void test_modes(void) {
    bytes normal, validated;
    char error[512];
    hv_box jp2c;
    hv_codestream cs;
    hv_item item;
    size_t at, last = 0;
    int links, mode;
    for (links = 0; links < 2; links++) {
        int a = merge_mode(inputs, NINPUTS, links, 0, &normal, error, sizeof error);
        int b = merge_mode(inputs, NINPUTS, links, 1, &validated, error, sizeof error);
        check(a == 0 && b == 0 && normal.size == validated.size &&
              memcmp(normal.data, validated.data, normal.size) == 0,
              "identical output in both modes, links=%d", links);
        bytes_free(&normal);
        bytes_free(&validated);
    }
    if (hv_served_jp2(inputs[0].buf, inputs[0].size, &jp2c, &at) != NULL ||
        hv_codestream_open(&cs, inputs[0].buf, jp2c.payload, jp2c.end, HV_READ_JPIP) != 0) {
        check(0, "cannot prepare PLT mode test");
        return;
    }
    while (hv_codestream_next(&cs, &item) == 1)
        if (item.plt != NULL) { last = item.plt->end - 1; break; }
    hv_codestream_close(&cs);
    if (last != 0) {
        hv_merge_input bad = inputs[0];
        uint8_t *copy = malloc(bad.size);
        memcpy(copy, bad.buf, bad.size);
        bad.buf = copy;
        copy[last] = 0x80;
        check(merge_mode(&bad, 1, 0, 0, &normal, error, sizeof error) == 0,
              "default accepts opaque malformed PLT: %s", error);
        if (normal.data != NULL) {
            hv_served_sources jpx;
            check(hv_served_jpx(normal.data, normal.size, &jpx, &at) == NULL,
                  "default output container remains valid");
            if (jpx.count == 1)
                check(jpx.jp2c[0].end - jpx.jp2c[0].payload == jp2c.end - jp2c.payload &&
                      memcmp(normal.data + jpx.jp2c[0].payload, copy + jp2c.payload,
                             jp2c.end - jp2c.payload) == 0,
                      "default copies malformed codestream byte for byte");
            hv_served_sources_free(&jpx);
        }
        bytes_free(&normal);
        check(merge_mode(&bad, 1, 0, 1, &validated, error, sizeof error) != 0 &&
              strstr(error, "invalid PLT") != NULL, "validation rejects malformed PLT: %s", error);
        bytes_free(&validated);
        copy[0] ^= 1;
        for (mode = 0; mode < 2; mode++) {
            check(merge_mode(&bad, 1, 0, mode, &normal, error, sizeof error) != 0,
                  "invalid container rejected in mode %d", mode);
            bytes_free(&normal);
        }
        free(copy);
    } else check(0, "fixture has no PLT");
}

/* Main-header bodies copied intact do not belong to header-box checking.
 * Full standard validation remains explicit and still rejects these cases. */
static void test_opaque_headers(void) {
    static const struct { const char *file, *rule; int validation; } cases[] = {
        {"jp2-rule-qcd.style-72.jp2", "qcd.style", 0},
        {"jp2-rule-qcc.component-69.jp2", "qcc.component", 0},
        {"jp2-rule-rgn.component-77.jp2", "rgn.component", 0},
        {"jp2-rule-crg.length-95.jp2", "invalid CRG", 0},
        {"jp2-rule-tlm.stlm-83.jp2", "invalid TLM", 0},
        {"jp2-rule-plm.length-87.jp2", "plm.length", 0},
        {"jp2-cod.sgcod.progression-5.jp2", "invalid COD", 1},
        {"jp2-cod.spcod.cbWidthExp-5.jp2", "cod.codeblock-area", 1},
        {"jp2-cod.spcod.cbStyle-64.jp2", "invalid COD", 1},
        {"jp2-siz.xtosiz-1.jp2", "siz.tile-origin", 1},
        {"jp2-siz.ytosiz-1.jp2", "siz.tile-origin", 1},
        {"jp2-rule-siz.tile-covers-origin-53.jp2", "siz.tile-covers-origin", 1}
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        char path[4096], error[512];
        hv_box input_cs;
        size_t at = 0;
        bytes input, output;
        int mode;
        snprintf(path, sizeof path, "%s/%s", JPEG2000_VECTORS, cases[i].file);
        input = read_file(path);
        hv_merge_input in = {path, input.data, input.size};
        const char *rule = hv_served_jp2(input.data, input.size, &input_cs, &at);
        check(rule == NULL, "%s: input framing: %s", cases[i].file, rule);
        if (rule != NULL) { bytes_free(&input); continue; }
        for (mode = 0; mode < 2; mode++) {
            int status = merge_mode(&in, 1, 0, mode, &output, error, sizeof error);
            if (mode && cases[i].validation) {
                check(status != 0 && strstr(error, cases[i].rule) != NULL,
                      "%s: explicit merge validation: %s", cases[i].file, error);
                bytes_free(&output);
                continue;
            }
            check(status == 0, "%s: merge mode %d: %s", cases[i].file, mode, error);
            if (status == 0) {
                hv_served_sources jpx;
                rule = hv_check_jpx_headers(output.data, output.size, &at);
                check(rule == NULL, "%s: generated header consistency: %s", cases[i].file, rule);
                rule = hv_served_jpx(output.data, output.size, &jpx, &at);
                check(rule == NULL && jpx.count == 1, "%s: output framing: %s", cases[i].file, rule);
                if (rule == NULL) {
                    if (jpx.count == 1) {
                        hv_box *out_cs = &jpx.jp2c[0];
                        size_t n = input_cs.end - input_cs.payload;
                        check(out_cs->end - out_cs->payload == n &&
                              memcmp(output.data + out_cs->payload, input.data + input_cs.payload, n) == 0,
                              "%s: copied codestream identity", cases[i].file);
                        rule = hv_codestream_check(output.data, out_cs->payload, out_cs->end, HV_READ_VALIDATE, &at);
                        check(rule != NULL && strcmp(rule, cases[i].rule) == 0,
                              "%s: explicit validation retained: %s", cases[i].file, rule);
                    }
                    hv_served_sources_free(&jpx);
                }
            }
            bytes_free(&output);
        }
        /* Opaque bodies still need a bounded length. */
        {
            hv_marker m;
            size_t pos = input_cs.payload;
            while (hv_marker_read(input.data, pos, input_cs.end, &m) == NULL && m.code != HV_QCD)
                pos = m.end;
            check(m.code == HV_QCD, "%s: QCD fixture marker", cases[i].file);
            if (m.code == HV_QCD) {
                put16(input.data + m.start + 2, 65535);
                check(merge_mode(&in, 1, 0, 0, &output, error, sizeof error) != 0,
                      "%s: opaque segment bounds retained", cases[i].file);
                bytes_free(&output);
            }
        }
        bytes_free(&input);
    }
}

/* Dropped bodies do not determine whether emitted boxes are usable. */
static void test_unused_boxes(void) {
    static const uint8_t ignored[][9] = {
        {0,0,0,9,'u','u','i','d',1},
        {0,0,0,9,'u','i','n','f',1}
    };
    bytes input = read_file(names[0]), baseline = {0}, output = {0};
    hv_merge_input in = {names[0], input.data, input.size};
    char error[512];
    size_t at = 0;
    if (input.data == NULL) return;
    check(merge_mode(&in, 1, 0, 0, &baseline, error, sizeof error) == 0,
          "unused-box baseline: %s", error);
    for (size_t i = 0; i < sizeof ignored / sizeof *ignored; i++) {
        bytes altered = {malloc(input.size + sizeof ignored[i]), input.size + sizeof ignored[i]};
        hv_header header;
        memcpy(altered.data, input.data, input.size);
        memcpy(altered.data + input.size, ignored[i], sizeof ignored[i]);
        in.buf = altered.data; in.size = altered.size;
        check(hv_read_jp2h(altered.data, altered.size, &header, &at) != NULL,
              "whole-file header checks retain dropped-body validation");
        for (int validate = 0; validate < 2; validate++) {
            check(merge_mode(&in, 1, 0, validate, &output, error, sizeof error) == 0,
                  "dropped box mode %d: %s", validate, error);
            check(output.data != NULL && baseline.data != NULL && output.size == baseline.size &&
                  memcmp(output.data, baseline.data, baseline.size) == 0,
                  "dropped body has no effect on emitted bytes");
            if (output.data != NULL)
                check(hv_check_jpx_headers(output.data, output.size, &at) == NULL,
                      "dropped body cannot invalidate output headers");
            bytes_free(&output);
        }
        /* The first source's header is copied; its malformed body must fail. */
        bytes copied = add_to_jp2h(&input, ignored[i], sizeof ignored[i]);
        in.buf = copied.data; in.size = copied.size;
        check(merge_mode(&in, 1, 0, 0, &output, error, sizeof error) != 0,
              "copied malformed header body rejected: %s", error);
        bytes_free(&output); bytes_free(&copied); bytes_free(&altered);
    }
    /* Header count/order remain merge input policy. */
    {
        hv_boxes boxes;
        hv_box box;
        const char *rule;
        hv_boxes_file(&boxes, input.data, input.size);
        while (hv_boxes_next(&boxes, &box, &rule, &at) == 1)
            if (box.type == HV_BOX_JP2H) {
                size_t n = box.end - box.start;
                bytes duplicate = {malloc(input.size + n), input.size + n};
                memcpy(duplicate.data, input.data, input.size);
                memcpy(duplicate.data + input.size, input.data + box.start, n);
                in.buf = duplicate.data; in.size = duplicate.size;
                check(merge_mode(&in, 1, 0, 0, &output, error, sizeof error) != 0 &&
                      strstr(error, "jp2.one-jp2h") != NULL,"duplicate header rejected: %s", error);
                bytes_free(&output); bytes_free(&duplicate);
                bytes late = {malloc(input.size), input.size};
                memcpy(late.data, input.data, box.start);
                memcpy(late.data + box.start, input.data + box.end, input.size - box.end);
                memcpy(late.data + input.size - n, input.data + box.start, n);
                in.buf = late.data; in.size = late.size;
                check(merge_mode(&in, 1, 0, 0, &output, error, sizeof error) != 0 &&
                      strstr(error, "jp2h.position") != NULL,"late header rejected: %s", error);
                bytes_free(&output); bytes_free(&late);
                break;
            }
    }
    /* IPR inside a dropped UINF cannot satisfy the emitted image's flag. */
    {
        static const uint8_t nested[] = {
            0,0,0,56,'u','i','n','f',
            0,0,0,26,'u','l','s','t',0,1,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0,0,13,'u','r','l',' ',0,0,0,0,0,
            0,0,0,9,'j','p','2','i',1
        };
        bytes altered = {malloc(input.size + sizeof nested), input.size + sizeof nested};
        hv_boxes boxes, children;
        hv_box box;
        hv_header header;
        const char *rule;
        memcpy(altered.data, input.data, input.size);
        memcpy(altered.data + input.size, nested, sizeof nested);
        hv_boxes_file(&boxes, altered.data, altered.size);
        while (hv_boxes_next(&boxes, &box, &rule, &at) == 1)
            if (box.type == HV_BOX_JP2H) {
                hv_boxes_children(&children, altered.data, &box);
                while (hv_boxes_next(&children, &box, &rule, &at) == 1)
                    if (box.type == HV_BOX_IHDR) altered.data[box.payload + 13] = 1;
            }
        check(hv_read_jp2h(altered.data, altered.size, &header, &at) == NULL,
              "nested IPR is available in the input file");
        in.buf = altered.data; in.size = altered.size;
        check(merge_mode(&in, 1, 0, 0, &output, error, sizeof error) != 0 &&
              strstr(error, "ihdr.ipr") != NULL,
              "dropped IPR cannot satisfy output availability: %s", error);
        bytes_free(&output); bytes_free(&altered);
    }
    /* Nested IPR in a copied Resolution box remains available in the output. */
    {
        static const uint8_t res[] = {
            0,0,0,35,'r','e','s',' ',
            0,0,0,18,'r','e','s','c',0,1,0,1,0,1,0,1,0,0,
            0,0,0,9,'j','p','2','i',1
        };
        bytes altered = add_to_jp2h(&input, res, sizeof res);
        hv_boxes boxes, children;
        hv_box box;
        hv_header header;
        const char *rule;
        hv_boxes_file(&boxes, altered.data, altered.size);
        while (hv_boxes_next(&boxes, &box, &rule, &at) == 1)
            if (box.type == HV_BOX_JP2H) {
                hv_boxes_children(&children, altered.data, &box);
                while (hv_boxes_next(&children, &box, &rule, &at) == 1)
                    if (box.type == HV_BOX_IHDR) altered.data[box.payload + 13] = 1;
            }
        check(hv_read_jp2h(altered.data, altered.size, &header, &at) == NULL,
              "nested Resolution IPR is valid input");
        hv_merge_input pair[] = {
            {names[0], input.data, input.size},
            {names[0], altered.data, altered.size}
        };
        for (int first = 0; first < 2; first++) {
            const hv_merge_input *sources = first ? &pair[1] : pair;
            size_t count = first ? 1 : 2;
            check(merge_mode(sources, count, 0, 0, &output, error, sizeof error) == 0,
                  "nested IPR survives copied %s: %s", first ? "JP2H" : "RES", error);
            if (output.data != NULL)
                check(hv_check_jpx_headers(output.data, output.size, &at) == NULL,
                      "copied nested IPR satisfies output availability");
            bytes_free(&output);
        }
        bytes_free(&altered);
    }
    bytes_free(&baseline); bytes_free(&input);
}

/* Copying into an embedded JPX does not need the input's primary JP2
 * brand. Linked output still requires the server's source-file contract. */
static void test_container_access(void) {
    bytes input = read_file(names[0]), ordinary = {0}, altered = {0};
    hv_merge_input in;
    hv_boxes it;
    hv_box box;
    const char *rule;
    size_t at;
    char error[1024];
    int found = 0;
    if (input.data == NULL)
        return;
    in.path = names[0];
    in.buf = input.data;
    in.size = input.size;
    check(merge_mode(&in, 1, 0, 0, &ordinary, error, sizeof error) == 0,
          "ordinary container baseline: %s", error);
    hv_boxes_file(&it, input.data, input.size);
    while (hv_boxes_next(&it, &box, &rule, &at) == 1) {
        if (box.type == HV_BOX_FTYP) {
            memcpy(input.data + box.payload, "test", 4);
            found = 1;
            break;
        }
    }
    check(found, "container fixture has ftyp");
    if (found) {
        check(merge_mode(&in, 1, 0, 0, &altered, error, sizeof error) == 0,
              "compatible primary brand embedded merge: %s", error);
        check(ordinary.size == altered.size && ordinary.data != NULL && altered.data != NULL &&
              memcmp(ordinary.data, altered.data, ordinary.size) == 0,
              "compatible primary brand identical generated JPX");
        bytes_free(&altered);
        check(merge_mode(&in, 1, 0, 1, &altered, error, sizeof error) != 0 &&
              strstr(error, "file.ftyp-brand") != NULL,
              "validated merge retains served brand: %s", error);
        bytes_free(&altered);
        check(merge_mode(&in, 1, 1, 0, &altered, error, sizeof error) != 0 &&
              strstr(error, "file.ftyp-brand") != NULL,
              "linked merge retains source brand: %s", error);
    }
    bytes_free(&altered);
    bytes_free(&ordinary);
    bytes_free(&input);
}

int main(void) {
    struct {
        const char *name;
        void (*run)(void);
    } groups[] = {
        {"hvJP2K reference", test_reference},
        {"default and validation modes", test_modes},
        {"general container access", test_container_access},
        {"opaque main-header bodies", test_opaque_headers},
        {"unused file boxes", test_unused_boxes},
        {"tiled Reader Requirements", test_tiled_rreq},
        {"linked merge", test_links},
        {"reader requirements", test_rreq},
        {"header boxes", test_headers},
        {"IPR boxes", test_ipr},
        {"XML with LBox = 0", test_xml_to_end},
        {"rejected inputs", test_errors},
        {"inputs opened on demand", test_opening},
        {"long linked URLs", test_long_link_urls},
    };
    size_t i;
    load_inputs();
    for (i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        int before = failures, n = checks;
        groups[i].run();
        printf("%-28s %5d checks, %d failed\n", groups[i].name, checks - n, failures - before);
    }
    for (i = 0; i < NINPUTS; i++)
        bytes_free(&files[i]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
