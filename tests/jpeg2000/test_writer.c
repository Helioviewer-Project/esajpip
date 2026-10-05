/* test_writer: hv_writer, compiled here with a largest LBox of 100 bytes
 * instead of 4 GiB - 1, so that the switch to XLBox (hv_end_box,
 * hv_write_box_header) runs on small boxes, a largest Psot of 1,000,000,
 * and a realloc that fails on demand, so that every write runs out of
 * memory at every step. What the writer writes is read back with
 * hv_reader. The file is included, so its static helpers are tested too.
 *
 *   test_writer */
#include <stdio.h>
#include <stdlib.h>

/* realloc for hv_writer.c: fails once when fail_realloc is set, and for
 * more than 1 GiB. */
static int fail_realloc;

static void *test_realloc(void *p, size_t n) {
    if (fail_realloc || n > ((size_t)1 << 30)) {
        fail_realloc = 0;
        return NULL;
    }
    return realloc(p, n);
}

#define realloc test_realloc
#define HV_LBOX_MAX 100u
#define HV_PSOT_MAX 1000000u
#include "jpeg2000/hv_writer.c"
#undef realloc

#include "jpeg2000/hv_reader.h"

static int failures;

static void check(int ok, const char *what, const char *detail) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
        failures++;
    }
}

/* The writer's invariant: the spare capacity past out->size is zero. */
static int spare_zero(const hv_out *out) {
    size_t i;
    for (i = out->size; i < out->capacity; i++)
        if (out->data[i] != 0)
            return 0;
    return 1;
}

/* The components of write_codestream's codestream: one, or two where the
 * layers (at most 65,535) cannot make `count` packets alone. */
static size_t components_for(size_t count) { return count > 65535 ? 2 : 1; }

/* A standard-valid codestream of 8 x 8 components, no decomposition, and
 * count / components layers, so `count` packets (one per layer and
 * component), in one tile-part whose PLT lists them, one byte each. */
static int write_codestream(hv_out *out, size_t count) {
    static SizFixed fixed;
    static Component component[2];
    static CodSegment cod;
    static QcdSegment qcd;
    size_t ncomp = components_for(count);
    hv_siz siz = {&fixed, component, ncomp};
    uint64_t *lengths = malloc(count * sizeof *lengths);
    size_t i, start;
    int status;

    if (lengths == NULL)
        return -1;
    for (i = 0; i < count; i++)
        lengths[i] = 1;
    memset(&fixed, 0, sizeof fixed);
    fixed.xsiz = fixed.ysiz = fixed.xtsiz = fixed.ytsiz = 8;
    fixed.csiz = ncomp;
    memset(component, 0, sizeof component);
    for (i = 0; i < ncomp; i++) {
        component[i].depthMinus1 = 7;
        component[i].xrsiz = component[i].yrsiz = 1;
    }
    memset(&cod, 0, sizeof cod);
    cod.body.sgcod.layers = count / ncomp;
    cod.body.spcod.cbWidthExp = cod.body.spcod.cbHeightExp = 4;
    cod.body.spcod.transform = 1;
    memset(&qcd, 0, sizeof qcd);
    qcd.body.sqcd = 0x40;
    qcd.body.spqcd.nCount = 1;
    qcd.body.spqcd.arr[0] = 0x48;
    status = hv_write_marker(out, HV_SOC) || hv_write_siz(out, &siz) || hv_write_cod(out, &cod) ||
             hv_write_qcd(out, &qcd) || hv_write_com(out, 1, (const uint8_t *)"test", 4) ||
             hv_begin_tile_part(out, 0, 0, 1, &start) || hv_write_plt(out, lengths, count) ||
             hv_write_marker(out, HV_SOD);
    for (i = 0; i < count && status == 0; i++)
        status = hv_write_bytes(out, "\x01", 1);
    if (status == 0)
        status = hv_end_tile_part(out, start) || hv_write_marker(out, HV_EOC);
    free(lengths);
    return status ? -1 : 0;
}

/* What write_codestream wrote element by element, read back item by item:
 * SIZ and its components, COM and its text, and the PLT split the way
 * Kakadu splits it, each Lxxx measured. */
static void check_elements(const hv_out *out, size_t count) {
    hv_codestream cs;
    hv_item item;
    size_t entries = 0, segments = 0, first = 0;
    int status, siz = 0, com = 0;

    if (hv_codestream_open(&cs, out->data, 0, out->size, HV_READ_VALIDATE) != 0) {
        check(0, "the codestream opens", cs.error);
        hv_codestream_close(&cs);
        return;
    }
    while ((status = hv_codestream_next(&cs, &item)) == 1) {
        if (item.siz != NULL)
            siz = item.siz == hv_codestream_siz(&cs) &&
                  item.siz->ncomponents == components_for(count) &&
                  item.siz->fixed->csiz == components_for(count) &&
                  item.siz->components[0].depthMinus1 == 7 &&
                  item.end - item.start == 2 + 38 + 3 * components_for(count);  /* code, Lsiz */
        if (item.com != NULL)
            com = item.com->rcom == 1 && item.com->size == 4 &&
                  memcmp(item.com->text, "test", 4) == 0 &&
                  item.com->text == out->data + item.end - 4;
        if (item.plt != NULL) {
            size_t pos = item.plt->start, n = 0;
            uint64_t v;
            int more;
            while ((more = hv_plt_next(out->data, &pos, item.plt->end, &v)) == 1 && v == 1)
                n++;
            check(more == 0 && pos == item.end && item.plt->end == item.end &&
                  item.plt->start == item.start + 5 && item.plt->zplt == segments,
                  "PLT entries read back", NULL);
            if (segments++ == 0)
                first = n;
            entries += n;
        }
    }
    check(status == 0, "the codestream reads back item by item", cs.error);
    check(siz, "SIZ read back", NULL);
    check(com, "COM read back, its text in place", NULL);
    /* Lplt 65,535: its own 2 bytes, Zplt, and 65,532 one-byte entries. */
    check(segments == 2 && first == 65532 && entries == count, "PLT split at Lplt 65,535",
          NULL);
    hv_codestream_close(&cs);
}

/* Every write leaves the spare capacity zero, and appending after
 * hv_out_rewind over nonzero bytes gives what a fresh hv_out gives: the
 * generated encoders skip zero bits instead of writing them. */
static void check_spare_capacity(void) {
    hv_out fresh, reused;
    uint8_t ones[70000];
    const char *error;

    hv_out_init(&fresh);
    hv_out_init(&reused);
    memset(ones, 0xFF, sizeof ones);
    check(hv_write_bytes(&reused, ones, sizeof ones) == 0 && spare_zero(&reused),
          "spare capacity after hv_write_bytes", reused.error);
    hv_out_rewind(&reused, 1);
    check(reused.size == 1 && spare_zero(&reused), "spare capacity after hv_out_rewind", NULL);
    hv_out_rewind(&reused, 0);
    check(write_codestream(&fresh, 70000) == 0, "codestream written", fresh.error);
    check(write_codestream(&reused, 70000) == 0, "codestream written after a rewind",
          reused.error);
    check(spare_zero(&fresh) && spare_zero(&reused), "spare capacity after the encoders", NULL);
    check(fresh.size == reused.size && memcmp(fresh.data, reused.data, fresh.size) == 0,
          "a rewound hv_out writes what a fresh one does", NULL);
    /* Two PLT segments (Zplt 0 and 1), Psot, and the PLT sums. */
    error = hv_codestream_check(fresh.data, 0, fresh.size, HV_READ_VALIDATE, &(size_t){0});
    check(error == NULL, "the codestream reads back", error);
    check_elements(&fresh, 70000);
    hv_out_free(&fresh);
    hv_out_free(&reused);
}

/* The first box of buf[0, size), in *box: 1 if there is one. */
static int read_box(const uint8_t *buf, size_t size, hv_box *box) {
    hv_boxes it;
    const char *error;
    size_t at;
    hv_boxes_file(&it, buf, size);
    return hv_boxes_next(&it, box, &error, &at) == 1;
}

static void check_xlbox(void) {
    hv_out out;
    hv_box box, inner;
    hv_boxes it;
    uint8_t payload[200];
    size_t start, inner_start, i, at;
    const char *error;
    int same = 1;

    for (i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i + 1);

    /* A box of exactly HV_LBOX_MAX bytes keeps LBox. */
    hv_out_init(&out);
    check(hv_begin_box(&out, HV_BOX_XML, 0, &start) == 0 &&
          hv_write_bytes(&out, payload, HV_LBOX_MAX - HV_BOX_HEADER) == 0 &&
          hv_end_box(&out, start) == 0 && out.size == HV_LBOX_MAX && read_box(out.data,
          out.size, &box) && box.payload == HV_BOX_HEADER && box.end == HV_LBOX_MAX,
          "a box of HV_LBOX_MAX bytes", out.error);
    hv_out_free(&out);

    /* One byte more: XLBox, the payload moved up 8 bytes and intact. */
    hv_out_init(&out);
    check(hv_begin_box(&out, HV_BOX_XML, 0, &start) == 0 &&
          hv_write_bytes(&out, payload, HV_LBOX_MAX - HV_BOX_HEADER + 1) == 0 &&
          hv_end_box(&out, start) == 0 && read_box(out.data, out.size, &box) &&
          box.type == HV_BOX_XML && box.payload == HV_BOX_HEADER_XL &&
          box.end == out.size && out.size == HV_LBOX_MAX + 1 + HV_BOX_HEADER_XL - HV_BOX_HEADER &&
          memcmp(out.data + box.payload, payload, HV_LBOX_MAX - HV_BOX_HEADER + 1) == 0 &&
          spare_zero(&out), "a box switched to XLBox", out.error);
    hv_out_free(&out);

    /* Begun extended: XLBox from the start. */
    hv_out_init(&out);
    check(hv_begin_box(&out, HV_BOX_XML, 1, &start) == 0 &&
          hv_write_bytes(&out, payload, 10) == 0 && hv_end_box(&out, start) == 0 &&
          read_box(out.data, out.size, &box) && box.payload == HV_BOX_HEADER_XL &&
          box.end == HV_BOX_HEADER_XL + 10, "a box begun extended", out.error);
    hv_out_free(&out);

    /* Nested: the inner box switches first, then the outer one. */
    hv_out_init(&out);
    check(hv_begin_box(&out, HV_BOX_JP2H, 0, &start) == 0 &&
          hv_begin_box(&out, HV_BOX_XML, 0, &inner_start) == 0 &&
          hv_write_bytes(&out, payload, sizeof payload) == 0 &&
          hv_end_box(&out, inner_start) == 0 && hv_end_box(&out, start) == 0,
          "nested boxes written", out.error);
    if (read_box(out.data, out.size, &box) && box.type == HV_BOX_JP2H &&
        box.payload == HV_BOX_HEADER_XL && box.end == out.size) {
        hv_boxes_children(&it, out.data, &box);
        same = hv_boxes_next(&it, &inner, &error, &at) == 1 && inner.type == HV_BOX_XML &&
               inner.payload == box.payload + HV_BOX_HEADER_XL && inner.end == box.end &&
               memcmp(out.data + inner.payload, payload, sizeof payload) == 0;
    } else {
        same = 0;
    }
    check(same, "nested boxes switched to XLBox", NULL);
    hv_out_free(&out);

    /* hv_write_box_header: XLBox for a payload above HV_LBOX_MAX - 8. */
    hv_out_init(&out);
    check(hv_write_box_header(&out, HV_BOX_XML, HV_LBOX_MAX - HV_BOX_HEADER) == 0 &&
          out.size == HV_BOX_HEADER, "box header with LBox", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_box_header(&out, HV_BOX_XML, HV_LBOX_MAX - HV_BOX_HEADER + 1) == 0 &&
          out.size == HV_BOX_HEADER_XL &&
          hv_write_bytes(&out, payload, HV_LBOX_MAX - HV_BOX_HEADER + 1) == 0 &&
          read_box(out.data, out.size, &box) && box.payload == HV_BOX_HEADER_XL &&
          box.end == out.size, "box header with XLBox", out.error);
    hv_out_free(&out);

    /* A whole codestream in a jp2c box that switches to XLBox. */
    hv_out_init(&out);
    check(hv_begin_box(&out, HV_BOX_JP2C, 0, &start) == 0 && write_codestream(&out, 3) == 0 &&
          hv_end_box(&out, start) == 0 && read_box(out.data, out.size, &box) &&
          box.payload == HV_BOX_HEADER_XL &&
          hv_codestream_check(out.data, box.payload, box.end, HV_READ_VALIDATE, &at) == NULL,
          "codestream in a jp2c box with XLBox", out.error);
    hv_out_free(&out);
}

/* PLT entries of every size, 1 to 10 bytes, each encoded on its own, read
 * back with hv_plt_next. */
static void check_plt_entries(void) {
    uint64_t lengths[11], v;
    hv_out out;
    size_t i, pos = 5;          /* after the code, Lplt and Zplt */
    int ok = 1;

    for (i = 0; i < 10; i++)
        lengths[i] = (uint64_t)1 << (7 * i);         /* i + 1 bytes */
    lengths[10] = UINT64_MAX;                        /* 10 bytes */
    hv_out_init(&out);
    check(hv_write_plt(&out, lengths, 11) == 0 && out.size == 5 + 55 + 10 &&
          out.data[2] == 0 && out.data[3] == 68 && out.data[4] == 0, "PLT of 11 entries",
          out.error);
    for (i = 0; i < 11 && ok; i++)
        ok = hv_plt_next(out.data, &pos, out.size, &v) == 1 && v == lengths[i];
    check(ok && hv_plt_next(out.data, &pos, out.size, &v) == 0,
          "PLT entries of 1 to 10 bytes read back", NULL);
    hv_out_free(&out);
}

/* The boxes written element by element with LBox measured, byte for
 * byte: ftyp (BR, MinV, the compatibility list), cmap (CMP, MTYP, PCOL per
 * entry), flst (NF 1 and a Fragment), url (VERS, FLAG, LOC and its NUL)
 * and rreq (RreqHeader with ML inserted, the standard features, NVF, the
 * vendor features). */
static void check_jpx_boxes(void) {
    static const uint8_t flst[] = {
        0, 0, 0, 24, 'f', 'l', 's', 't', 0, 1,
        0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 3, 4, 0, 5};
    static const uint8_t url[] = {
        0, 0, 0, 15, 'u', 'r', 'l', ' ', 0, 0, 0, 0, 'a', 'b', 0};
    static const uint8_t rreq[] = {
        0, 0, 0, 39, 'r', 'r', 'e', 'q', 2, 0x80, 0x00, 0x40, 0x00, 0, 1,
        0, 5, 0x80, 0x00, 0, 1,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0x40, 0x00};
    static const uint8_t ftyp[] = {
        0, 0, 0, 24, 'f', 't', 'y', 'p', 'j', 'p', 'x', ' ', 0, 0, 0, 1,
        'j', 'p', 'x', ' ', 'j', 'p', '2', ' '};
    static const uint8_t cmap[] = {0, 0, 0, 16, 'c', 'm', 'a', 'p', 0, 2, 1, 3, 1, 0, 0, 0};
    static const uint8_t nlst[] = {0, 0, 0, 16, 'n', 'l', 's', 't', 1, 0, 0, 5, 0, 0, 0, 0};
    static const NlstEntry an[] = {{1, 5}, {0, 0}}, rendered = {0, 1};
    static const FtypHeader ftyp_header = {HV_BRAND_JPX, 1};
    static const Brand compatible[] = {HV_BRAND_JPX, HV_BRAND_JP2};
    CmapEntry entries[2] = {{2, 1, 3}, {256, 0, 0}};
    RreqHeader header;
    RreqStandardFeature standard;
    RreqVendorFeature vendor;
    size_t start;
    hv_out out;
    size_t i;

    hv_out_init(&out);
    check(hv_write_ftyp(&out, &ftyp_header, compatible, 2) == 0 && out.size == sizeof ftyp &&
          memcmp(out.data, ftyp, sizeof ftyp) == 0, "ftyp box", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_ftyp(&out, &ftyp_header, compatible, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "File Type box without a compatibility entry") == 0 && out.size == 0,
          "ftyp box without a compatibility entry", out.error);
    hv_out_rewind(&out, 0);
    check(hv_begin_box(&out, HV_BOX_CMAP, 0, &start) == 0 &&
          hv_write_cmap_entry(&out, &entries[0]) == 0 &&
          hv_write_cmap_entry(&out, &entries[1]) == 0 && hv_end_box(&out, start) == 0 &&
          out.size == sizeof cmap && memcmp(out.data, cmap, sizeof cmap) == 0, "cmap box",
          out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_nlst(&out, an, 2) == 0 && out.size == sizeof nlst &&
          memcmp(out.data, nlst, sizeof nlst) == 0, "nlst box", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_nlst(&out, an, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "Number List box without an entry") == 0 && out.size == 0,
          "nlst box without an entry", out.error);
    hv_out_rewind(&out, 0);
    /* Table M.42: the rendered result is 0x00000000 only. */
    check(hv_write_nlst(&out, &rendered, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid Number List entry") == 0,
          "nlst entry of the rendered result with a number", out.error);
    hv_out_free(&out);

    hv_out_init(&out);
    check(hv_write_flst(&out, 0x102, 0x304, 5) == 0 && out.size == sizeof flst &&
          memcmp(out.data, flst, sizeof flst) == 0, "flst box", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_url(&out, "ab") == 0 && out.size == sizeof url &&
          memcmp(out.data, url, sizeof url) == 0, "url box", out.error);
    hv_out_rewind(&out, 0);

    memset(&header, 0, sizeof header);
    memset(&standard, 0, sizeof standard);
    memset(&vendor, 0, sizeof vendor);
    header.fuam.nCount = header.dcm.nCount = 2;
    header.fuam.arr[0] = 0x80;
    header.dcm.arr[0] = 0x40;
    header.nsf = 1;
    standard.sf = 5;
    standard.sm.nCount = 2;
    standard.sm.arr[0] = 0x80;
    for (i = 0; i < 16; i++)
        vendor.vf.arr[i] = (byte)(i + 1);
    vendor.vm.nCount = 2;
    vendor.vm.arr[0] = 0x40;
    check(hv_write_rreq(&out, &header, &standard, 1, &vendor, 1) == 0 &&
          out.size == sizeof rreq && memcmp(out.data, rreq, sizeof rreq) == 0, "rreq box",
          out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_rreq(&out, &header, &standard, 0, NULL, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "Reader Requirements NSF other than the standard features given")
          == 0 && out.size == 0, "rreq NSF against the features", out.error);
    hv_out_rewind(&out, 0);
    vendor.vm.nCount = 1;
    check(hv_write_rreq(&out, &header, &standard, 1, &vendor, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "Reader Requirements masks of different lengths (ML)") == 0 &&
          out.size == 0, "rreq masks of different lengths", out.error);
    hv_out_free(&out);
}

/* Errors: the message, sticky until hv_out_rewind. */
static void check_errors(void) {
    static uint8_t body[65534];
    uint64_t lengths[2] = {1, 0};
    hv_out out;
    size_t start;

    hv_out_init(&out);
    check(hv_write_segment(&out, HV_COM, body, sizeof body) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment body longer than 65,533 bytes") == 0,
          "segment body too long", out.error);
    check(hv_write_marker(&out, HV_SOC) != 0 && out.size == 0, "writes after an error", NULL);
    hv_out_rewind(&out, 0);
    check(out.error == NULL && hv_write_segment(&out, HV_COM, body, sizeof body - 1) == 0 &&
          out.size == sizeof body - 1 + 4, "longest segment body", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_plt(&out, lengths, 2) != 0 && out.error != NULL &&
          strcmp(out.error, "zero packet length") == 0, "zero packet length", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_com(&out, 1, body, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "COM segment without text") == 0, "COM without text", out.error);
    hv_out_rewind(&out, 0);
    /* Lcom 65,535: its own 2 bytes, Rcom and 65,531 bytes of text. */
    check(hv_write_com(&out, 0, body, sizeof body - 3) == 0 && out.size == 2 + 65535 &&
          hv_write_com(&out, 0, body, sizeof body - 2) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment longer than 65,535 bytes (Lxxx)") == 0,
          "longest COM text, and one byte more", out.error);
    hv_out_rewind(&out, 0);
    check(hv_end_tile_part(&out, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "tile-part shorter than SOT and SOD") == 0,
          "tile-part end without a tile-part", out.error);
    hv_out_rewind(&out, 0);
    check(hv_begin_box(&out, HV_BOX_XML, 0, &start) == 0 && hv_end_box(&out, start + 1) != 0 &&
          strcmp(out.error, "box shorter than its header") == 0, "box end without a box",
          out.error);
    hv_out_free(&out);
}

/* hv_out_init sets every field, whatever was there; hv_out_free frees and
 * clears them. */
static void check_init_free(void) {
    hv_out out;
    memset(&out, 0xAA, sizeof out);
    hv_out_init(&out);
    check(out.data == NULL && out.size == 0 && out.capacity == 0 && out.error == NULL,
          "hv_out_init", NULL);
    check(hv_write_bytes(&out, "ab", 2) == 0, "a write", out.error);
    hv_out_free(&out);
    check(out.data == NULL && out.size == 0 && out.capacity == 0 && out.error == NULL,
          "hv_out_free", NULL);
}

/* Values for the segment and box writers. */
static SizFixed v_fixed;
static Component v_components[3];
static CodSegment v_cod;
static QcdSegment v_qcd;
static RreqHeader v_rreq;
static RreqStandardFeature v_standard[2];
static RreqVendorFeature v_vendor;
static uint64_t v_lengths[40];
static uint8_t v_bytes[300];

static void set_values(void) {
    size_t i;
    memset(&v_fixed, 0, sizeof v_fixed);
    v_fixed.xsiz = v_fixed.ysiz = v_fixed.xtsiz = v_fixed.ytsiz = 8;
    v_fixed.csiz = 3;
    memset(v_components, 0, sizeof v_components);
    for (i = 0; i < 3; i++) {
        v_components[i].depthMinus1 = 7;
        v_components[i].xrsiz = v_components[i].yrsiz = 1;
    }
    memset(&v_cod, 0, sizeof v_cod);
    v_cod.body.sgcod.layers = 1;
    v_cod.body.spcod.cbWidthExp = v_cod.body.spcod.cbHeightExp = 4;
    memset(&v_qcd, 0, sizeof v_qcd);
    v_qcd.body.sqcd = 0x40;
    v_qcd.body.spqcd.nCount = 1;
    v_qcd.body.spqcd.arr[0] = 0x48;
    memset(&v_rreq, 0, sizeof v_rreq);
    memset(v_standard, 0, sizeof v_standard);
    memset(&v_vendor, 0, sizeof v_vendor);
    v_rreq.fuam.nCount = v_rreq.dcm.nCount = 2;
    v_rreq.nsf = 2;
    for (i = 0; i < 2; i++) {
        v_standard[i].sf = (FeatureCode)(5 + i);
        v_standard[i].sm.nCount = 2;
    }
    v_vendor.vm.nCount = 2;
    for (i = 0; i < 40; i++)
        v_lengths[i] = (uint64_t)1 << (i % 64);
    for (i = 0; i < sizeof v_bytes; i++)
        v_bytes[i] = (uint8_t)(i + 1);
}

/* Every call of a scenario must return 0 exactly when out->error is NULL.
 * 1 when one does not. */
#define STEP(call)                                                           \
    do {                                                                     \
        int status_ = (call);                                                \
        if ((status_ == 0) != (out->error == NULL))                          \
            return 1;                                                        \
    } while (0)

static int s_bytes(hv_out *out) { STEP(hv_write_bytes(out, v_bytes, 300)); return 0; }
static int s_empty(hv_out *out) { STEP(hv_write_bytes(out, v_bytes, 0)); return 0; }
static int s_marker(hv_out *out) { STEP(hv_write_marker(out, HV_SOC)); return 0; }
static int s_segment(hv_out *out) { STEP(hv_write_segment(out, 0xFF64, v_bytes, 200)); return 0; }
static int s_siz(hv_out *out) {
    hv_siz siz = {&v_fixed, v_components, 3};
    STEP(hv_write_siz(out, &siz));
    return 0;
}
static int s_cod(hv_out *out) { STEP(hv_write_cod(out, &v_cod)); return 0; }
static int s_qcd(hv_out *out) { STEP(hv_write_qcd(out, &v_qcd)); return 0; }
static int s_com(hv_out *out) { STEP(hv_write_com(out, 1, v_bytes, 150)); return 0; }
static int s_plt(hv_out *out) { STEP(hv_write_plt(out, v_lengths, 40)); return 0; }
static int s_plt_none(hv_out *out) { STEP(hv_write_plt(out, v_lengths, 0)); return 0; }
static int s_plt_segment(hv_out *out) {
    STEP(hv_write_plt_segment(out, 3, v_lengths, 40));
    return 0;
}
static int s_tile_part(hv_out *out) {
    size_t start = 0;
    STEP(hv_begin_tile_part(out, 0, 0, 1, &start));
    STEP(hv_write_marker(out, HV_SOD));
    STEP(hv_write_bytes(out, v_bytes, 20));
    STEP(hv_end_tile_part(out, start));
    return 0;
}
static int s_box(hv_out *out, int extended, size_t payload) {
    size_t start = 0;
    STEP(hv_begin_box(out, HV_BOX_XML, extended, &start));
    STEP(hv_write_bytes(out, v_bytes, payload));
    STEP(hv_end_box(out, start));
    return 0;
}
static int s_box_short(hv_out *out) { return s_box(out, 0, 50); }
static int s_box_switched(hv_out *out) { return s_box(out, 0, 150); }
static int s_box_extended(hv_out *out) { return s_box(out, 1, 50); }
static int s_box_header(hv_out *out) {
    STEP(hv_write_box_header(out, HV_BOX_XML, 50));
    STEP(hv_write_box_header(out, HV_BOX_XML, 200));
    return 0;
}
static int s_flst(hv_out *out) { STEP(hv_write_flst(out, 12, 34, 1)); return 0; }
static int s_ndr(hv_out *out) { STEP(hv_write_ndr(out, 2)); return 0; }
static int s_url(hv_out *out) { STEP(hv_write_url(out, "file://./a.jp2")); return 0; }
static int s_ftyp(hv_out *out) {
    static const FtypHeader header = {HV_BRAND_JPX, 1};
    static const Brand compatible[] = {HV_BRAND_JPX, HV_BRAND_JP2};
    STEP(hv_write_ftyp(out, &header, compatible, 2));
    return 0;
}
static int s_cmap(hv_out *out) {
    CmapEntry entry = {2, 1, 3};
    size_t start = 0;
    STEP(hv_begin_box(out, HV_BOX_CMAP, 0, &start));
    STEP(hv_write_cmap_entry(out, &entry));
    STEP(hv_write_cmap_entry(out, &entry));
    STEP(hv_end_box(out, start));
    return 0;
}
static int s_nlst(hv_out *out) {
    static const NlstEntry entries[] = {{1, 0}, {2, 0}};
    STEP(hv_write_nlst(out, entries, 2));
    return 0;
}
static int s_rreq(hv_out *out) {
    STEP(hv_write_rreq(out, &v_rreq, v_standard, 2, &v_vendor, 1));
    return 0;
}

static const struct {
    const char *name;
    int (*run)(hv_out *);
} SCENARIOS[] = {
    {"hv_write_bytes", s_bytes}, {"hv_write_bytes of none", s_empty},
    {"hv_write_marker", s_marker}, {"hv_write_segment", s_segment}, {"hv_write_siz", s_siz},
    {"hv_write_cod", s_cod}, {"hv_write_qcd", s_qcd}, {"hv_write_com", s_com},
    {"hv_write_plt", s_plt}, {"hv_write_plt of none", s_plt_none},
    {"hv_write_plt_segment", s_plt_segment}, {"a tile-part", s_tile_part},
    {"a box", s_box_short}, {"a box switched to XLBox", s_box_switched},
    {"a box begun extended", s_box_extended}, {"hv_write_box_header", s_box_header},
    {"hv_write_flst", s_flst}, {"hv_write_ndr", s_ndr}, {"hv_write_url", s_url},
    {"hv_write_rreq", s_rreq}, {"hv_write_ftyp", s_ftyp}, {"hv_write_cmap_entry", s_cmap},
    {"hv_write_nlst", s_nlst},
};

/* After an error, every write returns -1 and changes nothing, the error
 * included, until hv_out_rewind. */
static void check_sticky_errors(void) {
    size_t i;
    for (i = 0; i < sizeof SCENARIOS / sizeof *SCENARIOS; i++) {
        hv_out out;
        const char *error;
        hv_out_init(&out);
        hv_write_bytes(&out, v_bytes, 10);
        hv_write_com(&out, 0, v_bytes, 0);
        error = out.error;
        check(SCENARIOS[i].run(&out) == 0 && out.size == 10 && out.error == error &&
              spare_zero(&out), "writes after an error", SCENARIOS[i].name);
        hv_out_free(&out);
    }
}

/* realloc failing at every step of every write: the result agrees with
 * out->error, which is "out of memory", and the spare capacity stays zero.
 * A prefill of `size` bytes leaves `free` bytes before the buffer grows. */
static void run_out_of_memory(const char *name, int (*run)(hv_out *), size_t size,
                              size_t from, size_t to) {
    size_t free_bytes;
    int failed = 0, passed = 0;
    uint8_t *zeros = calloc(size, 1);
    if (zeros == NULL) {
        check(0, "out of memory for the test", name);
        return;
    }
    for (free_bytes = from; free_bytes <= to; free_bytes++) {
        hv_out out;
        int bad;
        hv_out_init(&out);
        if (hv_write_bytes(&out, zeros, size - free_bytes) != 0 || out.capacity != size) {
            check(0, "prefill", name);
            hv_out_free(&out);
            break;
        }
        fail_realloc = 1;
        bad = run(&out);
        fail_realloc = 0;
        if (out.error != NULL) {
            failed = 1;
            bad |= strcmp(out.error, "out of memory") != 0;
        } else {
            passed = 1;
        }
        if (bad || !spare_zero(&out)) {
            char detail[128];
            snprintf(detail, sizeof detail, "%s, %zu bytes free: %s", name, free_bytes,
                     out.error ? out.error : "no error");
            check(0, "out of memory", detail);
        }
        hv_out_free(&out);
    }
    check(failed && (passed || to < from), "out of memory reached and passed", name);
    free(zeros);
}

static int s_plt_split(hv_out *out) {
    static uint64_t *ones;
    size_t i;
    if (ones == NULL && (ones = malloc(65533 * sizeof *ones)) != NULL)
        for (i = 0; i < 65533; i++)
            ones[i] = 1;
    if (ones == NULL)
        return 1;
    STEP(hv_write_plt(out, ones, 65533));
    return 0;
}

static void check_out_of_memory(void) {
    size_t i;
    for (i = 0; i < sizeof SCENARIOS / sizeof *SCENARIOS; i++)
        if (SCENARIOS[i].run != s_plt_none && SCENARIOS[i].run != s_empty)
            run_out_of_memory(SCENARIOS[i].name, SCENARIOS[i].run, 8192, 0, 700);
    /* The second PLT segment: 65,537 bytes of the first one, then its code,
     * Lplt, Zplt and entry. */
    run_out_of_memory("the PLT split", s_plt_split, 262144, 65537, 65537 + 40);
}

/* Values outside their types fail in the generated encoders, which check
 * the constraints (T.800 Tables A.9 and A.11, A.44; T.801 Table M.17). */
static void check_constraints(void) {
    hv_out out;
    hv_siz siz = {&v_fixed, v_components, 3};

    hv_out_init(&out);
    v_fixed.xsiz = 0;
    check(hv_write_siz(&out, &siz) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid SIZ segment") == 0, "SIZ with Xsiz 0", out.error);
    v_fixed.xsiz = 8;
    hv_out_rewind(&out, 0);
    v_components[2].xrsiz = 0;
    check(hv_write_siz(&out, &siz) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid SIZ segment") == 0, "SIZ with XRsiz 0", out.error);
    v_components[2].xrsiz = 1;
    hv_out_rewind(&out, 0);
    siz.ncomponents = 2;                            /* Csiz 3 */
    check(hv_write_siz(&out, &siz) != 0 && out.error != NULL &&
          strcmp(out.error, "siz.csiz-count") == 0 && out.size == 0, "SIZ with Csiz 3 of 2",
          out.error);
    siz.ncomponents = 3;
    hv_out_rewind(&out, 0);
    check(hv_write_box_header(&out, HV_BOX_XML, UINT64_MAX - HV_BOX_HEADER_XL) == 0 &&
          out.size == HV_BOX_HEADER_XL, "box header with XLBox 2^64 - 1", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_box_header(&out, HV_BOX_XML, UINT64_MAX - HV_BOX_HEADER_XL + 1) != 0 &&
          out.error != NULL && strcmp(out.error, "box longer than 2^64 - 1 bytes") == 0 &&
          out.size == 0, "box header over 2^64 - 1 bytes", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_com(&out, 2, v_bytes, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid COM segment") == 0, "COM with Rcom 2", out.error);
    hv_out_rewind(&out, 0);
    {
        CmapEntry entry = {0, 2, 0};                /* MTYP 0 or 1 (I.5.3.5) */
        hv_out_rewind(&out, 0);
        check(hv_write_cmap_entry(&out, &entry) != 0 && out.error != NULL &&
              strcmp(out.error, "invalid Component Mapping entry") == 0 && out.size == 0,
              "cmap entry with MTYP 2", out.error);
        hv_out_rewind(&out, 0);
    }
    check(hv_write_flst(&out, 11, 1, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid Fragment List box contents") == 0, "flst with OFF 11",
          out.error);
    check(spare_zero(&out), "spare capacity after invalid values", NULL);
    hv_out_free(&out);
}

/* hv_write_plt_segment: Zplt 0 to 255, one entry or more, none zero, and
 * Lplt up to 65,535; hv_write_plt of no length writes nothing. */
static void check_plt_segment(void) {
    static uint64_t ones[65533];
    hv_out out;
    size_t i;

    for (i = 0; i < 65533; i++)
        ones[i] = 1;
    hv_out_init(&out);
    check(hv_write_plt_segment(&out, 255, ones, 2) == 0 && out.size == 7 &&
          out.data[3] == 5 && out.data[4] == 255 && out.data[5] == 1 && out.data[6] == 1,
          "PLT segment with Zplt 255", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_plt_segment(&out, 256, ones, 2) != 0 && out.error != NULL &&
          strcmp(out.error, "more than 256 PLT segments in a tile-part") == 0,
          "PLT segment with Zplt 256", out.error);
    hv_out_rewind(&out, 0);
    ones[1] = 0;
    check(hv_write_plt_segment(&out, 0, ones, 2) != 0 && out.error != NULL &&
          strcmp(out.error, "zero packet length") == 0, "PLT segment with a zero length",
          out.error);
    ones[1] = 1;
    hv_out_rewind(&out, 0);
    check(hv_write_plt_segment(&out, 0, ones, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "PLT segment without entries") == 0, "PLT segment of no entry",
          out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_plt_segment(&out, 0, ones, 65532) == 0 && out.size == 2 + 65535 &&
          hv_write_plt_segment(&out, 1, ones, 65533) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment longer than 65,535 bytes (Lxxx)") == 0,
          "longest PLT segment, and one entry more", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_plt(&out, ones, 0) == 0 && out.size == 0 && out.error == NULL,
          "PLT of no length", out.error);
    hv_out_free(&out);
}

/* The lengths the writer measures, at their limits and without their
 * segment or box: Lxxx 2 for an empty body, Psot up to HV_PSOT_MAX, a
 * tile-part or box end at a wrong offset. */
static void check_lengths(void) {
    static uint8_t body[HV_PSOT_MAX];
    static const uint8_t lbox5[] = {0, 0, 0, 5, 'x', 'm', 'l', ' ', 0, 0};
    hv_out out;
    SotSegment sot;
    size_t start = 0;           /* set by hv_begin_tile_part, if reached */

    hv_out_init(&out);
    check(hv_write_segment(&out, 0xFF64, NULL, 0) == 0 && out.size == 4 && out.data[2] == 0 &&
          out.data[3] == 2, "segment with an empty body: Lxxx 2", out.error);
    hv_out_rewind(&out, 0);
    /* SOT (12 bytes), SOD, and the rest up to Psot = HV_PSOT_MAX. */
    check(hv_write_marker(&out, HV_SOC) == 0 && hv_begin_tile_part(&out, 0, 0, 1, &start) == 0 &&
          hv_write_marker(&out, HV_SOD) == 0 &&
          hv_write_bytes(&out, body, HV_PSOT_MAX - 14) == 0 && hv_end_tile_part(&out, start) == 0,
          "tile-part of HV_PSOT_MAX bytes", out.error);
    check(READ_BACK(SotSegment, &sot, &out, start + MARKER, HV_FIXED(SotSegment)) == 0 &&
          sot.psot == HV_PSOT_MAX, "Psot HV_PSOT_MAX read back", out.error);
    check(hv_write_bytes(&out, body, 1) == 0 && hv_end_tile_part(&out, start) != 0 &&
          out.error != NULL &&
          strcmp(out.error, "tile-part longer than 4,294,967,295 bytes (Psot)") == 0,
          "tile-part of HV_PSOT_MAX + 1 bytes", out.error);
    hv_out_rewind(&out, 0);
    check(hv_begin_tile_part(&out, 0, 0, 1, &start) == 0 && hv_write_bytes(&out, body, 1) == 0 &&
          hv_end_tile_part(&out, start) != 0 && out.error != NULL &&
          strcmp(out.error, "tile-part shorter than SOT and SOD") == 0,
          "tile-part of 13 bytes", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_bytes(&out, body, 5) == 0 && hv_begin_tile_part(&out, 0, 0, 1, &start) == 0 &&
          hv_end_tile_part(&out, start) != 0 && out.error != NULL &&
          strcmp(out.error, "tile-part shorter than SOT and SOD") == 0,
          "tile-part of 12 bytes after others", out.error);
    hv_out_rewind(&out, 0);
    check(hv_begin_tile_part(&out, 0, 0, 1, &start) == 0 && hv_write_marker(&out, HV_SOD) == 0 &&
          hv_write_bytes(&out, body, 10) == 0 && hv_end_tile_part(&out, start + 1) != 0 &&
          out.error != NULL && strcmp(out.error, "no SOT segment at the given offset") == 0,
          "tile-part end at a wrong offset", out.error);
    hv_out_rewind(&out, 0);
    check(hv_write_bytes(&out, lbox5, sizeof lbox5) == 0 && hv_end_box(&out, 0) != 0 &&
          out.error != NULL && strcmp(out.error, "no box header at the given offset") == 0,
          "box end on LBox 5", out.error);
    hv_out_free(&out);
}

/* hv_write_bytes up to SIZE_MAX bytes in all. */
static void check_size_limit(void) {
    hv_out out;
    hv_out_init(&out);
    check(hv_write_bytes(&out, "a", 1) == 0 && hv_write_bytes(&out, "a", SIZE_MAX) != 0 &&
          out.error != NULL && strcmp(out.error, "output larger than SIZE_MAX bytes") == 0 &&
          out.size == 1, "output over SIZE_MAX bytes", out.error);
    hv_out_rewind(&out, 1);
    check(hv_write_bytes(&out, "a", SIZE_MAX - 1) != 0 && out.error != NULL &&
          strcmp(out.error, "out of memory") == 0 && out.size == 1 && spare_zero(&out),
          "output of SIZE_MAX bytes", out.error);
    hv_out_free(&out);
}

/* The Reader Requirements masks: every one ML bytes. */
static void check_rreq_masks(void) {
    hv_out out;
    hv_out_init(&out);
    v_standard[0].sm.nCount = 1;
    check(hv_write_rreq(&out, &v_rreq, v_standard, 2, &v_vendor, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "Reader Requirements masks of different lengths (ML)") == 0 &&
          out.size == 0, "rreq: the first standard feature's mask", out.error);
    v_standard[0].sm.nCount = 2;
    hv_out_rewind(&out, 0);
    v_rreq.dcm.nCount = 1;
    check(hv_write_rreq(&out, &v_rreq, v_standard, 2, &v_vendor, 1) != 0 && out.error != NULL &&
          strcmp(out.error, "Reader Requirements masks of different lengths (ML)") == 0 &&
          out.size == 0, "rreq: DCM", out.error);
    v_rreq.dcm.nCount = 2;
    hv_out_free(&out);
}

/* The static helpers on offsets no public function passes them: they fail
 * with their own message. */
typedef int TestFail;
typedef int TestSkip;
#define TestFail_REQUIRED_BYTES_FOR_ACN_ENCODING 4
#define TestSkip_REQUIRED_BYTES_FOR_ACN_ENCODING 2

/* An encoder that writes and then fails. */
static flag TestFail_ACN_Encode(const TestFail *value, BitStream *s, int *err, flag check) {
    (void)value;
    (void)check;
    BitStream_AppendByte(s, 0xFF, FALSE);
    BitStream_AppendByte(s, 0xFF, FALSE);
    *err = 1;
    return FALSE;
}

/* An encoder that writes 16 zero bits by skipping them, as the generated
 * ones do. */
static flag TestSkip_ACN_Encode(const TestSkip *value, BitStream *s, int *err, flag check) {
    (void)value;
    (void)check;
    *err = 0;
    BitStream_AppendNBitZero(s, 16);
    return TRUE;
}

DEFINE_ENCODE(TestFail, "test value")
DEFINE_ENCODE(TestSkip, "test value")

static void check_helpers(void) {
    static const uint8_t boxes[] = {0, 0, 0, 8, 'x', 'm', 'l', ' ', 0, 0, 0, 8, 'x', 'm', 'l', ' '};
    hv_out out;
    SegmentLength l = 2;
    BoxHeader h;
    TestFail f = 0;
    TestSkip k = 0;
    size_t written;

    hv_out_init(&out);
    hv_write_bytes(&out, boxes, 6);
    check(ENCODE(SegmentLength, &l, &out, 7, 0, &written) != 0 && out.error != NULL &&
          strcmp(out.error, "no marker segment length at the given offset") == 0,
          "encoder past the output", out.error);
    hv_out_rewind(&out, 6);
    check(ENCODE(SegmentLength, &l, &out, 1, 6, &written) != 0 && out.error != NULL &&
          strcmp(out.error, "no marker segment length at the given offset") == 0,
          "encoder clearing past the output", out.error);
    hv_out_rewind(&out, 0);
    hv_write_bytes(&out, boxes, sizeof boxes);
    check(READ_BACK(BoxHeader, &h, &out, out.size + 1, 8) != 0 && out.error != NULL &&
          strcmp(out.error, "no box header at the given offset") == 0,
          "read back past the output", out.error);
    hv_out_rewind(&out, sizeof boxes);
    check(READ_BACK(BoxHeader, &h, &out, 4, out.size - 3) != 0 && out.error != NULL &&
          strcmp(out.error, "no box header at the given offset") == 0,
          "read back of more than the output", out.error);
    /* Segment ends on 3 bytes, on 3 of 5 bytes, and past 5 bytes. */
    hv_out_rewind(&out, 3);
    check(out.size == 3 && end_segment(&out, 0) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment shorter than its code and Lxxx") == 0,
          "segment end after 3 bytes", out.error);
    hv_out_rewind(&out, 0);
    hv_write_bytes(&out, boxes, 5);
    check(end_segment(&out, 2) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment shorter than its code and Lxxx") == 0,
          "segment end 3 bytes before the end", out.error);
    hv_out_rewind(&out, 5);
    check(end_segment(&out, 6) != 0 && out.error != NULL &&
          strcmp(out.error, "marker segment shorter than its code and Lxxx") == 0,
          "segment end past the output", out.error);
    hv_out_rewind(&out, 5);
    check(ENCODE(TestFail, &f, &out, out.size, 0, &written) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid test value") == 0 && out.size == 5 && spare_zero(&out),
          "a failing encoder's bytes cleared", out.error);
    /* The same with the encoder's room up to the end of the buffer. */
    hv_out_rewind(&out, 0);
    while (out.error == NULL && out.size < out.capacity - TestFail_REQUIRED_BYTES_FOR_ACN_ENCODING)
        hv_write_bytes(&out, "\xFF", 1);
    check(ENCODE(TestFail, &f, &out, out.size, 0, &written) != 0 && out.error != NULL &&
          strcmp(out.error, "invalid test value") == 0 && spare_zero(&out),
          "a failing encoder's bytes cleared at the end of the buffer", out.error);
    hv_out_rewind(&out, 0);
    hv_write_bytes(&out, "\xFF\xFF\xFF\xFF", 4);
    check(ENCODE(TestSkip, &k, &out, 1, 2, &written) == 0 && written == 2 && out.size == 4 &&
          memcmp(out.data, "\xFF\x00\x00\xFF", 4) == 0, "an in-place patch clears its bytes",
          out.error);
    hv_out_free(&out);
}

int main(void) {
    set_values();
    check_init_free();
    check_sticky_errors();
    check_out_of_memory();
    check_constraints();
    check_plt_segment();
    check_lengths();
    check_size_limit();
    check_rreq_masks();
    check_helpers();
    check_spare_capacity();
    check_xlbox();
    check_plt_entries();
    check_jpx_boxes();
    check_errors();
    printf(failures ? "%d failures\n" : "all checks passed\n", failures);
    return failures != 0;
}
