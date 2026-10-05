/* hv_rewrite.c: see hv_rewrite.h. */
#include "hv_rewrite.h"

#include "hv_codes.h"
#include "hv_decode.h"
#include "jp2-boxes.h"        /* Fragment */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* A marker code, and the marker and length that start a marker segment
 * (T.800 A.1). */
#define MARKER HV_FIXED(MarkerCode)
#define SEGMENT_START (HV_FIXED(MarkerCode) + HV_FIXED(SegmentLength))

HV_DEFINE_DECODE(FtypHeader)
HV_DEFINE_DECODE(Brand)
HV_DEFINE_DECODE(CmapEntry)
HV_DEFINE_DECODE(FragmentCount)
HV_DEFINE_DECODE(Fragment)
HV_DEFINE_DECODE(UrlHeader)
HV_DEFINE_DECODE(DataReferenceCount)
HV_DEFINE_DECODE(RreqHeader)
HV_DEFINE_DECODE(RreqStandardFeature)
HV_DEFINE_DECODE(FeatureCount)
HV_DEFINE_DECODE(RreqVendorFeature)

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

typedef struct {
    const uint8_t *buf;
    hv_read_mode mode;
    hv_out *out;
    hv_rewrite_result *r;
    uint64_t *lengths;      /* the nonzero entries of a PLT segment */
    size_t nlengths, maxlengths;
    unsigned zplt;          /* Zplt of the next PLT segment written */
    unsigned zplt_in;       /* PLT segments read in the tile-part header */
    const char *refused;    /* why the writer cannot write the input, or NULL */
} rewrite;

static void differs(rewrite *w, const char *why) {
    if (w->r->uncomparable == NULL)
        w->r->uncomparable = why;
}

static int add_length(rewrite *w, uint64_t v) {
    if (w->nlengths == w->maxlengths) {
        size_t n = w->maxlengths ? 2 * w->maxlengths : 1024;
        uint64_t *p = realloc(w->lengths, n * sizeof *p);
        if (p == NULL)
            return -1;
        w->lengths = p;
        w->maxlengths = n;
    }
    w->lengths[w->nlengths++] = v;
    return 0;
}

/* One PLT segment as the input splits them, with its nonzero entries.
 * The writer writes each entry in as few 7-bit groups as its value needs
 * (a first byte 0x80 is a leading zero group). */
static int write_plt(rewrite *w, const hv_item *item) {
    size_t pos = item->plt->start, entry = pos;
    uint64_t v;
    int more;

    /* T.800 A.7.3 lets the segments of a header come in any order, the
     * entries following their Zplt; the writer writes them in segment
     * order, Zplt 0, 1, 2, ... */
    if (item->plt->zplt != w->zplt_in++) {
        w->refused = "PLT segments out of Zplt order, which the writer writes in order";
        return -1;
    }
    w->nlengths = 0;
    while ((more = hv_plt_next(w->buf, &pos, item->plt->end, &v)) == 1) {
        if (w->buf[entry] == 0x80)
            differs(w, "PLT entries in more bytes than their values need");
        if (v == 0)
            differs(w, "zero PLT entries are dropped");
        else if (add_length(w, v) != 0)
            return -1;
        entry = pos;
    }
    if (more < 0)
        return -1;
    if (w->nlengths == 0)
        return 0;
    return hv_write_plt_segment(w->out, w->zplt++, w->lengths, w->nlengths);
}

/* Writes one codestream item. */
static int write_item(rewrite *w, const hv_item *item, size_t *tp_start) {
    hv_out *out = w->out;

    switch (item->kind) {
    case HV_TILE_PART:
        if (item->sot.psot == 0)
            differs(w, "Psot = 0 is written as a length");
        w->zplt = w->zplt_in = 0;
        return hv_begin_tile_part(out, (uint16_t)item->sot.isot, (uint8_t)item->sot.tpsot,
                                  (uint8_t)item->sot.tnsot, tp_start);
    case HV_TILE_DATA:
        if (hv_write_marker(out, HV_SOD) != 0 ||
            hv_write_bytes(out, w->buf + item->start, item->end - item->start) != 0)
            return -1;
        return hv_end_tile_part(out, *tp_start);
    case HV_END:
        return hv_write_marker(out, HV_EOC);
    case HV_SEGMENT:
    case HV_TILE_SEGMENT:
        if (item->plt)
            return write_plt(w, item);
        if (item->siz)
            return hv_write_siz(out, item->siz);
        if (item->cod)
            return hv_write_cod(out, item->cod);
        if (item->qcd)
            return hv_write_qcd(out, item->qcd);
        if (item->com)
            return hv_write_com(out, item->com->rcom, item->com->text, item->com->size);
        if (item->end - item->start == MARKER)
            return hv_write_marker(out, item->code);
        return hv_write_segment(out, item->code, w->buf + item->start + SEGMENT_START,
                                item->end - item->start - SEGMENT_START);
    }
    return -1;
}

static int write_failed(rewrite *w, size_t at) {
    w->r->error = w->refused ? w->refused : w->out->error ? w->out->error : "out of memory";
    w->r->at = at;
    return -1;
}

static int rewrite_codestream(rewrite *w, size_t start, size_t end) {
    hv_codestream cs;
    hv_item item;
    size_t tp_start = 0;
    int status = hv_codestream_open(&cs, w->buf, start, end, w->mode);

    if (status == 0 && hv_write_marker(w->out, HV_SOC) != 0) {
        hv_codestream_close(&cs);
        return write_failed(w, start);
    }
    while (status == 0 && (status = hv_codestream_next(&cs, &item)) == 1) {
        if (write_item(w, &item, &tp_start) != 0) {
            hv_codestream_close(&cs);
            return write_failed(w, item.start);
        }
        if (item.kind == HV_END)
            break;
        status = 0;
    }
    if (status < 0) {
        w->r->error = cs.error;
        w->r->at = cs.error_at;
    }
    hv_codestream_close(&cs);
    return status < 0 ? -1 : 0;
}

/* A Reader Requirements box in hv_write_rreq's form: its parts decode
 * (as the reader decodes them), every mask has ML bytes and nothing
 * follows. 1 written, 0 not in that form, -1 on error. */
static int write_rreq(rewrite *w, const hv_box *box) {
    RreqHeader h;
    RreqStandardFeature *sf = NULL;
    RreqVendorFeature *vf = NULL;
    FeatureCount nvf;
    size_t p = box->payload, used, ml, i;
    int status = 0;

    if (HV_DECODE_USED(RreqHeader, &h, w->buf, p, box->end - p, &used) != 0)
        return 0;
    p += used;
    ml = (size_t)h.fuam.nCount;
    if ((size_t)h.dcm.nCount != ml)
        return 0;
    if (h.nsf != 0 && (sf = malloc((size_t)h.nsf * sizeof *sf)) == NULL)
        return -1;
    for (i = 0; i < h.nsf; i++, p += used)
        if (HV_DECODE_USED(RreqStandardFeature, &sf[i], w->buf, p,
                           min_size(box->end - p, HV_FIXED(FeatureCode) + ml), &used) != 0 ||
            (size_t)sf[i].sm.nCount != ml)
            goto done;
    if (HV_DECODE_USED(FeatureCount, &nvf, w->buf, p, box->end - p, &used) != 0)
        goto done;
    p += used;
    if (nvf != 0 && (vf = malloc((size_t)nvf * sizeof *vf)) == NULL) {
        status = -1;
        goto done;
    }
    for (i = 0; i < nvf; i++, p += used)
        if (HV_DECODE_USED(RreqVendorFeature, &vf[i], w->buf, p,
                           min_size(box->end - p, HV_FIXED(VendorId) + ml), &used) != 0 ||
            (size_t)vf[i].vm.nCount != ml)
            goto done;
    if (p == box->end)
        status = hv_write_rreq(w->out, &h, sf, (size_t)h.nsf, vf, (size_t)nvf) != 0 ? -1 : 1;
done:
    free(sf);
    free(vf);
    return status;
}

/* A box with a short header that a hv_write_* function writes whole, when
 * its contents are in that function's form. 1 written, 0 not, -1 on
 * error. */
static int write_box_function(rewrite *w, const hv_box *box) {
    const uint8_t *buf = w->buf;
    size_t n = box->end - box->payload, used;

    if (box->payload - box->start != HV_BOX_HEADER || box->to_end)
        return 0;
    if (box->type == HV_BOX_FTYP) {
        /* BR and MinV, then one or more whole compatibility entries. */
        FtypHeader h;
        Brand *compatible;
        size_t count, i, one;
        int status = 1;
        if (HV_DECODE_USED(FtypHeader, &h, buf, box->payload, n, &used) != 0 || n == used ||
            (n - used) % HV_FIXED(Brand) != 0)
            return 0;
        count = (n - used) / HV_FIXED(Brand);
        if ((compatible = malloc(count * sizeof *compatible)) == NULL)
            return -1;
        for (i = 0; i < count && status == 1; i++)
            if (HV_DECODE_USED(Brand, &compatible[i], buf,
                               box->payload + used + i * HV_FIXED(Brand), HV_FIXED(Brand),
                               &one) != 0)
                status = 0;
        if (status == 1 && hv_write_ftyp(w->out, &h, compatible, count) != 0)
            status = -1;
        free(compatible);
        return status;
    }
    if (box->type == HV_BOX_FLST) {
        /* NF 1 and one fragment: FragmentList-Profile, hv_write_flst's. */
        FragmentCount nf;
        Fragment f;
        size_t more;
        if (HV_DECODE_USED(FragmentCount, &nf, buf, box->payload, n, &used) != 0 || nf != 1 ||
            n - used != HV_FIXED(Fragment) ||
            HV_DECODE_USED(Fragment, &f, buf, box->payload + used, n - used, &more) != 0)
            return 0;
        return hv_write_flst(w->out, f.off, (uint32_t)f.len, (uint16_t)f.dr) != 0 ? -1 : 1;
    }
    if (box->type == HV_BOX_URL) {
        /* VERS and FLAG (0, the type's only values), then LOC and its NUL
         * at the end of the box. */
        UrlHeader h;
        const uint8_t *loc;
        if (HV_DECODE_USED(UrlHeader, &h, buf, box->payload, n, &used) != 0 || n == used)
            return 0;
        loc = buf + box->payload + used;
        if (memchr(loc, 0, n - used) != loc + (n - used - 1))
            return 0;
        return hv_write_url(w->out, (const char *)loc) != 0 ? -1 : 1;
    }
    if (box->type == HV_BOX_RREQ)
        return write_rreq(w, box);
    return 0;
}

static int rewrite_boxes(rewrite *w, hv_boxes *it, int depth);

/* The children of a superbox, or of a dtbl after its NDR, from `from`. */
static int rewrite_children(rewrite *w, const hv_box *box, size_t from, int depth) {
    hv_boxes children;
    hv_box parent = *box;
    if (depth + 1 > HV_BOX_DEPTH_MAX) {       /* depth 0 is the top level */
        w->r->error = "superboxes nested too deep";
        w->r->at = box->start;
        return -1;
    }
    parent.payload = from;
    hv_boxes_children(&children, w->buf, &parent);
    return rewrite_boxes(w, &children, depth + 1);
}

/* A cmap in the writer's form: whole entries, each within its type.
 * Nonzero if so. */
static int cmap_entries(rewrite *w, const hv_box *box) {
    size_t n = box->end - box->payload, p, used;
    CmapEntry entry;
    if (n == 0 || n % HV_FIXED(CmapEntry) != 0)
        return 0;
    for (p = box->payload; p < box->end; p += HV_FIXED(CmapEntry))
        if (HV_DECODE_USED(CmapEntry, &entry, w->buf, p, HV_FIXED(CmapEntry), &used) != 0)
            return 0;
    return 1;
}

/* A dtbl in the writer's form: NDR, then boxes that frame. The length
 * NDR takes, or 0. */
static size_t dtbl_ndr(rewrite *w, const hv_box *box, DataReferenceCount *ndr) {
    hv_boxes children;
    hv_box parent = *box, child;
    const char *error;
    size_t used, at;
    int status;
    if (HV_DECODE_USED(DataReferenceCount, ndr, w->buf, box->payload, box->end - box->payload,
                       &used) != 0)
        return 0;
    parent.payload += used;
    hv_boxes_children(&children, w->buf, &parent);
    while ((status = hv_boxes_next(&children, &child, &error, &at)) == 1)
        ;
    return status == 0 ? used : 0;
}

static int rewrite_boxes(rewrite *w, hv_boxes *it, int depth) {
    hv_box box;
    int status;

    while ((status = hv_boxes_next(it, &box, &w->r->error, &w->r->at)) == 1) {
        size_t start = 0, ndr_size;
        DataReferenceCount ndr;
        if (box.to_end)
            differs(w, "LBox = 0 is written as a length");
        if ((status = write_box_function(w, &box)) != 0) {
            if (status < 0)
                return write_failed(w, box.start);
            continue;
        }
        if (hv_begin_box(w->out, box.type, box.payload - box.start == HV_BOX_HEADER_XL,
                         &start) != 0)
            return write_failed(w, box.start);
        if (box.type == HV_BOX_JP2C) {
            if (rewrite_codestream(w, box.payload, box.end) != 0)
                return -1;
        } else if (hv_is_superbox(box.type)) {
            if (rewrite_children(w, &box, box.payload, depth) != 0)
                return -1;
        } else if (box.type == HV_BOX_CMAP && cmap_entries(w, &box)) {
            size_t p, used;
            CmapEntry entry;
            for (p = box.payload; p < box.end; p += HV_FIXED(CmapEntry))
                if (HV_DECODE_USED(CmapEntry, &entry, w->buf, p, HV_FIXED(CmapEntry), &used) != 0 ||
                    hv_write_cmap_entry(w->out, &entry) != 0)
                    return write_failed(w, box.start);
        } else if (box.type == HV_BOX_DTBL && (ndr_size = dtbl_ndr(w, &box, &ndr)) != 0) {
            if (hv_write_ndr(w->out, (uint16_t)ndr) != 0)
                return write_failed(w, box.start);
            if (rewrite_children(w, &box, box.payload + ndr_size, depth) != 0)
                return -1;
        } else if (hv_write_bytes(w->out, w->buf + box.payload, box.end - box.payload) != 0) {
            return write_failed(w, box.start);
        }
        if (hv_end_box(w->out, start) != 0)
            return write_failed(w, box.start);
    }
    return status < 0 ? -1 : 0;
}

int hv_rewrite(const uint8_t *buf, size_t size, int raw, hv_read_mode mode, hv_out *out,
               hv_rewrite_result *r) {
    rewrite w;
    int status;
    size_t out_start = out->size;

    memset(r, 0, sizeof *r);
    if (out->error != NULL) {
        r->error = out->error;
        return -1;
    }
    if (mode != HV_READ_VALIDATE && mode != HV_READ_PADDED && mode != HV_READ_JPIP) {
        r->error = "rewrite requires validation mode";
        return -1;
    }
    memset(&w, 0, sizeof w);
    w.buf = buf;
    w.mode = mode;
    w.out = out;
    w.r = r;
    if (raw) {
        status = rewrite_codestream(&w, 0, size);
    } else {
        hv_boxes it;
        hv_boxes_file(&it, buf, size);
        status = rewrite_boxes(&w, &it, 0);
    }
    free(w.lengths);
    if (status != 0) hv_out_rewind(out, out_start);
    return status;
}
