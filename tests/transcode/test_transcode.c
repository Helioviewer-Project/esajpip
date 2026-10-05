/* test_transcode.c: tests of hv_transcode_file and hv_transcode_codestream,
 * run by CTest as "transcode" (label "tools").
 *
 * TRANSCODE_FIXTURES holds JP2 files in input/ and, under the same names
 * in kakadu/, what kdu_transcode made of them (FIXTURES.md). With the
 * environment variable TRANSCODE_ARCHIVE set to directories separated by
 * ':', every .jp2 file in them is transcoded too. */
#define _POSIX_C_SOURCE 200809L

#include "jpeg2000/hv_served.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_writer.h"
#include "transcode/tier2.h"
#include "transcode/transcode.h"

#ifndef TRANSCODE_FIXTURES
#error "TRANSCODE_FIXTURES must name the fixture directory"
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

/* ------------------------------------------------------------------------
 * Byte buffers and files
 * ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t size;
} bytes;

static void bytes_free(bytes *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

static bytes bytes_copy(const uint8_t *data, size_t size) {
    bytes b;
    b.data = malloc(size ? size : 1);
    b.size = size;
    if (b.data == NULL)
        abort();
    memcpy(b.data, data, size);
    return b;
}

static void append(bytes *b, const void *data, size_t size) {
    uint8_t *grown = realloc(b->data, b->size + size + 1);
    if (grown == NULL)
        abort();
    b->data = grown;
    if (size)
        memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
}

static unsigned get16(const uint8_t *p) {
    return (unsigned)p[0] << 8 | p[1];
}

static bytes read_file(const char *path) {
    bytes b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    uint8_t chunk[65536];
    size_t n;
    if (f == NULL) {
        check(0, "cannot open %s", path);
        return b;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        append(&b, chunk, n);
    fclose(f);
    return b;
}

/* The payload of the first top-level box of `type`, or {NULL, 0}. */
static bytes box_payload(const bytes *file, uint32_t type) {
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    bytes b = {NULL, 0};
    hv_boxes_file(&it, file->data, file->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if (box.type == type) {
            b.data = file->data + box.payload;
            b.size = box.end - box.payload;
            break;
        }
    return b;
}

/* A codestream without its main-header COM segments, which Kakadu writes
 * its own of. */
static bytes without_com(bytes cs) {
    bytes out = bytes_copy(cs.data, 2);
    size_t p = 2;
    while (p + 4 <= cs.size && get16(cs.data + p) != HV_SOT) {
        size_t n = 2 + get16(cs.data + p + 2);
        if (get16(cs.data + p) != HV_COM)
            append(&out, cs.data + p, n);
        p += n;
    }
    append(&out, cs.data + p, cs.size - p);
    return out;
}

/* ------------------------------------------------------------------------
 * Running the transcoder
 * ------------------------------------------------------------------------ */

/* NULL if the codestream in buf[start, end) reads through with these
 * hv_codestream_open flags, otherwise the reader's error. */
static const char *read_error(const uint8_t *buf, size_t start, size_t end, unsigned flags) {
    size_t at;
    return hv_codestream_check(buf, start, end, flags, &at);
}

/* A transcoded file is within the whole served profile. */
static void expect_served(const char *name, const uint8_t *buf, size_t size) {
    hv_box jp2c;
    size_t at;
    const char *error = hv_served_jp2(buf, size, &jp2c, &at);
    if (error == NULL)
        error = read_error(buf, jp2c.payload, jp2c.end, HV_READ_JPIP);
    check(error == NULL, "%s: output outside the served profile: %s", name, error);
}

typedef struct {
    int status;
    char error[256];
    bytes out;
} result;

/* Transcodes a codestream without the profile. An output whose main header
 * is within the served profile must be within all of it: the rest is what
 * the transcoder writes. */
static result transcode(const uint8_t *data, size_t size, int ppx, int ppy) {
    result r;
    hv_out out;
    hv_out_init(&out);
    strcpy(r.error, "unset");
    r.status = hv_transcode_codestream(data, 0, size, ppx, ppy, HV_OUTPUT_JPEG2000, &out, r.error, sizeof r.error);
    if (r.status == 0)
        check(r.error[0] == 0, "an accepted transcode left the error \"%s\"", r.error);
    r.out.data = out.data;
    r.out.size = out.size;
    if (r.status != 0 && out.size != 0)
        check(0, "a failed transcode left %zu bytes of output", out.size);
    if (r.status == 0 && read_error(out.data, 0, out.size, HV_READ_PACKETS_JPIP) == NULL) {
        const char *error = read_error(out.data, 0, out.size, HV_READ_JPIP);
        check(error == NULL, "an output outside the served profile: %s", error);
    }
    return r;
}

/* Accepted, with this output. */
static void expect_output(const char *name, const bytes *input, const bytes *expected) {
    result r = transcode(input->data, input->size, 7, 7);
    check(r.status == 0, "%s: rejected: %s", name, r.error);
    if (r.status == 0)
        check(r.out.size == expected->size && memcmp(r.out.data, expected->data, r.out.size) == 0,
              "%s: output differs", name);
    bytes_free(&r.out);
}

/* Rejected with exactly this message. */
static void expect_error(const char *name, const bytes *input, const char *message) {
    result r = transcode(input->data, input->size, 7, 7);
    check(r.status != 0, "%s: accepted, expected \"%s\"", name, message);
    if (r.status != 0)
        check(strcmp(r.error, message) == 0, "%s: \"%s\", expected \"%s\"", name, r.error, message);
    bytes_free(&r.out);
}

/* Rejected by the reader with this message, at this offset. */
static void expect_reader_error(const char *name, const bytes *input, const char *message,
                                size_t at) {
    char expected[256];
    snprintf(expected, sizeof expected, "%s at %zu", message, at);
    expect_error(name, input, expected);
}

/* Transcoding an output again gives the same output. */
static void expect_stable(const char *name, const bytes *out) {
    result r = transcode(out->data, out->size, 7, 7);
    check(r.status == 0 && r.out.size == out->size && memcmp(r.out.data, out->data, out->size) == 0,
          "%s: output does not transcode to itself%s%s", name, r.status ? ": " : "",
          r.status ? r.error : "");
    bytes_free(&r.out);
}

/* ------------------------------------------------------------------------
 * The Kakadu references
 * ------------------------------------------------------------------------ */

/* The fixture outside the served profile: nonzero origins. */
#define ORIGIN_FIXTURE "synthetic_rgb_129x129_origin129_CPRL.jp2"

/* Every input transcodes to its Kakadu reference: the same boxes, the
 * codestream equal except COM, the XML box the input's before its NUL.
 * The origin fixture's file is rejected, and only its codestream is
 * compared. */
static void test_references(void) {
    char path[4096];
    DIR *dir;
    struct dirent *e;
    int files = 0;

    snprintf(path, sizeof path, "%s/input", TRANSCODE_FIXTURES);
    if ((dir = opendir(path)) == NULL) {
        check(0, "cannot open %s", path);
        return;
    }
    while ((e = readdir(dir)) != NULL) {
        size_t n = strlen(e->d_name);
        bytes input, ref, got_cs, ref_cs, a, b;
        hv_out out;
        hv_boxes it_out, it_ref;
        hv_box bo, br;
        const char *message;
        char error[256];
        size_t at;
        int more_out, more_ref;
        if (n < 4 || strcmp(e->d_name + n - 4, ".jp2") != 0)
            continue;
        files++;
        snprintf(path, sizeof path, "%s/input/%s", TRANSCODE_FIXTURES, e->d_name);
        input = read_file(path);
        snprintf(path, sizeof path, "%s/kakadu/%s", TRANSCODE_FIXTURES, e->d_name);
        ref = read_file(path);
        hv_out_init(&out);
        if (strcmp(e->d_name, ORIGIN_FIXTURE) == 0) {
            bytes in_cs = box_payload(&input, HV_BOX_JP2C);
            char expected[64];
            result r;
            snprintf(expected, sizeof expected, "siz.zero-origin at %zu",
                     (size_t)(in_cs.data - input.data) + 2);         /* SIZ */
            check(hv_transcode_file(input.data, input.size, 7, 7, HV_OUTPUT_JPIP, &out, error,
                                    sizeof error) != 0 &&
                  strcmp(error, expected) == 0 && out.size == 0,
                  "%s: file accepted or rejected otherwise: %s", e->d_name, error);
            r = transcode(in_cs.data, in_cs.size, 7, 7);
            check(r.status == 0, "%s: codestream rejected: %s", e->d_name, r.error);
            if (r.status == 0) {
                a = without_com(r.out);
                b = without_com(box_payload(&ref, HV_BOX_JP2C));
                check(a.size == b.size && memcmp(a.data, b.data, a.size) == 0,
                      "%s: codestream differs from Kakadu's (COM aside)", e->d_name);
                bytes_free(&a);
                bytes_free(&b);
                expect_stable(e->d_name, &r.out);
            }
            bytes_free(&r.out);
            hv_out_free(&out);
            bytes_free(&input);
            bytes_free(&ref);
            continue;
        }
        if (hv_transcode_file(input.data, input.size, 7, 7, HV_OUTPUT_JPIP, &out, error, sizeof error) != 0) {
            check(0, "%s: rejected: %s", e->d_name, error);
            hv_out_free(&out);
            bytes_free(&input);
            bytes_free(&ref);
            continue;
        }
        expect_served(e->d_name, out.data, out.size);
        /* The same boxes; each equal, the codestream except COM. */
        hv_boxes_file(&it_out, out.data, out.size);
        hv_boxes_file(&it_ref, ref.data, ref.size);
        for (;;) {
            more_out = hv_boxes_next(&it_out, &bo, &message, &at) == 1;
            more_ref = hv_boxes_next(&it_ref, &br, &message, &at) == 1;
            if (!more_out || !more_ref)
                break;
            check(bo.type == br.type, "%s: box order differs from the reference", e->d_name);
            if (bo.type != br.type)
                break;
            if (bo.type == HV_BOX_XML) {
                /* The input's XML box, before its NUL if it has one; the
                 * reference's is its root element alone, as hvJP2K
                 * reserialized it. */
                bytes xml = box_payload(&input, HV_BOX_XML);
                const uint8_t *nul = memchr(xml.data, 0, xml.size);
                size_t n = nul != NULL ? (size_t)(nul - xml.data) : xml.size;
                check(bo.end - bo.payload == n && memcmp(out.data + bo.payload, xml.data, n) == 0,
                      "%s: the XML box is not the input's, before its NUL", e->d_name);
                continue;
            }
            if (bo.type != HV_BOX_JP2C) {
                check(bo.end - bo.payload == br.end - br.payload &&
                      memcmp(out.data + bo.payload, ref.data + br.payload,
                             bo.end - bo.payload) == 0,
                      "%s: a box differs from the reference", e->d_name);
                continue;
            }
            got_cs.data = out.data + bo.payload;
            got_cs.size = bo.end - bo.payload;
            ref_cs.data = ref.data + br.payload;
            ref_cs.size = br.end - br.payload;
            a = without_com(got_cs);
            b = without_com(ref_cs);
            check(a.size == b.size && memcmp(a.data, b.data, a.size) == 0,
                  "%s: codestream differs from Kakadu's (COM aside)", e->d_name);
            bytes_free(&a);
            bytes_free(&b);
            expect_stable(e->d_name, &got_cs);
        }
        check(more_out == more_ref, "%s: box count differs from the reference", e->d_name);
        hv_out_free(&out);
        bytes_free(&input);
        bytes_free(&ref);
    }
    closedir(dir);
    check(files == 7, "expected 7 input fixtures, found %d", files);
}

/* ------------------------------------------------------------------------
 * Tile-parts and packets
 * ------------------------------------------------------------------------ */

/* The codestream of a fixture, which the tests take apart by its markers:
 * without it they cannot go on. */
static bytes fixture_codestream(const char *dir, const char *name, bytes *file) {
    char path[4096];
    bytes cs;
    snprintf(path, sizeof path, "%s/%s/%s", TRANSCODE_FIXTURES, dir, name);
    *file = read_file(path);
    cs = box_payload(file, HV_BOX_JP2C);
    if (cs.data == NULL || cs.size < 4) {
        fprintf(stderr, "FAIL: no codestream in %s\n", path);
        exit(1);
    }
    return cs;
}

/* A codestream split at its first SOT: the main header and the tile's
 * data after SOD, the tile-part's PLT lengths, and the whole. */
typedef struct {
    bytes file, cs, main, body;
    uint64_t *lengths;
    size_t nlengths;
} split;

static void split_free(split *s) {
    bytes_free(&s->file);
    free(s->lengths);
    s->lengths = NULL;
}

static void split_codestream(split *s, const char *dir, const char *name) {
    size_t sot, p, end;
    uint64_t v = 0;
    memset(s, 0, sizeof *s);
    s->cs = fixture_codestream(dir, name, &s->file);
    for (sot = 2; get16(s->cs.data + sot) != HV_SOT; sot += 2 + get16(s->cs.data + sot + 2)) {}
    s->main.data = s->cs.data;
    s->main.size = sot;
    for (p = sot + 12; get16(s->cs.data + p) != HV_SOD; p += 2 + get16(s->cs.data + p + 2)) {
        size_t k;
        if (get16(s->cs.data + p) != HV_PLT)
            continue;
        end = p + 2 + get16(s->cs.data + p + 2);
        for (k = p + 5; k < end; k++) {
            v = v << 7 | (s->cs.data[k] & 0x7F);
            if (s->cs.data[k] < 0x80) {
                uint64_t *grown = realloc(s->lengths, (s->nlengths + 1) * sizeof *grown);
                if (grown == NULL)
                    abort();
                s->lengths = grown;
                s->lengths[s->nlengths++] = v;
                v = 0;
            }
        }
    }
    s->body.data = s->cs.data + p + 2;
    s->body.size = s->cs.size - 2 - (p + 2);
}

/* SOT, SOD and data of one tile-part; Psot = 0 when psot_zero. */
static void tile_part(bytes *b, unsigned isot, unsigned tpsot, unsigned tnsot,
                      const uint8_t *data, size_t size, int psot_zero) {
    uint8_t h[14];
    put16(h, HV_SOT);
    put16(h + 2, 10);
    put16(h + 4, isot);
    put32(h + 6, psot_zero ? 0 : (uint32_t)(14 + size));
    h[10] = (uint8_t)tpsot;
    h[11] = (uint8_t)tnsot;
    put16(h + 12, HV_SOD);
    append(b, h, 14);
    append(b, data, size);
}

static bytes codestream(const bytes *main) {
    return bytes_copy(main->data, main->size);
}

static void eoc(bytes *b) {
    static const uint8_t code[2] = {0xFF, 0xD9};
    append(b, code, 2);
}

/* The integrity cases: one tile split into tile-parts every way T.800
 * allows, and broken in the ways it does not. */
static void test_tile_parts(void) {
    split s;
    bytes expected, b;
    size_t first, total = 0, i;
    result r;

    split_codestream(&s, "kakadu", "2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2");
    for (i = 0; i < s.nlengths; i++)
        total += s.lengths[i];
    check(s.nlengths > 0 && total == s.body.size, "AIA reference: PLT does not cover its tile");
    first = (size_t)s.lengths[0];
    r = transcode(s.cs.data, s.cs.size, 7, 7);
    check(r.status == 0, "AIA reference: rejected: %s", r.error);
    expected = r.out;

    b = codestream(&s.main);                    /* empty parts around two */
    tile_part(&b, 0, 0, 4, NULL, 0, 0);
    tile_part(&b, 0, 1, 4, s.body.data, first, 0);
    tile_part(&b, 0, 2, 4, s.body.data + first, s.body.size - first, 0);
    tile_part(&b, 0, 3, 4, NULL, 0, 0);
    eoc(&b);
    expect_output("four tile-parts, two empty", &b, &expected);
    bytes_free(&b);

    b = codestream(&s.main);                    /* the last part with Psot = 0 */
    tile_part(&b, 0, 0, 2, s.body.data, first, 0);
    tile_part(&b, 0, 1, 2, s.body.data + first, s.body.size - first, 1);
    eoc(&b);
    expect_output("Psot = 0 on the last tile-part", &b, &expected);
    b.size -= 2;
    expect_reader_error("Psot = 0 without EOC", &b,
                        "Psot = 0 but the codestream does not end with EOC",
                        s.main.size + 14 + first);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 0, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_output("TNsot = 0", &b, &expected);
    bytes_free(&b);

    b = codestream(&s.main);                    /* all 255 TPsot numbers */
    for (i = 0; i < 254; i++)
        tile_part(&b, 0, (unsigned)i, 0, NULL, 0, 0);
    first = b.size;
    tile_part(&b, 0, 254, 0, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_output("255 tile-parts", &b, &expected);
    b.size = first;                             /* and a 256th */
    tile_part(&b, 0, 254, 0, NULL, 0, 0);
    tile_part(&b, 0, 255, 0, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_reader_error("256 tile-parts", &b, "invalid SOT", first + 14);
    first = (size_t)s.lengths[0];
    bytes_free(&b);

    b = codestream(&s.main);                    /* XTsiz 2,048: two tiles */
    put32(b.data + 24, 2048);
    tile_part(&b, 0, 0, 1, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_error("two tiles", &b, "only single-tile codestreams are supported");
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 2, s.body.data, first - 1, 0);
    tile_part(&b, 0, 1, 2, s.body.data + first - 1, s.body.size - first + 1, 0);
    eoc(&b);
    expect_error("a packet across two tile-parts", &b, "packet crosses a tile-part boundary");
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 1, s.body.data, s.body.size, 0);
    append(&b, "", 1);                          /* inside the tile-part */
    put32(b.data + s.main.size + 6, (uint32_t)(14 + s.body.size + 1));
    eoc(&b);
    expect_error("a byte after the last packet", &b, "1 unparsed tile bytes");
    bytes_free(&b);

    b = bytes_copy(s.cs.data, s.cs.size);
    append(&b, "", 1);
    expect_reader_error("a byte after EOC", &b, "codestream.extent", s.cs.size);
    b.size = s.cs.size - 2;
    expect_reader_error("no EOC", &b, "tile-part overruns the codestream", s.main.size);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 1, 0, 1, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_reader_error("tile 1 of 1", &b, "sot.isot-range", s.main.size);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 1, 1, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_reader_error("tile-part 1 first", &b, "sot.tpsot-sequence", s.main.size);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 2, s.body.data, s.body.size, 0);
    eoc(&b);
    expect_reader_error("one of two tile-parts", &b, "sot.tnsot-count", b.size - 2);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 2, s.body.data, first, 0);
    tile_part(&b, 0, 1, 3, s.body.data + first, s.body.size - first, 0);
    eoc(&b);
    expect_reader_error("TNsot 2 then 3", &b, "sot.tnsot-inconsistent",
                        s.main.size + 14 + first);
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 1, s.body.data, s.body.size - (size_t)s.lengths[s.nlengths - 1], 0);
    eoc(&b);
    expect_error("without the last packet", &b,
                 "truncated input is not supported: missing packets");
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 1, s.body.data, s.body.size - 1, 0);
    eoc(&b);
    expect_error("without the last byte", &b, "truncated input is not supported: missing packets");
    bytes_free(&b);

    b = codestream(&s.main);                    /* Psot = 0 before the last */
    tile_part(&b, 0, 0, 0, s.body.data, first, 1);
    tile_part(&b, 0, 1, 0, s.body.data + first, s.body.size - first, 0);
    eoc(&b);
    expect_error("Psot = 0 on the first of two", &b, "Psot=0 is only valid for the last tile-part");
    bytes_free(&b);

    b = codestream(&s.main);
    tile_part(&b, 0, 0, 2, s.body.data, first, 1);
    tile_part(&b, 0, 1, 2, s.body.data + first, s.body.size - first, 0);
    eoc(&b);
    expect_reader_error("Psot = 0 on the first of two, TNsot = 2", &b, "sot.tnsot-count",
                        b.size - 2);
    bytes_free(&b);

    bytes_free(&expected);
    split_free(&s);

    /* A run of 1 bits (0xFF 0x7F pairs, legal stuffing) grows Lblock past
     * 62 bits: the length saturates instead of wrapping around. */
    split_codestream(&s, "input", "synthetic_rgb_129x129_origin129_CPRL.jp2");
    b = codestream(&s.main);
    {
        bytes tile = bytes_copy(s.body.data, 2917);
        static const uint8_t ones[2] = {0xFF, 0x7F};
        for (i = 0; i < 20; i++)
            append(&tile, ones, 2);
        append(&tile, s.body.data + 2932, s.body.size - 2932);
        tile_part(&b, 0, 0, 1, tile.data, tile.size, 0);
        bytes_free(&tile);
    }
    eoc(&b);
    expect_error("Lblock past 62 bits", &b,
                 "truncated input is not supported: packet data overruns the tile");
    bytes_free(&b);
    split_free(&s);
}

/* ------------------------------------------------------------------------
 * Main headers
 * ------------------------------------------------------------------------ */

/* rgb with `size` bytes at `offset` replaced by `value`. */
static bytes patched(const bytes *rgb, size_t offset, const void *value, size_t size) {
    bytes b = bytes_copy(rgb->data, rgb->size);
    memcpy(b.data + offset, value, size);
    return b;
}

static bytes concat3(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                     const uint8_t *c, size_t nc) {
    bytes out = bytes_copy(a, na);
    append(&out, b, nb);
    append(&out, c, nc);
    return out;
}

/* Patched, rejected by the reader at `at`. */
#define EXPECT_PATCHED(name, offset, value, message, at)                    \
    do {                                                                     \
        static const uint8_t v_[] = value;                                   \
        bytes b_ = patched(&rgb, (offset), v_, sizeof v_);                    \
        expect_reader_error((name), &b_, (message), (at));                   \
        bytes_free(&b_);                                                     \
    } while (0)
#define BYTES(...) {__VA_ARGS__}

static void test_headers(void) {
    bytes file, cs = fixture_codestream("input", "synthetic_rgb_129x129_origin129_CPRL.jp2", &file);
    bytes rgb = bytes_copy(cs.data, cs.size), b;
    size_t siz = 2, cod = siz + 2 + get16(rgb.data + siz + 2), sot = cod, n;
    static const char *too_many = "code-block count exceeds supported limit (250,000)";
    uint8_t *p;

    while (get16(rgb.data + sot) != HV_SOT)
        sot += 2 + get16(rgb.data + sot + 2);

    /* Output profiles are choices, not reader flags. */
    {
        static const hv_output_profile profiles[] = {(hv_output_profile)-1, (hv_output_profile)2};
        char error[256];
        hv_out out;
        for (size_t i = 0; i < sizeof profiles / sizeof *profiles; i++) {
            hv_out_init(&out);
            check(hv_transcode_codestream(rgb.data, 0, rgb.size, 7, 7, profiles[i], &out, error,
                                          sizeof error) != 0 &&
                  strcmp(error, "invalid output profile") == 0 && out.size == 0,
                  "invalid output profile: %s", error);
            hv_out_free(&out);
        }
    }

    b.data = rgb.data;
    b.size = 0;
    expect_reader_error("empty", &b, "codestream does not start with SOC", 0);
    b.size = 4;
    expect_reader_error("SOC and SIZ code only", &b, "truncated SIZ", siz);
    b = concat3(rgb.data, 2, rgb.data + cod, rgb.size - cod, NULL, 0);
    expect_reader_error("no SIZ", &b, "SOC is not followed by SIZ", siz);
    bytes_free(&b);
    b = concat3(rgb.data, cod, rgb.data + sot, rgb.size - sot, NULL, 0);
    expect_reader_error("no COD or QCD", &b, "codestream.one-cod-before-sot", cod);
    bytes_free(&b);
    b = concat3(rgb.data, cod, rgb.data + siz, cod - siz, rgb.data + cod, rgb.size - cod);
    expect_reader_error("two SIZ", &b, "main.marker-code", cod);
    bytes_free(&b);
    b = concat3(rgb.data, sot, rgb.data + cod, sot - cod, rgb.data + sot, rgb.size - sot);
    expect_reader_error("two COD", &b, "codestream.one-cod-before-sot", sot);
    bytes_free(&b);

    EXPECT_PATCHED("Lsiz 2", siz + 2, BYTES(0x00, 0x02), "invalid SIZ", siz);
    EXPECT_PATCHED("Lsiz 65,535", siz + 2, BYTES(0xFF, 0xFF), "truncated SIZ", siz);
    EXPECT_PATCHED("Csiz 0", siz + 4 + 34, BYTES(0x00, 0x00), "invalid SIZ", siz);
    EXPECT_PATCHED("Csiz 4 of 3", siz + 4 + 34, BYTES(0x00, 0x04),
                   "siz.csiz-count", siz);
    EXPECT_PATCHED("XOsiz past Xsiz", siz + 4 + 10, BYTES(0x00, 0x00, 0x01, 0x02),
                   "siz.origin-inside", siz);
    EXPECT_PATCHED("XTsiz 0", siz + 4 + 18, BYTES(0x00, 0x00, 0x00, 0x00), "invalid SIZ", siz);
    EXPECT_PATCHED("XRsiz 0", siz + 4 + 37, BYTES(0x00), "invalid SIZ", siz);

    /* Components subsampled so much that coarse resolutions are empty. */
    b = bytes_copy(rgb.data, rgb.size);
    b.data[siz + 4 + 40] = 174;
    b.data[siz + 4 + 44] = 255;
    expect_error("empty resolutions", &b, "5569 unparsed tile bytes");
    bytes_free(&b);

    EXPECT_PATCHED("Lcod 0", cod + 2, BYTES(0x00, 0x00), "marker segment overruns its header",
                   cod);
    /* One precinct byte short. */
    n = get16(rgb.data + cod + 2);
    b = concat3(rgb.data, cod + n + 1, rgb.data + cod + n + 2, rgb.size - cod - n - 2, NULL, 0);
    put16(b.data + cod + 2, (unsigned)n - 1);
    expect_reader_error("short COD", &b, "cod.precincts-count", cod);
    bytes_free(&b);
    EXPECT_PATCHED("Scod reserved bit", cod + 4, BYTES(0x08), "invalid COD", cod);
    EXPECT_PATCHED("progression 5", cod + 4 + 1, BYTES(0x05), "invalid COD", cod);
    EXPECT_PATCHED("0 layers", cod + 4 + 2, BYTES(0x00, 0x00), "invalid COD", cod);
    EXPECT_PATCHED("66 levels", cod + 4 + 5, BYTES(0x42), "invalid COD", cod);
    EXPECT_PATCHED("xcb 11", cod + 4 + 6, BYTES(0x09), "invalid COD", cod);
    EXPECT_PATCHED("xcb + ycb 13", cod + 4 + 7, BYTES(0x05), "cod.codeblock-area", cod);
    EXPECT_PATCHED("PPy 0 at resolution 1", cod + 4 + 11, BYTES(0x08),
                   "cod.precincts-higher-zero", cod);

    /* A huge image and many layers with little data. */
    b = bytes_copy(rgb.data, rgb.size);
    put32(b.data + siz + 4 + 2, 0x7FFFFFFF);
    put32(b.data + siz + 4 + 18, 0x7FFFFFFF);
    put16(b.data + cod + 4 + 2, 60164);
    expect_error("more packets than bytes", &b, "packet count exceeds tile data");
    bytes_free(&b);
    /* Large sparse image, largest precincts: too many code-blocks. */
    b = bytes_copy(rgb.data, rgb.size);
    p = b.data + siz + 4;
    put32(p + 2, 65536);                        /* Xsiz, Ysiz */
    put32(p + 6, 65536);
    put32(p + 18, 65536);                       /* XTsiz, YTsiz */
    put32(p + 22, 65536);
    memset(b.data + cod + 4 + 10, 0xFF, 3);     /* 2^15 precincts */
    expect_error("too many code-blocks", &b, too_many);
    put16(b.data + cod + 4 + 2, 200);
    expect_error("too many code-blocks, 200 layers", &b, too_many);
    bytes_free(&b);

    bytes_free(&rgb);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * The served profile: what hv_transcode_file accepts
 * ------------------------------------------------------------------------ */

static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

/* A minimal JP2 file around a codestream: signature, file type with this
 * brand and compatibility entry, a JP2 Header box as SIZ describes the
 * image (ihdr; colr greyscale or sRGB; its components share one depth),
 * codestream box. */
static bytes jp2_file(const bytes *cs, const char *brand) {
    uint8_t h[32] = {0, 0, 0, 12, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A,
                     0, 0, 0, 20, 'f', 't', 'y', 'p'};
    uint8_t jp2h[45] = {0, 0, 0, 45, 'j', 'p', '2', 'h', 0, 0, 0, 22, 'i', 'h', 'd', 'r'};
    const uint8_t *siz = cs->data + 4;          /* Lsiz */
    unsigned nc = get16(siz + 36);
    bytes b = {NULL, 0};
    memcpy(h + 20, brand, 4);
    memset(h + 24, 0, 4);
    memcpy(h + 28, brand, 4);
    put32(jp2h + 16, get32(siz + 8) - get32(siz + 16));     /* Ysiz - YOsiz */
    put32(jp2h + 20, get32(siz + 4) - get32(siz + 12));     /* Xsiz - XOsiz */
    put16(jp2h + 24, nc);
    jp2h[26] = siz[38];                                     /* Ssiz of component 0 */
    jp2h[27] = 7;
    memcpy(jp2h + 30, "\0\0\0\x0F" "colr" "\x01\0\0", 11);   /* METH 1, PREC, APPROX */
    put32(jp2h + 41, nc == 1 ? 17 : 16);                    /* greyscale or sRGB */
    append(&b, h, sizeof h);
    append(&b, jp2h, sizeof jp2h);
    put32(h, (uint32_t)(8 + cs->size));
    memcpy(h + 4, "jp2c", 4);
    append(&b, h, 8);
    append(&b, cs->data, cs->size);
    return b;
}

/* The file is rejected, with an error that starts with `message`. */
static void expect_file_error(const char *name, const bytes *file, const char *message) {
    hv_out out;
    char error[256];
    hv_out_init(&out);
    check(hv_transcode_file(file->data, file->size, 7, 7, HV_OUTPUT_JPIP, &out, error, sizeof error) != 0 &&
          strncmp(error, message, strlen(message)) == 0 && out.size == 0,
          "%s: \"%s\", expected \"%s\"", name, out.size ? "accepted" : error, message);
    hv_out_free(&out);
}

/* The file is accepted, the output is served and its codestream is the
 * codestream-level transcode. */
static void expect_file_output(const char *name, const bytes *file, const bytes *cs) {
    hv_out out;
    char error[256];
    result r = transcode(cs->data, cs->size, 7, 7);
    hv_out_init(&out);
    if (hv_transcode_file(file->data, file->size, 7, 7, HV_OUTPUT_JPIP, &out, error, sizeof error) != 0) {
        check(0, "%s: rejected: %s", name, error);
    } else {
        bytes got = {out.data, out.size}, got_cs = box_payload(&got, HV_BOX_JP2C);
        expect_served(name, out.data, out.size);
        check(r.status == 0 && got_cs.size == r.out.size &&
              memcmp(got_cs.data, r.out.data, got_cs.size) == 0,
              "%s: codestream differs from the codestream-level transcode", name);
    }
    hv_out_free(&out);
    bytes_free(&r.out);
}

static void test_served_profile(void) {
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    bytes b, f;

    /* The fixture, wrapped minimally: accepted. */
    f = jp2_file(&cs, "jp2 ");
    expect_file_output("minimal JP2", &f, &cs);
    bytes_free(&f);

    /* Scale the reference grid and sampling together: the component grid
     * and compressed samples stay identical. Raw output keeps that layout;
     * the serving file target requires unit sampling. */
    {
        result baseline = transcode(cs.data, cs.size, 7, 7);
        check(baseline.status == 0, "sampling baseline: %s", baseline.error);
        b = bytes_copy(cs.data, cs.size);
        if (baseline.status == 0) {
            bytes expected = bytes_copy(baseline.out.data, baseline.out.size);
            for (size_t axis = 0; axis < 2; axis++) {
                unsigned scale = (unsigned)axis + 2;
                size_t extent = 8 + 4 * axis, tile = 24 + 4 * axis;
                put32(b.data + extent, get32(b.data + extent) * scale);
                put32(b.data + tile, get32(b.data + tile) * scale);
                b.data[43 + axis] = (uint8_t)scale;
                put32(expected.data + extent, get32(expected.data + extent) * scale);
                put32(expected.data + tile, get32(expected.data + tile) * scale);
                expected.data[43 + axis] = (uint8_t)scale;
            }
            expect_output("raw component sampling", &b, &expected);
            const char *message = read_error(expected.data, 0, expected.size, 0);
            check(message == NULL, "subsampled raw output validation: %s",
                  message ? message : "");
            f = jp2_file(&b, "jp2 ");
            expect_file_error("serving component sampling", &f, "siz.component-sampling at ");
            hv_out general;
            char error[256];
            size_t at = 0;
            hv_out_init(&general);
            check(hv_transcode_file(f.data, f.size, 7, 7, HV_OUTPUT_JPEG2000,
                                    &general, error, sizeof error) == 0,
                  "general JP2 preserves component sampling: %s", error);
            bytes container = {general.data, general.size};
            bytes stream = box_payload(&container, HV_BOX_JP2C);
            check(stream.size == expected.size &&
                  memcmp(stream.data, expected.data, stream.size) == 0,
                  "general file/raw profiles produce identical codestreams");
            check(hv_check_jp2h(general.data, general.size, &at) == NULL,
                  "general output has consistent JP2 headers");
            hv_out_free(&general);
            bytes_free(&f);
            bytes_free(&expected);
        }
        bytes_free(&b);
        bytes_free(&baseline.out);
    }

    /* Not JP2 files. */
    expect_file_error("raw codestream", &cs, "file.signature at 0");
    f = jp2_file(&cs, "jpx ");
    expect_file_error("JPX brand", &f, "file.ftyp-brand at 12");
    bytes_free(&f);
    {
        bytes box = box_payload(&file, HV_BOX_JP2C);           /* 8-byte box header */
        b = bytes_copy(file.data, file.size);
        append(&b, box.data - 8, box.size + 8);
        expect_file_error("two codestreams", &b, "jp2.one-codestream");
        bytes_free(&b);
    }
    bytes_free(&file);
}

/* Packet layout preserves Rsiz. Declaration validation stays separate from
 * transcode operation requirements, including unsupported coding markers. */
/* File and raw operations share the output target, including nonzero origins. */
static void test_file_profiles(void) {
    bytes file, cs = fixture_codestream("input", "synthetic_rgb_129x129_origin129_CPRL.jp2", &file);
    result raw = transcode(cs.data, cs.size, 7, 7);
    hv_out out;
    char error[256];
    size_t at = 0;
    hv_out_init(&out);
    check(raw.status == 0, "origin raw transcode: %s", raw.error);
    check(hv_transcode_file(file.data, file.size, 7, 7, HV_OUTPUT_JPEG2000,
                            &out, error, sizeof error) == 0,
          "general file origin transcode: %s", error);
    bytes container = {out.data, out.size};
    bytes stream = box_payload(&container, HV_BOX_JP2C);
    check(raw.status == 0 && stream.size == raw.out.size &&
          memcmp(stream.data, raw.out.data, stream.size) == 0,
          "origin file/raw output codestream identity");
    check(hv_check_jp2h(out.data, out.size, &at) == NULL,
          "origin output header consistency");
    size_t prefix = out.size;
    check(hv_transcode_file(file.data, file.size, 7, 7, HV_OUTPUT_JPIP,
                            &out, error, sizeof error) != 0 && out.size == prefix,
          "JPIP rejects origin without changing prior output");
    check(hv_transcode_file(file.data, file.size, 7, 7, (hv_output_profile)99,
                            &out, error, sizeof error) != 0 && out.size == prefix &&
          strcmp(error, "invalid output profile") == 0,
          "invalid file profile preserves prior output");
    hv_out_free(&out); bytes_free(&raw.out); bytes_free(&file);
}

static void test_declaration_policy(void) {
    static const struct { unsigned value; const char *error; } declarations[] = {
        {3, "siz.unsupported-profile"},
        {0x4000, "siz.unsupported-capabilities"},
        {0xffff, "siz.unsupported-capabilities"}
    };
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    bytes wrapped = jp2_file(&cs, "jp2 ");
    hv_out baseline;
    char error[256];
    hv_out_init(&baseline);
    check(hv_transcode_file(wrapped.data, wrapped.size, 7, 7, HV_OUTPUT_JPIP, &baseline, error, sizeof error) == 0,
          "Rsiz baseline: %s", error);
    bytes_free(&wrapped);
    for (size_t i = 0; i < sizeof declarations / sizeof *declarations; i++) {
        bytes altered = bytes_copy(cs.data, cs.size);
        hv_codestream reader;
        hv_out output;
        put16(altered.data + 6, declarations[i].value);
        check(hv_codestream_open(&reader, altered.data, 0, altered.size, HV_READ_VALIDATE) != 0 &&
              strcmp(reader.error, declarations[i].error) == 0,
              "standard validator retains Rsiz 0x%04X rejection", declarations[i].value);
        hv_codestream_close(&reader);
        check(hv_codestream_open(&reader, altered.data, 0, altered.size, HV_READ_PACKETS) == 0,
              "packet access admits Rsiz 0x%04X", declarations[i].value);
        hv_codestream_close(&reader);
        expect_reader_error("raw writer retains declaration support limit", &altered,
                            declarations[i].error, 2);
        if (declarations[i].value == 0x4000) {
            static const uint8_t cap[] = {0xff,0x50,0,2};
            size_t end_siz = 4 + get16(altered.data + 4);
            bytes extension = bytes_copy(altered.data, end_siz);
            append(&extension, cap, sizeof cap);
            append(&extension, altered.data + end_siz, altered.size - end_siz);
            hv_out unsupported;
            hv_out_init(&unsupported);
            check(hv_transcode_codestream(extension.data, 0, extension.size, 7, 7,
                                          HV_OUTPUT_JPIP, &unsupported, error, sizeof error) != 0 &&
                  strcmp(error, "unsupported main header marker 0xFF50") == 0 &&
                  unsupported.size == 0,
                  "serving writer still rejects CAP: %s", error);
            hv_out_free(&unsupported); bytes_free(&extension);
        }
        wrapped = jp2_file(&altered, "jp2 ");
        hv_out_init(&output);
        check(hv_transcode_file(wrapped.data, wrapped.size, 7, 7, HV_OUTPUT_JPIP, &output, error, sizeof error) == 0,
              "serving transcode preserves Rsiz 0x%04X: %s", declarations[i].value, error);
        if (output.size != 0 && baseline.size != 0) {
            bytes expected = bytes_copy(baseline.data, baseline.size);
            bytes stream = box_payload(&expected, HV_BOX_JP2C);
            size_t at;
            put16(stream.data + 6, declarations[i].value);
            check(output.size == expected.size && memcmp(output.data, expected.data, expected.size) == 0,
                  "Rsiz 0x%04X changes only its original two bytes", declarations[i].value);
            expect_served("preserved Rsiz", output.data, output.size);
            check(hv_check_jp2h(output.data, output.size, &at) == NULL,
                  "preserved Rsiz output header consistency");
            bytes_free(&expected);
        }
        hv_out_free(&output); bytes_free(&wrapped); bytes_free(&altered);
    }
    hv_out_free(&baseline); bytes_free(&file);
}

/* The boxes around the codestream: copied as read and in order, LBox = 0
 * included; JP2 box children checked; extension bodies opaque; XML cut at a NUL. */
static void test_boxes(void) {
    /* A last superbox with LBox = 0 whose last child has LBox = 0 too. */
    static const uint8_t to_end[] = {0, 0, 0, 0, 'a', 's', 'o', 'c',
                                     0, 0, 0, 0, 'l', 'b', 'l', ' ', 'x'};
    static const uint8_t bad_child[] = {0, 0, 0, 17, 'u', 'i', 'n', 'f',
                                        0, 0, 0, 7, 'u', 'l', 's', 't', 'x'};
    static const uint8_t bad_xml[] = {0, 0, 0, 11, 'x', 'm', 'l', ' ', '<', 'a', '>'};
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    bytes f = jp2_file(&cs, "jp2 "), g;
    hv_out once, twice;
    char error[256];

    g = bytes_copy(f.data, f.size);
    append(&g, to_end, sizeof to_end);
    hv_out_init(&once);
    hv_out_init(&twice);
    if (hv_transcode_file(g.data, g.size, 7, 7, HV_OUTPUT_JPIP, &once, error, sizeof error) != 0) {
        check(0, "LBox = 0 superbox: rejected: %s", error);
    } else {
        expect_served("LBox = 0 superbox", once.data, once.size);
        check(once.size >= sizeof to_end &&
              memcmp(once.data + once.size - sizeof to_end, to_end, sizeof to_end) == 0,
              "LBox = 0 superbox: not copied as read");
        error[0] = 0;
        check(hv_transcode_file(once.data, once.size, 7, 7, HV_OUTPUT_JPIP, &twice, error, sizeof error) == 0 &&
              twice.size == once.size && memcmp(twice.data, once.data, once.size) == 0,
              "LBox = 0 superbox: output does not transcode to itself: %s", error);
    }
    hv_out_free(&once);
    hv_out_free(&twice);
    bytes_free(&g);

    g = bytes_copy(f.data, f.size);
    append(&g, bad_child, sizeof bad_child);
    expect_file_error("uinf child with LBox 7", &g, "box.framing at ");
    bytes_free(&g);

    /* XML boxes: each ends before its first NUL, with a length, the last
     * one's LBox = 0 included; one without a NUL is copied as read, even
     * when its XML is not well-formed. */
    {
        static const uint8_t nul_end[] = {0, 0, 0, 13, 'x', 'm', 'l', ' ', '<', 'a', '/', '>', 0};
        static const uint8_t nul_inside[] = {0,   0,   0,   14,  'x', 'm', 'l', ' ',
                                             '<', 'c', '/', '>', 0,   'x'};
        static const uint8_t to_end_nul[] = {0, 0, 0, 0, 'x', 'm', 'l', ' ', '<', 'd', '/', '>', 0, 0};
        static const uint8_t expected[] = {
            0, 0, 0, 12, 'x', 'm', 'l', ' ', '<', 'a', '/', '>',
            0, 0, 0, 11, 'x', 'm', 'l', ' ', '<', 'a', '>',             /* bad_xml */
            0, 0, 0, 12, 'x', 'm', 'l', ' ', '<', 'c', '/', '>',
            0, 0, 0, 12, 'x', 'm', 'l', ' ', '<', 'd', '/', '>'};
        g = bytes_copy(f.data, f.size);
        append(&g, nul_end, sizeof nul_end);
        append(&g, bad_xml, sizeof bad_xml);
        append(&g, nul_inside, sizeof nul_inside);
        append(&g, to_end_nul, sizeof to_end_nul);
        hv_out_init(&once);
        if (hv_transcode_file(g.data, g.size, 7, 7, HV_OUTPUT_JPIP, &once, error, sizeof error) != 0) {
            check(0, "XML boxes: rejected: %s", error);
        } else {
            expect_served("XML boxes", once.data, once.size);
            check(once.size >= sizeof expected &&
                      memcmp(once.data + once.size - sizeof expected, expected,
                             sizeof expected) == 0,
                  "XML boxes: not cut before their first NUL, or not copied as read");
        }
        hv_out_free(&once);
        bytes_free(&g);
    }

    /* JPX-only boxes in this JP2 are opaque, including malformed child
     * framing. The bounded bodies must survive unchanged. */
    {
        static const uint8_t extensions[][9] = {
            {0,0,0,9,'a','s','o','c',1},
            {0,0,0,9,'g','r','p',' ',1}
        };
        for (size_t i = 0; i < sizeof extensions / sizeof *extensions; i++) {
            g = bytes_copy(f.data, f.size);
            append(&g, extensions[i], sizeof extensions[i]);
            hv_out_init(&once);
            check(hv_transcode_file(g.data, g.size, 7, 7, HV_OUTPUT_JPIP, &once, error, sizeof error) == 0,
                  "opaque extension %zu: rejected: %s", i, error);
            if (once.size >= sizeof extensions[i]) {
                size_t at;
                expect_served("opaque extension", once.data, once.size);
                check(hv_check_jp2h(once.data, once.size, &at) == NULL,
                      "opaque extension: output JP2 headers valid");
                check(memcmp(once.data + once.size - sizeof extensions[i], extensions[i],
                             sizeof extensions[i]) == 0,
                      "opaque extension %zu: bytes changed", i);
            }
            hv_out_free(&once);
            /* Opaque bodies still require a bounded top-level extent. */
            put32(g.data + f.size, sizeof extensions[i] + 1);
            expect_file_error("extension overruns file", &g, "box overruns its container at ");
            bytes_free(&g);
        }
    }
    /* Apparent JPX nesting does not impose a recursion limit on opaque
     * JP2 bytes. Both sides of the old limit are copied exactly. */
    {
        for (int depth = HV_BOX_DEPTH_MAX; depth <= HV_BOX_DEPTH_MAX + 1; depth++) {
            g = bytes_copy(f.data, f.size);
            for (int k = 0; k < depth; k++) {
                uint8_t h[8] = {0, 0, 0, 0, 'a', 's', 'o', 'c'};
                put32(h, (uint32_t)(8 * (depth - k)));
                append(&g, h, sizeof h);
            }
            hv_out_init(&once);
            check(hv_transcode_file(g.data, g.size, 7, 7, HV_OUTPUT_JPIP, &once, error, sizeof error) == 0,
                  "%d apparent extension levels: rejected: %s", depth, error);
            if (once.size >= 8 * (size_t)depth) {
                size_t at;
                expect_served("opaque nested extension", once.data, once.size);
                check(hv_check_jp2h(once.data, once.size, &at) == NULL,
                      "opaque nested extension: output JP2 headers valid");
                check(memcmp(once.data + once.size - 8 * (size_t)depth,
                             g.data + f.size, 8 * (size_t)depth) == 0,
                      "%d apparent extension levels: bytes changed", depth);
            }
            hv_out_free(&once);
            bytes_free(&g);
        }
    }

    bytes_free(&f);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * Corrupted tile data: every result is a rejection or a stable output
 * ------------------------------------------------------------------------ */

static uint32_t rng_state = 1;

/* Unsigned arithmetic that wraps on purpose, which Clang's
 * -fsanitize=integer would report. */
#if defined(__clang__)
#define WRAPS __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base")))
#else
#define WRAPS
#endif

/* xorshift32. */
WRAPS static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static size_t below(size_t n) {
    return n ? rng() % n : 0;
}

static void test_corruption(void) {
    static const char *const names[3] = {
        "solo_fsi174_127x129_RLCP_PLT.jp2", "synthetic_rgb_129x129_origin129_CPRL.jp2",
        "synthetic_rgb_129x129_CPRL_SOP_EPH.jp2"};
    static const uint8_t runs[4] = {0xFF, 0x00, 0x7F, 0x80};
    split s[3];
    int i, k, accepted = 0, rejected = 0;

    for (i = 0; i < 3; i++)
        split_codestream(&s[i], "input", names[i]);
    for (i = 0; i < 2000; i++) {
        split *o = &s[below(3)];
        bytes tile = bytes_copy(o->body.data, o->body.size), b;
        size_t at = below(tile.size - 16), n, m;
        result r;
        switch (below(4)) {
        case 0:                                 /* a few bytes changed */
            for (k = 1 + (int)below(8); k > 0; k--)
                tile.data[below(tile.size)] = (uint8_t)rng();
            break;
        case 1:                                 /* cut short */
            tile.size = at;
            break;
        case 2: {                               /* n bytes replaced by a run of m */
            uint8_t v = runs[below(4)];
            n = 1 + below(16);
            m = 1 + below(16);
            b = bytes_copy(tile.data, at);
            for (k = 0; k < (int)m; k++)
                append(&b, &v, 1);
            if (at + n < tile.size)
                append(&b, tile.data + at + n, tile.size - at - n);
            bytes_free(&tile);
            tile = b;
            break;
        }
        default:                                /* random bytes inserted */
            b = bytes_copy(tile.data, at);
            for (k = 1 + (int)below(32); k > 0; k--) {
                uint8_t v = (uint8_t)rng();
                append(&b, &v, 1);
            }
            append(&b, tile.data + at, tile.size - at);
            bytes_free(&tile);
            tile = b;
        }
        b = codestream(&o->main);
        tile_part(&b, 0, 0, 1, tile.data, tile.size, 0);
        eoc(&b);
        r = transcode(b.data, b.size, 7, 7);
        if (r.status != 0) {
            rejected++;
        } else {
            accepted++;
            expect_stable("corrupted tile", &r.out);
        }
        bytes_free(&r.out);
        bytes_free(&b);
        bytes_free(&tile);
    }
    check(accepted > 0 && rejected > 0, "corruption: %d accepted, %d rejected", accepted, rejected);
    for (i = 0; i < 3; i++)
        split_free(&s[i]);
}

/* ------------------------------------------------------------------------
 * SOP, EPH, bit stuffing and code-block data (A.8.1, A.8.2, B.10.1,
 * B.10.7, C.3.4)
 * ------------------------------------------------------------------------ */

static size_t find_all(const bytes *b, const uint8_t *pattern, size_t n, size_t *at, size_t max) {
    size_t i, count = 0;
    for (i = 0; i + n <= b->size && count < max; i++)
        if (memcmp(b->data + i, pattern, n) == 0)
            at[count++] = i;
    return count;
}

static void set_psot(bytes *b) {
    size_t sot = 2;
    while (get16(b->data + sot) != HV_SOT)
        sot += 2 + get16(b->data + sot + 2);
    put32(b->data + sot + 6, (uint32_t)(b->size - 2 - sot));
}

static void test_packet_rules(void) {
    static const uint8_t sop_code[4] = {0xFF, 0x91, 0x00, 0x04}, eph_code[2] = {0xFF, 0x92};
    bytes file, cs = fixture_codestream("input", "synthetic_rgb_129x129_CPRL_SOP_EPH.jp2", &file);
    bytes b, noplt;
    size_t sop[64], eph[64], nsop, neph, i, sot, p;
    char message[128];

    nsop = find_all(&cs, sop_code, 4, sop, 64);
    neph = find_all(&cs, eph_code, 2, eph, 64);
    check(nsop == 27 && neph == 27, "SOP/EPH fixture: %zu SOP, %zu EPH", nsop, neph);
    if (nsop < 8 || neph < 8)
        return;

    b = bytes_copy(cs.data, cs.size);
    b.data[sop[5] + 5] ^= 1;
    expect_error("Nsop out of sequence", &b, "invalid SOP marker segment before packet 5");
    bytes_free(&b);
    b = bytes_copy(cs.data, cs.size);
    b.data[sop[5] + 3] = 5;
    expect_error("Lsop 5", &b, "invalid SOP marker segment before packet 5");
    bytes_free(&b);

    /* A header byte after 0xFF with its top bit set. */
    for (i = 0; i < nsop; i++) {
        size_t k;
        for (k = sop[i] + 6; k + 1 < eph[i]; k++)
            if (cs.data[k] == 0xFF)
                break;
        if (k + 1 < eph[i]) {
            b = bytes_copy(cs.data, cs.size);
            b.data[k + 1] |= 0x80;
            snprintf(message, sizeof message, "invalid bit stuffing in packet header %zu", i);
            expect_error("bad stuffing", &b, message);
            bytes_free(&b);
            break;
        }
    }
    check(i < nsop, "SOP/EPH fixture: no packet header with 0xFF");

    /* The code-block data of a packet, from its EPH to the next SOP: its
     * last byte 0xFF ends a contribution in 0xFF (B.10.7); 0xFF and 0xA0 as
     * its last two bytes are a marker inside a contribution (C.3.4) where
     * the last contribution holds both. */
    {
        static const uint8_t sot_code[2] = {0xFF, 0x90};
        int found = 0;
        for (i = 0; i + 1 < nsop; i++) {
            bytes body = {cs.data + eph[i] + 2, sop[i + 1] - (eph[i] + 2)};
            size_t at;
            if (sop[i + 1] <= eph[i] + 2 || find_all(&body, sot_code, 2, &at, 1) != 0)
                continue;
            b = bytes_copy(cs.data, cs.size);
            b.data[sop[i + 1] - 1] = 0xFF;
            snprintf(message, sizeof message,
                     "a code-block's contribution to packet %zu ends in 0xFF (T.800 B.10.7)", i);
            expect_error("contribution ending in 0xFF", &b, message);
            bytes_free(&b);
            break;
        }
        check(i + 1 < nsop, "SOP/EPH fixture: no packet with code-block data");
        for (i = 0; i + 1 < nsop && !found; i++) {
            bytes body = {cs.data + eph[i] + 2, sop[i + 1] - (eph[i] + 2)};
            size_t at;
            result r;
            if (sop[i + 1] < eph[i] + 4 || find_all(&body, sot_code, 2, &at, 1) != 0)
                continue;
            b = bytes_copy(cs.data, cs.size);
            b.data[sop[i + 1] - 2] = 0xFF;
            b.data[sop[i + 1] - 1] = 0xA0;
            snprintf(message, sizeof message,
                     "a code-block's contribution to packet %zu holds 0xFF and a byte above "
                     "0x8F, a marker (T.800 C.3.4)", i);
            r = transcode(b.data, b.size, 7, 7);
            found = r.status != 0 && strcmp(r.error, message) == 0;
            bytes_free(&r.out);
            bytes_free(&b);
        }
        check(found, "SOP/EPH fixture: no contribution with 0xFF and 0xA0 rejected (C.3.4)");
    }

    /* EPH missing after one header: without the PLT, which would no longer
     * add up. */
    noplt = bytes_copy(cs.data, cs.size);
    for (sot = 2; get16(noplt.data + sot) != HV_SOT; sot += 2 + get16(noplt.data + sot + 2)) {}
    for (p = sot + 12; get16(noplt.data + p) != HV_SOD;) {
        size_t n = 2 + get16(noplt.data + p + 2);
        if (get16(noplt.data + p) == HV_PLT) {
            memmove(noplt.data + p, noplt.data + p + n, noplt.size - p - n);
            noplt.size -= n;
        } else {
            p += n;
        }
    }
    set_psot(&noplt);
    {
        result r = transcode(noplt.data, noplt.size, 7, 7);
        check(r.status == 0, "SOP/EPH fixture without PLT: rejected: %s", r.error);
        bytes_free(&r.out);
    }
    neph = find_all(&noplt, eph_code, 2, eph, 64);
    b = bytes_copy(noplt.data, noplt.size);
    memmove(b.data + eph[7], b.data + eph[7] + 2, b.size - eph[7] - 2);
    b.size -= 2;
    set_psot(&b);
    expect_error("EPH missing", &b, "missing EPH marker after packet header 7");
    bytes_free(&b);
    bytes_free(&noplt);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * Zero PLT entries after the last packet of the tile-part, as every EUI
 * file has: accepted by the codestream-level and the file-level transcode
 * alike.
 * ------------------------------------------------------------------------ */

static void test_plt_padding(void) {
    static const uint8_t plt_code[2] = {0xFF, 0x58}, sot_code[2] = {0xFF, 0x90};
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    size_t plt[2], sot[2], end;
    bytes b, f;

    if (find_all(&cs, sot_code, 2, sot, 2) != 1 || find_all(&cs, plt_code, 2, plt, 2) != 1) {
        check(0, "PLT padding: the fixture has not one tile-part with a PLT");
        bytes_free(&file);
        return;
    }
    end = plt[0] + 2 + get16(cs.data + plt[0] + 2);
    b.size = cs.size + 2;
    if ((b.data = malloc(b.size)) == NULL)
        abort();
    memcpy(b.data, cs.data, end);
    b.data[end] = b.data[end + 1] = 0;            /* two zero Iplt */
    memcpy(b.data + end + 2, cs.data + end, cs.size - end);
    put16(b.data + plt[0] + 2, get16(cs.data + plt[0] + 2) + 2);
    set_psot(&b);
    f = jp2_file(&b, "jp2 ");
    expect_file_output("zero PLT entries after the last packet", &f, &b);
    bytes_free(&f);
    bytes_free(&b);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * Memory bounds for what headers declare
 * ------------------------------------------------------------------------ */

/* A codestream with ncomps components (sub-sampling xr, grid origin x0),
 * `levels` decomposition levels, one layer and 10 bytes of tile data. */
static bytes declared(unsigned ncomps, unsigned levels, unsigned xr, uint32_t x0) {
    bytes b = bytes_copy((const uint8_t *)"\xFF\x4F\xFF\x51", 4);
    uint8_t h[40], c[3] = {7, 0, 0};
    static const uint8_t data[10] = {0};
    unsigned i;
    put16(h, 38 + 3 * ncomps);
    put16(h + 2, 0);
    put32(h + 4, x0 + 100);                     /* Xsiz, Ysiz */
    put32(h + 8, 100);
    put32(h + 12, x0);                          /* XOsiz, YOsiz */
    put32(h + 16, 0);
    put32(h + 20, x0 + 100);                    /* XTsiz, YTsiz */
    put32(h + 24, 100);
    put32(h + 28, 0);                           /* XTOsiz, YTOsiz */
    put32(h + 32, 0);
    put16(h + 36, ncomps);
    append(&b, h, 38);
    c[1] = c[2] = (uint8_t)xr;
    for (i = 0; i < ncomps; i++)
        append(&b, c, 3);
    {
        uint8_t cod[14] = {0xFF, 0x52, 0, 12, 0, 0, 0, 1, 0, 0, 4, 4, 0, 0};
        /* Scalar derived (Equation A-4: one step size) for the 9-7. */
        static const uint8_t qcd[7] = {0xFF, 0x5C, 0, 5, 0x41, 0x40, 0x00};
        cod[9] = (uint8_t)levels;
        append(&b, cod, 14);
        append(&b, qcd, 7);
    }
    tile_part(&b, 0, 0, 1, data, sizeof data, 0);
    eoc(&b);
    return b;
}

/* 128 x 128 samples in 4 x 4 code-blocks and precincts, no levels: 1,024
 * precincts in `layers` layers, with `data` bytes of tile data (zero:
 * empty packets). */
static bytes many_packets(unsigned layers, size_t data) {
    bytes b = bytes_copy((const uint8_t *)"\xFF\x4F\xFF\x51", 4);
    uint8_t h[41] = {0}, cod[15] = {0xFF, 0x52, 0, 13, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x22};
    static const uint8_t qcd[6] = {0xFF, 0x5C, 0, 4, 0x40, 0x40};
    uint8_t *zeros = calloc(data, 1);
    put16(h, 41);
    put32(h + 4, 128);                          /* Xsiz, Ysiz */
    put32(h + 8, 128);
    put32(h + 20, 128);                         /* XTsiz, YTsiz */
    put32(h + 24, 128);
    put16(h + 36, 1);
    h[38] = 7;                                  /* Ssiz, XRsiz, YRsiz */
    h[39] = h[40] = 1;
    append(&b, h, 41);
    put16(cod + 6, layers);
    append(&b, cod, 15);
    append(&b, qcd, 6);
    tile_part(&b, 0, 0, 1, zeros, data, 0);
    eoc(&b);
    free(zeros);
    return b;
}

static void test_memory_bounds(void) {
    static const char *too_many =
        "component and resolution count exceeds supported limit (65,536)";
    bytes b;
    /* Input packets: 16,384,000 in 16,384,001 bytes, over the limit; and
     * 1,024,000 in fewer bytes than that, over the data's. */
    b = many_packets(16000, 16384001);
    expect_error("16,384,000 empty packets", &b,
                 "packet count exceeds supported limit (16,000,000)");
    bytes_free(&b);
    b = many_packets(1000, 1023999);
    expect_error("1,024,000 packets in 1,023,999 bytes", &b, "packet count exceeds tile data");
    bytes_free(&b);
    b = declared(16384, 32, 1, 0);
    expect_error("16,384 components, 33 resolutions", &b, too_many);
    bytes_free(&b);
    b = declared(16384, 32, 255, 1);            /* nearly all of them empty */
    expect_error("16,384 sub-sampled components", &b, too_many);
    bytes_free(&b);
    b = declared(2048, 32, 1, 0);
    expect_error("2,048 components, 33 resolutions", &b, too_many);
    bytes_free(&b);
    b = declared(2048, 31, 1, 0);               /* 65,536: at the limit */
    expect_error("2,048 components, 32 resolutions", &b, "packet count exceeds tile data");
    bytes_free(&b);
}

/* The contributions hv_read_packets records are bounded: past the limit of
 * hv_codeblocks_init it fails, whatever the data (a packet header signals
 * a contribution in a few bits). A fixture's tile, read as transcode.c
 * reads it, with a limit below its contributions and with one above. */
static void test_contribution_limit(void) {
    static const hv_geometry_limits limits = {65536, 1000000, NULL};
    static const size_t max[] = {3, 1000000};
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    hv_codestream c;
    hv_item item;
    hv_span parts[64];
    hv_geometry g;
    hv_packet *packets = NULL;
    hv_codeblocks cb;
    size_t nparts = 0, count = 0, k;
    char error[256] = "";
    int zero_psot = 0, status;

    memset(&g, 0, sizeof g);
    if (hv_codestream_open(&c, cs.data, 0, cs.size, HV_READ_PADDED) == 0)
        while (hv_codestream_next(&c, &item) == 1 && item.kind != HV_END) {
            zero_psot |= item.kind == HV_TILE_PART && item.sot.psot == 0;
            if (item.kind == HV_TILE_DATA && nparts < 64) {
                parts[nparts].start = item.start;
                parts[nparts++].end = item.end;
            }
        }
    if (c.error != NULL || nparts == 0 ||
        hv_geometry_init(&g, hv_codestream_siz(&c), hv_codestream_cod(&c), 0, &limits, error,
                         sizeof error) != 0 ||
        hv_geometry_packets(&g, &packets, &count, error, sizeof error) != 0) {
        check(0, "fixture laid out: %s", c.error ? c.error : error);
    } else {
        hv_tile_data tile = {cs.data, parts, nparts, zero_psot};
        const Cod *cod = hv_codestream_cod(&c);
        for (k = 0; k < 2; k++) {
            if (hv_codeblocks_init(&cb, (size_t)g.nblocks, max[k]) != 0) {
                check(0, "out of memory");
                break;
            }
            status = hv_read_packets(&g, packets, count, &tile, cod->scod.sopMarkers,
                                     cod->scod.ephMarkers, &cb, error, sizeof error);
            if (k == 0)
                check(status != 0 && strcmp(error, "code-block contribution count exceeds "
                                                   "supported limit (3)") == 0,
                      "3 contributions: \"%s\"", status ? error : "accepted");
            else
                check(status == 0 && cb.ncontrib > 3, "1,000,000 contributions: %s", error);
            hv_codeblocks_free(&cb);
        }
    }
    free(packets);
    hv_geometry_free(&g);
    hv_codestream_close(&c);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * Layouts: the code-block partition, the precinct sizes, the limits
 * ------------------------------------------------------------------------ */

/* A codestream of w x h samples in one component, `levels` levels,
 * `layers` layers, code-blocks 2^xcb x 2^ycb and, at each resolution r,
 * precincts precincts[r] (PPx | PPy << 4; NULL: the largest), every
 * packet empty: one byte 0 of tile data each. The packets are counted
 * with hv_geometry on the codestream without them. */
static bytes synthetic(uint32_t w, uint32_t h, unsigned levels, unsigned layers, unsigned xcb,
                       unsigned ycb, const uint8_t *precincts) {
    uint8_t siz[43] = {0xFF, 0x51, 0, 41}, cod[14 + 33] = {0xFF, 0x52}, qcd[6 + 96] = {0xFF, 0x5C};
    static const hv_geometry_limits limits = {65536, (uint64_t)1 << 40, NULL};
    size_t ncod = 14 + (precincts ? levels + 1 : 0), nqcd = 6 + 3 * levels, packets = 0, k;
    bytes b, data;
    hv_codestream c;
    hv_item item;
    hv_geometry g;
    char error[256];
    int pass;
    put32(siz + 6, w);
    put32(siz + 10, h);
    put32(siz + 22, w);
    put32(siz + 26, h);
    put16(siz + 38, 1);
    siz[40] = 7;
    siz[41] = siz[42] = 1;
    put16(cod + 2, (unsigned)ncod - 2);
    cod[4] = precincts != NULL;
    put16(cod + 6, layers);
    cod[9] = (uint8_t)levels;
    cod[10] = (uint8_t)(xcb - 2);
    cod[11] = (uint8_t)(ycb - 2);
    cod[13] = 1;                                /* 5-3 reversible */
    for (k = 0; precincts != NULL && k <= levels; k++)
        cod[14 + k] = precincts[k];
    put16(qcd + 2, (unsigned)nqcd - 2);
    qcd[4] = 0x40;                              /* no quantization, 2 guard bits */
    memset(qcd + 5, 0x40, 3 * levels + 1);
    for (pass = 0; pass < 2; pass++) {
        b = bytes_copy((const uint8_t *)"\xFF\x4F", 2);
        append(&b, siz, 43);
        append(&b, cod, ncod);
        append(&b, qcd, nqcd);
        data.data = calloc(packets ? packets : 1, 1);
        data.size = packets;
        tile_part(&b, 0, 0, 1, data.data, data.size, 0);
        eoc(&b);
        free(data.data);
        if (pass == 1)
            break;
        memset(&g, 0, sizeof g);
        if (hv_codestream_open(&c, b.data, 0, b.size, HV_READ_VALIDATE) == 0)
            while (hv_codestream_next(&c, &item) == 1 && item.kind != HV_TILE_PART)
                ;
        if (c.error == NULL && hv_geometry_init(&g, hv_codestream_siz(&c), hv_codestream_cod(&c),
                                                0, &limits, error, sizeof error) == 0)
            packets = (size_t)(g.nprecincts * (uint64_t)layers);
        else
            check(0, "synthetic %ux%u: %s", w, h, c.error ? c.error : error);
        hv_geometry_free(&g);
        hv_codestream_close(&c);
        bytes_free(&b);
    }
    return b;
}

static void test_layouts(void) {
    /* Code-blocks 128 x 32 over 64 x 64 samples, two levels. The new
     * precincts, 128 x 128, clip the code-blocks to 64 wide in the bands of
     * resolutions 1 and 2 (T.800 B-17), not at resolution 0. */
    static const struct {
        const char *what;
        uint8_t precincts[3];
        int same;
    } partitions[] = {
        {"the largest precincts", {0xFF, 0xFF, 0xFF}, 0},
        {"another partition at the last resolution", {0x77, 0x77, 0xFF}, 0},
        {"another partition at resolution 0", {0x66, 0x77, 0x77}, 0},
        {"the new precincts", {0x77, 0x77, 0x77}, 1},
    };
    static const char *changed = "128x128 precincts change the code-block partition";
    static const char *range = "precinct dimensions must be powers of 2 from 2 to 32,768";
    static const int bad[][2] = {{0, 7}, {7, 0}, {16, 7}, {7, 16}};
    const uint8_t largest[2] = {0xFF, 0xFF};
    bytes b, same = {NULL, 0};
    result r;
    size_t i;

    for (i = 0; i < sizeof partitions / sizeof *partitions; i++) {
        b = synthetic(64, 64, 2, 1, 7, 5, partitions[i].precincts);
        if (partitions[i].same) {
            r = transcode(b.data, b.size, 7, 7);
            check(r.status == 0 && r.error[0] == 0, "%s: rejected: %s", partitions[i].what,
                  r.error);
            bytes_free(&r.out);
            same = bytes_copy(b.data, b.size);
        } else {
            expect_error(partitions[i].what, &b, changed);
        }
        bytes_free(&b);
    }
    /* Only empty bands clipped (1 x 1 samples: no band at resolution 1),
     * and one of a single code-block (2 x 2 samples). */
    b = synthetic(1, 1, 1, 1, 7, 5, largest);
    r = transcode(b.data, b.size, 7, 7);
    check(r.status == 0, "only empty bands clipped: rejected: %s", r.error);
    bytes_free(&r.out);
    bytes_free(&b);
    b = synthetic(2, 2, 1, 1, 7, 5, largest);
    expect_error("a band of one code-block clipped", &b, changed);
    bytes_free(&b);

    /* The precinct sizes: 2 to 2^15 each way, and the message's. */
    for (i = 0; i < sizeof bad / sizeof *bad; i++) {
        r = transcode(same.data, same.size, bad[i][0], bad[i][1]);
        check(r.status != 0 && strcmp(r.error, range) == 0, "precincts 2^%d x 2^%d: \"%s\"",
              bad[i][0], bad[i][1], r.status ? r.error : "accepted");
        bytes_free(&r.out);
    }
    r = transcode(same.data, same.size, 1, 2);
    check(r.status != 0 && strcmp(r.error, "2x4 precincts change the code-block partition") == 0,
          "2x4 precincts: \"%s\"", r.status ? r.error : "accepted");
    bytes_free(&r.out);
    b = synthetic(64, 64, 2, 1, 7, 5, partitions[0].precincts);
    r = transcode(b.data, b.size, 15, 15);
    check(r.status == 0, "2^15 x 2^15 precincts: rejected: %s", r.error);
    bytes_free(&r.out);
    bytes_free(&b);

    /* An earlier write failed: nothing more is written. */
    {
        hv_out out;
        char error[256];
        hv_out_init(&out);
        out.error = "earlier error";
        check(hv_transcode_codestream(same.data, 0, same.size, 7, 7, HV_OUTPUT_JPEG2000, &out, error,
                                      sizeof error) != 0 &&
              strcmp(error, "earlier error") == 0 && out.size == 0,
              "after a failed write: \"%s\"", error);
        hv_out_free(&out);
    }
    bytes_free(&same);

    /* 250,000 code-blocks of 4 x 4, and 250,500. */
    b = synthetic(2000, 2000, 0, 1, 2, 2, NULL);
    r = transcode(b.data, b.size, 7, 7);
    check(r.status == 0, "250,000 code-blocks: rejected: %s", r.error);
    bytes_free(&r.out);
    bytes_free(&b);
    b = synthetic(2004, 2000, 0, 1, 2, 2, NULL);
    expect_error("250,500 code-blocks", &b, "code-block count exceeds supported limit (250,000)");
    bytes_free(&b);

    /* The new packets: 32 x 32 precincts of 4,096 x 4,096 samples in 1,954
     * layers are 2,000,896. */
    b = synthetic(4096, 4096, 0, 1954, 6, 6, NULL);
    expect_error("2,000,896 new packets", &b, "packet count exceeds supported limit (2,000,000)");
    bytes_free(&b);
}

/* ------------------------------------------------------------------------
 * The markers kept, dropped and rejected
 * ------------------------------------------------------------------------ */

enum { COPIED, DROPPED, REJECTED };

/* A marker segment, or a marker without one, and what the transcoder does
 * with it (main_marker and tile_marker in transcode.c). */
typedef struct {
    const char *name;
    uint8_t bytes[64];
    size_t size;
    int action;
} marker_case;

/* Rejected entries in this table are unsupported layout/coding markers.
 * Its main-header entries except 0xFF70 also lie outside HV_READ_PACKETS_JPIP. */
static void rejection(char *message, size_t size, unsigned flags,
                      const char *where, unsigned code) {
    if (flags == HV_READ_PACKETS_JPIP && strcmp(where, "main header") == 0 && code != 0xFF70)
        snprintf(message, size, "main.marker-code at ");
    else
        snprintf(message, size, "unsupported %s marker 0x%04X", where, code);
}

/* The codestream cs with marker m in it, at the codestream level and in a
 * JP2 file: copied or dropped, both accept it, the codestream level with
 * this output and the file level with the same codestream; rejected, both
 * reject it, each with rejection()'s message. */
static void expect_marker(const char *name, const bytes *cs, const marker_case *m,
                          const char *where, const bytes *expected) {
    bytes file = jp2_file(cs, "jp2 ");
    char message[256];
    unsigned code = get16(m->bytes);
    if (m->action != REJECTED) {
        expect_output(name, cs, expected);
        expect_file_output(name, &file, cs);
    } else {
        result r = transcode(cs->data, cs->size, 7, 7);
        rejection(message, sizeof message, 0, where, code);
        check(r.status != 0 && strncmp(r.error, message, strlen(message)) == 0,
              "%s: \"%s\", expected \"%s\"", name, r.status ? r.error : "accepted", message);
        bytes_free(&r.out);
        rejection(message, sizeof message, HV_READ_PACKETS_JPIP, where, code);
        expect_file_error(name, &file, message);
    }
    bytes_free(&file);
}

/* Each marker the transcoder treats in its own way, in the fixture's main
 * header before SOT and in its tile-part header after SOT, through
 * hv_transcode_codestream and hv_transcode_file: a copied marker is in the
 * output before SOT, as in the input; a dropped one is not. */
static void test_markers(void) {
    /* TLM and PLM describe the fixture's tile-part (below). */
    static marker_case main_cases[] = {
        {"QCC missing values", {0xFF, 0x5D, 0x00, 0x05, 0x00, 0x40, 0x40}, 7, COPIED},
        {"RGN", {0xFF, 0x5E, 0x00, 0x05, 0x00, 0x00, 0x07}, 7, COPIED},
        {"CRG", {0xFF, 0x63, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 8, COPIED},
        {"COM", {0xFF, 0x64, 0x00, 0x05, 0x00, 0x01, 'x'}, 7, COPIED},
        {"TLM", {0xFF, 0x55, 0x00, 0x08, 0x00, 0x40}, 10, DROPPED},
        {"PLM", {0xFF, 0x57}, 0, DROPPED},
        {"COC", {0xFF, 0x53, 0x00, 0x03, 0x00}, 5, REJECTED},
        {"POC", {0xFF, 0x5F, 0x00, 0x03, 0x00}, 5, REJECTED},
        {"PPM", {0xFF, 0x60, 0x00, 0x03, 0x00}, 5, REJECTED},
        {"0xFF30", {0xFF, 0x30}, 2, REJECTED},
        {"0xFF3F", {0xFF, 0x3F}, 2, REJECTED},
        {"0xFF70", {0xFF, 0x70, 0x00, 0x04, 0x00, 0x00}, 6, REJECTED},
        {"RGN reserved style", {0xFF, 0x5E, 0x00, 0x05, 0x00, 0x01, 0x07}, 7, COPIED},
        {"CRG short body", {0xFF, 0x63, 0x00, 0x03, 0x00}, 5, COPIED},
        {"COM reserved Rcom", {0xFF, 0x64, 0x00, 0x05, 0xFF, 0xFF, 'x'}, 7, COPIED},
        {"TLM short body", {0xFF, 0x55, 0x00, 0x03, 0x00}, 5, DROPPED},
        {"PLM short body", {0xFF, 0x57, 0x00, 0x03, 0x00}, 5, DROPPED},
        {"QCC", {0xFF, 0x5D}, 0, COPIED},
    };
    static marker_case tile_cases[] = {
        {"COM", {0xFF, 0x64, 0x00, 0x05, 0x00, 0x01, 'x'}, 7, DROPPED},
        {"QCD", {0xFF, 0x5C, 0x00, 0x04, 0x40, 0x40}, 6, REJECTED},
        {"RGN", {0xFF, 0x5E, 0x00, 0x05, 0x00, 0x00, 0x07}, 7, REJECTED},
        {"POC", {0xFF, 0x5F, 0x00, 0x03, 0x00}, 5, REJECTED},
        {"0xFF30", {0xFF, 0x30}, 2, REJECTED},
        {"0xFF70", {0xFF, 0x70, 0x00, 0x04, 0x00, 0x00}, 6, REJECTED},
        {"COM reserved Rcom", {0xFF, 0x64, 0x00, 0x05, 0xFF, 0xFF, 'x'}, 7, DROPPED},
        {"PLT empty body", {0xFF, 0x58, 0x00, 0x02}, 4, DROPPED},
        {"PLT unterminated entry", {0xFF, 0x58, 0x00, 0x04, 0x00, 0x80}, 6, DROPPED},
        {"QCD complete", {0xFF, 0x5C}, 0, REJECTED},
    };
    bytes file, cs = fixture_codestream("input", "solo_fsi174_127x129_RLCP_PLT.jp2", &file);
    result plain = transcode(cs.data, cs.size, 7, 7);
    size_t sot = 2, out_sot = 2, i;
    uint32_t psot;
    char name[64];

    check(plain.status == 0, "the fixture: rejected: %s", plain.error);
    if (plain.status != 0) {
        bytes_free(&file);
        return;
    }
    while (get16(cs.data + sot) != HV_SOT)
        sot += 2 + get16(cs.data + sot + 2);
    while (get16(plain.out.data + out_sot) != HV_SOT)
        out_sot += 2 + get16(plain.out.data + out_sot + 2);
    psot = get32(cs.data + sot + 6);
    /* Preserve the short QCC/QCD above as invalid-input regressions. The
     * marker-support cases need the fixture's full quantization table. */
    {
        marker_case *qcc = &main_cases[sizeof main_cases / sizeof *main_cases - 1];
        marker_case *qcd = &tile_cases[sizeof tile_cases / sizeof *tile_cases - 1];
        size_t pos = 2, n;
        while (get16(cs.data + pos) != HV_QCD)
            pos += 2 + get16(cs.data + pos + 2);
        n = get16(cs.data + pos + 2) - 2;   /* Sqcd and every SPqcd entry */
        if (n + 5 > sizeof qcc->bytes) abort();
        put16(qcc->bytes + 2, (uint16_t)(n + 3));
        qcc->bytes[4] = 0;                /* Cqcc */
        memcpy(qcc->bytes + 5, cs.data + pos + 4, n);
        qcc->size = n + 5;
        memcpy(qcd->bytes, cs.data + pos, n + 4);
        qcd->size = n + 4;
    }
    /* TLM: Ztlm 0, ST 0 and SP 1 (A.7.1): the tile-part's Psot. PLM: Zplm
     * 0, and the lengths of the PLT segment of its header (A.7.2). */
    put32(main_cases[4].bytes + 6, psot);
    {
        const uint8_t *plt = cs.data + sot + 12;
        size_t n = get16(plt + 2) - 3;              /* Iplt bytes */
        check(get16(plt) == HV_PLT && n <= 255 && 6 + n <= sizeof main_cases[5].bytes,
              "the fixture's tile-part header: one PLT segment of up to 255 bytes");
        put16(main_cases[5].bytes + 2, (uint16_t)(4 + n));
        main_cases[5].bytes[4] = 0;
        main_cases[5].bytes[5] = (uint8_t)n;
        memcpy(main_cases[5].bytes + 6, plt + 5, n);
        main_cases[5].size = 6 + n;
    }

    for (i = 0; i < sizeof main_cases / sizeof *main_cases; i++) {
        const marker_case *m = &main_cases[i];
        bytes in = concat3(cs.data, sot, m->bytes, m->size, cs.data + sot, cs.size - sot);
        bytes expected = m->action == COPIED
                             ? concat3(plain.out.data, out_sot, m->bytes, m->size,
                                       plain.out.data + out_sot, plain.out.size - out_sot)
                             : bytes_copy(plain.out.data, plain.out.size);
        snprintf(name, sizeof name, "%s in the main header", m->name);
        if (i == 0) {
            const char *error = read_error(in.data, 0, in.size, HV_READ_PADDED);
            check(error != NULL && strcmp(error, "codestream.quantization-coverage") == 0,
                  "short main QCC: expected missing quantization entries");
        }
        expect_marker(name, &in, m, "main header", &expected);
        bytes_free(&expected);
        bytes_free(&in);
    }
    for (i = 0; i < sizeof tile_cases / sizeof *tile_cases; i++) {
        const marker_case *m = &tile_cases[i];
        bytes in = concat3(cs.data, sot + 12, m->bytes, m->size, cs.data + sot + 12,
                           cs.size - sot - 12);
        if (psot != 0)
            put32(in.data + sot + 6, psot + (uint32_t)m->size);
        snprintf(name, sizeof name, "%s in the tile-part header", m->name);
        if (i == 1) {
            /* Explicit validation still diagnoses the body, whereas both
             * transcode APIs reject the unsupported coding override. */
            bytes invalid_file = jp2_file(&in, "jp2 ");
            const char *error = read_error(in.data, 0, in.size, HV_READ_PADDED);
            check(error != NULL && strcmp(error, "codestream.quantization-coverage") == 0,
                  "short tile QCD: expected missing quantization entries");
            expect_error(name, &in, "unsupported tile-part marker 0xFF5C");
            expect_file_error(name, &invalid_file, "unsupported tile-part marker 0xFF5C");
            bytes_free(&invalid_file);
        } else
            expect_marker(name, &in, m, "tile-part", &plain.out);
        bytes_free(&in);
    }
    /* Copying and dropping never bypass segment bounds, in either API. */
    {
        uint8_t marker[] = {0xFF, 0x64, 0xFF, 0xFF};
        size_t k;
        for (k = 0; k < 3; k++) {
            size_t pos = k == 2 ? sot + 12 : sot;
            marker[1] = k == 0 ? 0x64 : k == 1 ? 0x55 : 0x58; /* COM, TLM, PLT */
            bytes in = concat3(cs.data, pos, marker, sizeof marker, cs.data + pos, cs.size - pos);
            if (k == 2 && psot != 0) put32(in.data + sot + 6, psot + sizeof marker);
            expect_reader_error("opaque segment overrun", &in, "marker segment overruns its header", pos);
            bytes wrapped = jp2_file(&in, "jp2 ");
            expect_file_error("opaque segment overrun", &wrapped, "marker segment overruns its header at ");
            bytes_free(&wrapped);
            bytes_free(&in);
        }
    }

    /* The required QCD is copied, including an unrecognized quantization
     * style. Compare the entire result with the same byte changed in the
     * reference output; explicit semantic validation still rejects it. */
    {
        size_t in_qcd = 2, out_qcd = 2;
        bytes in = bytes_copy(cs.data, cs.size);
        bytes expected = bytes_copy(plain.out.data, plain.out.size);
        while (get16(in.data + in_qcd) != HV_QCD) in_qcd += 2 + get16(in.data + in_qcd + 2);
        while (get16(expected.data + out_qcd) != HV_QCD) out_qcd += 2 + get16(expected.data + out_qcd + 2);
        in.data[in_qcd + 4] = (in.data[in_qcd + 4] & 0xE0) | 3;
        expected.data[out_qcd + 4] = in.data[in_qcd + 4];
        const char *error = read_error(in.data, 0, in.size, 0);
        check(error != NULL && strcmp(error, "qcd.style") == 0, "explicit QCD validation retained");
        expect_output("opaque QCD style", &in, &expected);
        bytes wrapped = jp2_file(&in, "jp2 ");
        expect_file_output("opaque QCD style", &wrapped, &in);
        bytes_free(&wrapped);
        bytes_free(&expected);
        bytes_free(&in);
    }
    bytes_free(&plain.out);
    bytes_free(&file);
}

/* ------------------------------------------------------------------------
 * Optional: a directory of real files
 * ------------------------------------------------------------------------ */

static void test_archive(const char *dirs) {
    char *list = strdup(dirs), *dir, *save = NULL;
    int files = 0;
    for (dir = strtok_r(list, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (d == NULL) {
            check(0, "cannot open %s", dir);
            continue;
        }
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            char path[4096], error[256];
            bytes file, cs;
            hv_out out;
            if (n < 4 || strcmp(e->d_name + n - 4, ".jp2") != 0)
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            file = read_file(path);
            hv_out_init(&out);
            files++;
            if (hv_transcode_file(file.data, file.size, 7, 7, HV_OUTPUT_JPIP, &out, error, sizeof error) != 0) {
                check(0, "%s: rejected: %s", path, error);
            } else {
                bytes result_file = {out.data, out.size};
                expect_served(path, out.data, out.size);
                cs = box_payload(&result_file, HV_BOX_JP2C);
                expect_stable(path, &cs);
            }
            hv_out_free(&out);
            bytes_free(&file);
        }
        closedir(d);
    }
    printf("archive: %d files\n", files);
    free(list);
}

int main(void) {
    struct {
        const char *name;
        void (*run)(void);
    } groups[] = {
        {"Kakadu references", test_references},
        {"tile-parts and packets", test_tile_parts},
        {"main headers", test_headers},
        {"served profile", test_served_profile},
        {"file output profiles", test_file_profiles},
        {"declaration policy", test_declaration_policy},
        {"boxes", test_boxes},
        {"corrupted tile data", test_corruption},
        {"SOP, EPH and bit stuffing", test_packet_rules},
        {"zero PLT entries after the last packet", test_plt_padding},
        {"memory bounds", test_memory_bounds},
        {"contribution limit", test_contribution_limit},
        {"layouts", test_layouts},
        {"markers", test_markers},
    };
    const char *archive = getenv("TRANSCODE_ARCHIVE");
    size_t i;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        int before = failures, n = checks;
        groups[i].run();
        printf("%-28s %5d checks, %d failed\n", groups[i].name, checks - n, failures - before);
    }
    if (archive != NULL && *archive)
        test_archive(archive);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
