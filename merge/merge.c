/* merge.c: see merge.h. A port of hvJP2K's jpx_merge, box for box. The
 * boxes it writes field by field (ftyp, the generated cmap, rreq, flst,
 * url, dtbl) go through hv_writer, whose encoders are generated from the
 * model, and every input and link is checked with the reader's rules. */
#define _XOPEN_SOURCE 700

#include "jpeg2000/hv_served.h"
#include "merge.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_codes.h"
#include "jpeg2000/hv_decode.h"
#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_reader.h"
#include "jpeg2000/hv_writer.h"
#include "jp2-boxes.h"
#include "transcode/transcode.h"

/* The Color Specification boxes read from one jp2h. */
#define MAX_COLR 16

/* The entities of an nlst entry (T.801 M.11.12, Table M.42): a codestream
 * and a compositing layer, numbered in 24 bits, which also bound the input
 * count (to 16,777,215). */
enum { NLST_CODESTREAM = 1, NLST_LAYER = 2, NLST_INDEX_MAX = 0xFFFFFF };

/* The signature box's payload (T.800 I.5.1). */
static const uint8_t jp_signature[] = HV_SIGNATURE_BYTES;

/* One input: its boxes, type 0 where absent, and what the output needs of
 * them; in.buf only while the input is open. */
typedef struct {
    hv_merge_input in;
    hv_box jp2h, jp2c, xml;
    hv_box ihdr, bpcc, pclr, cmap, cdef, res;
    hv_box colr[MAX_COLR];
    int ncolr;
    hv_box *ipr;                   /* spans at the top level and in jp2h */
    size_t nipr, ipr_capacity;
    unsigned rsiz, nc, ipr_flag;
    int opacity, premultiplied;     /* cdef Typ 1, Typ 2 */
} source;

typedef struct {
    FILE *file;
    hv_out out;             /* boxes not yet written to file */
    uint64_t written;
    char *error;
    size_t error_size;
} writer;

HV_DEFINE_DECODE(ColrHeader)

/* ------------------------------------------------------------------------
 * Inputs
 * ------------------------------------------------------------------------ */

/* The box tree rules of a JPX file for `box` (none: type 0) of s's input,
 * copied as a child of `parent` (hv_rule_box_placed); 0, or -1 with the
 * error naming the input, the rule and `where`. On success, *ipr counts
 * IPR in this copied tree. */
static int placed(const source *s, const hv_box *box, uint32_t parent, const char *where,
                  int *ipr, char *error, size_t size) {
    const char *rule;
    size_t at;
    *ipr = 0;
    if (box->type == 0 || (rule = hv_rule_box_placed(s->in.buf, box, parent, ipr, &at)) == NULL)
        return 0;
    return hv_fail(error, size, "%s: %s at %zu, in %s", s->in.path, rule, at, where);
}

/* Where s records the first jp2h child of this type, or NULL. */
static hv_box *header_box(source *s, uint32_t type) {
    switch (type) {
    case HV_BOX_IHDR: return &s->ihdr;
    case HV_BOX_BPCC: return &s->bpcc;
    case HV_BOX_PCLR: return &s->pclr;
    case HV_BOX_CMAP: return &s->cmap;
    case HV_BOX_CDEF: return &s->cdef;
    case HV_BOX_RES: return &s->res;
    default: return NULL;
    }
}

static int record_ipr(source *s, const hv_box *box, char *error, size_t size) {
    if (s->nipr == s->ipr_capacity) {
        size_t capacity;
        hv_box *spans;
        if (s->ipr_capacity > SIZE_MAX / 2 / sizeof *spans)
            return hv_fail(error, size, "%s: out of memory", s->in.path);
        capacity = s->ipr_capacity ? 2 * s->ipr_capacity : 8;
        spans = realloc(s->ipr, capacity * sizeof *spans);
        if (spans == NULL)
            return hv_fail(error, size, "%s: out of memory", s->in.path);
        s->ipr = spans;
        s->ipr_capacity = capacity;
    }
    s->ipr[s->nipr++] = *box;
    return 0;
}

static int read_source(const hv_merge_input *in, source *s, int served, hv_output_profile profile, char *error, size_t size) {
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    int status, headers = 0;
    hv_header header;
    SizFixed siz;

    memset(s, 0, sizeof *s);
    s->in = *in;
    if (in->size > INT_MAX)
        return hv_fail(error, size, "%s: file.size-limit at 0", in->path);
    /* Transcoding requires the served container, but not input PLT. */
    message = served || profile == HV_OUTPUT_JPIP ? hv_served_jp2(in->buf, in->size, &s->jp2c, &at)
                               : hv_read_jp2(in->buf, in->size, &s->jp2c, &at);
    if (message != NULL ||
        (profile == HV_OUTPUT_JPIP && (message = hv_codestream_check(in->buf, s->jp2c.payload, s->jp2c.end,
                                                  HV_READ_JPIP, &at)) != NULL))
        return hv_fail(error, size, "%s: %s at %zu", in->path, message, at);
    hv_boxes_file(&it, in->buf, in->size);
    while (hv_boxes_next(&it, &box, &message, &at) == 1) {
        if (box.type == HV_BOX_JP2H) {
            if (headers++ == 0) s->jp2h = box;
        } else if (box.type == HV_BOX_XML && s->xml.type == 0)
            s->xml = box;
        else if (box.type == HV_BOX_JP2I && record_ipr(s, &box, error, size) != 0)
            return -1;
    }
    if (headers != 1)
        return hv_fail(error, size, "%s: jp2.one-jp2h at %zu", in->path, in->size);
    if (s->jp2h.start > s->jp2c.start)
        return hv_fail(error, size, "%s: jp2h.position at %zu", in->path, in->size);
    message = hv_read_jp2h_boxes(in->buf, &s->jp2h, &s->jp2c, &header, &at);
    if (message != NULL)
        return hv_fail(error, size, "%s: %s at %zu", in->path, message, at);
    /* Selected header: ihdr first and NC = Csiz (at most 16,384),
     * at most one bpcc, pclr, cmap, cdef and res. */
    hv_boxes_children(&it, in->buf, &s->jp2h);
    while ((status = hv_boxes_next(&it, &box, &message, &at)) == 1) {
        hv_box *first = header_box(s, box.type);
        if (first != NULL && first->type == 0)
            *first = box;
        if (box.type == HV_BOX_JP2I && record_ipr(s, &box, error, size) != 0)
            return -1;
        if (box.type == HV_BOX_COLR) {
            if (s->ncolr == MAX_COLR)
                return hv_fail(error, size, "%s: more than %d Color Specification boxes",
                               in->path, MAX_COLR);
            s->colr[s->ncolr++] = box;
        }
    }
    if (status < 0)
        return hv_fail(error, size, "%s: %s at %zu", in->path, message, at);
    message = hv_read_siz(in->buf + s->jp2c.payload, s->jp2c.end - s->jp2c.payload,
                          &siz, NULL, 0, &at);
    if (message != NULL)
        return hv_fail(error, size, "%s: %s at %zu", in->path, message, s->jp2c.payload + at);
    /* Reader Requirements below describe only these declarations. */
    if (siz.rsiz > 2)
        return hv_fail(error, size, "%s: unsupported Rsiz %u for merge Reader Requirements "
                       "(supported: 0, 1, 2)", in->path, (unsigned)siz.rsiz);
    s->rsiz = (unsigned)siz.rsiz;
    s->nc = (unsigned)header.image.nc;
    s->ipr_flag = (unsigned)header.image.ipr;
    s->opacity = header.opacity;
    s->premultiplied = header.premultiplied;
    return 0;
}

/* Byte i of a Color Specification box's payload as the JPX file carries
 * it. A JP2 reader ignores PREC and APPROX (T.800 I.5.3.3); the output
 * writes PREC 0 and APPROX 1 (T.801 M.11.7.2, as hvJP2K's jpx_colr). */
enum { COLR_APPROX = 2 };

static uint8_t colr_byte(const uint8_t *payload, size_t i) {
    return i == 1 ? 0 : i == COLR_APPROX ? 1 : payload[i];
}

/* An input's Color Specification boxes as the JPX file holds them,
 * PREC 0 and APPROX 1 (colr_byte): the first input's in the jp2h
 * (parent HV_BOX_JP2H), a later one's, where they differ from the first's,
 * in a cgrp (HV_BOX_CGRP). hv_rule_colr for a JPX jp2h or color group,
 * which a JP2 file's jp2h need not keep (at most one with METH 1 and one
 * with METH 2, T.801 M.11.7.1; APPROX 1 to 4, M.11.7.2). A cgrp is never
 * empty (cgrp.empty): every input has a colr (jp2h.colr). */
static int check_jpx_colrs(const source *s, uint32_t parent, char *error, size_t size) {
    hv_header h;
    const char *rule = NULL;
    const char *where = parent == HV_BOX_JP2H ? "the JPX file's jp2h" : "its jplh's cgrp";
    size_t used = 0;
    int k;
    hv_header_init(&h, parent, 1);
    for (k = 0; k < s->ncolr && rule == NULL; k++) {
        const hv_box *colr = &s->colr[k];
        ColrHeader header;
        if (HV_DECODE_USED(ColrHeader, &header, s->in.buf, colr->payload,
                           colr->end - colr->payload, &used) != 0)
            return hv_fail(error, size, "%s: colr at %zu cannot be decoded", s->in.path,
                           colr->start);
        header.approx = colr_byte(s->in.buf + colr->payload, COLR_APPROX);
        rule = hv_rule_colr(&h, &header, s->in.buf + colr->payload + used,
                            colr->end - colr->payload - used);
    }
    return rule == NULL ? 0
                        : hv_fail(error, size, "%s: %s at %zu, in %s", s->in.path, rule,
                                  s->colr[k - 1].start, where);
}

/* The same contents, or both absent. */
static int same_box(const source *a, const hv_box *x, const source *b, const hv_box *y) {
    size_t n = x->end - x->payload;
    if (x->type == 0 || y->type == 0)
        return x->type == y->type;
    return x->type == y->type && n == y->end - y->payload &&
           memcmp(a->in.buf + x->payload, b->in.buf + y->payload, n) == 0;
}

static int same_colrs(const source *a, const source *b) {
    int k;
    size_t i, n;
    if (a->ncolr != b->ncolr)
        return 0;
    for (k = 0; k < a->ncolr; k++) {
        const uint8_t *x = a->in.buf + a->colr[k].payload, *y = b->in.buf + b->colr[k].payload;
        n = a->colr[k].end - a->colr[k].payload;
        if (n != b->colr[k].end - b->colr[k].payload)
            return 0;
        for (i = 0; i < n; i++)
            if (colr_byte(x, i) != colr_byte(y, i))
                return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------------ */

/* n bytes to the file, which stays within INT_MAX bytes. */
static int write_file(writer *w, const uint8_t *bytes, size_t n) {
    if (n > INT_MAX - w->written)
        return hv_fail(w->error, w->error_size,
                       "file.size-limit: output larger than INT_MAX bytes");
    if (n != 0 && fwrite(bytes, 1, n, w->file) != n)
        return hv_fail(w->error, w->error_size, "cannot write the JPX file");
    w->written += n;
    return 0;
}

/* The boxes in w->out, written to the file. */
static int flush(writer *w) {
    if (w->out.error != NULL)
        return hv_fail(w->error, w->error_size, "%s", w->out.error);
    if (write_file(w, w->out.data, w->out.size) != 0)
        return -1;
    hv_out_rewind(&w->out, 0);
    return 0;
}

/* Bytes of an input, written straight to the file after w->out. */
static int write_through(writer *w, const uint8_t *bytes, size_t n) {
    return flush(w) != 0 ? -1 : write_file(w, bytes, n);
}

/* The signature and File Type boxes (T.800 I.5.1, T.801 M.8), as
 * hvJP2K writes them: brand jpx, MinV 1, compatible with jpx, and when the
 * codestreams are embedded with jp2 and jpxb too. */
static int write_start(hv_out *out, int links) {
    static const Brand embedded[] = {HV_BRAND_JPX, HV_BRAND_JP2, HV_BRAND_JPXB};
    static const Brand linked[] = {HV_BRAND_JPX};
    const Brand *compatible = links ? linked : embedded;
    size_t ncompatible = links ? sizeof linked / sizeof *linked
                               : sizeof embedded / sizeof *embedded;
    FtypHeader header = {HV_BRAND_JPX, 1};

    return hv_write_box_header(out, HV_BOX_JP, sizeof jp_signature) != 0 ||
           hv_write_bytes(out, jp_signature, sizeof jp_signature) != 0
               ? -1
               : hv_write_ftyp(out, &header, compatible, ncompatible);
}

/* Where the header boxes go, box by box: counted, for the length of a
 * superbox, which is written before its contents; or written, box headers
 * and the few bytes the merge makes itself through w->out, and every
 * payload copied from an input straight to the file (write_through), so
 * that memory does not grow with the boxes. */
typedef struct {
    writer *w;
    int counting;
    uint64_t size;              /* counted */
} sink;

static int emit_header(sink *k, uint32_t type, uint64_t payload) {
    if (k->counting) {
        k->size += hv_box_header_size(payload);
        return 0;
    }
    return hv_write_box_header(&k->w->out, type, payload);
}

/* Bytes the merge makes itself, a few at a time: buffered. */
static int emit_small(sink *k, const uint8_t *bytes, size_t n) {
    if (k->counting) {
        k->size += n;
        return 0;
    }
    return hv_write_bytes(&k->w->out, bytes, n);
}

/* Bytes of an input: to the file. */
static int emit_input(sink *k, const uint8_t *bytes, size_t n) {
    if (k->counting) {
        k->size += n;
        return 0;
    }
    return write_through(k->w, bytes, n);
}

/* A box of an input with a fresh header, as glymur writes it. */
static int emit_box(sink *k, const source *s, const hv_box *box) {
    return emit_header(k, box->type, box->end - box->payload) != 0 ? -1
         : emit_input(k, s->in.buf + box->payload, box->end - box->payload);
}

/* A Color Specification box as the JPX file holds it, its APPROX set
 * (colr_byte): METH, PREC and APPROX, which hv_read_jp2h has checked it
 * has, then the rest as read. */
static int emit_colr(sink *k, const source *s, const hv_box *box) {
    const uint8_t *p = s->in.buf + box->payload;
    uint8_t head[COLR_APPROX + 1];
    size_t i;
    for (i = 0; i < sizeof head; i++)
        head[i] = colr_byte(p, i);
    return emit_header(k, HV_BOX_COLR, box->end - box->payload) != 0 ||
           emit_small(k, head, sizeof head) != 0
               ? -1
               : emit_input(k, p + sizeof head, box->end - box->payload - sizeof head);
}

/* The input's IPR boxes (T.800 I.6), at the top level and in its jp2h,
 * which its ihdr's IPR 1 announces (ihdr.ipr), into its jpch: the JPX
 * file's IPR 1, in that jpch's ihdr or in the jp2h's it takes, needs one
 * in the file (jpx.ipr). */
static int emit_ipr(sink *k, const source *s) {
    size_t i;
    for (i = 0; i < s->nipr; i++)
        if (emit_box(k, s, &s->ipr[i]) != 0)
            return -1;
    return 0;
}

/* What a header box holds: an input, the first, and whether their JP2
 * Header boxes are the same. */
typedef struct {
    const source *s, *first;
    int same;
} headers;

/* A superbox whose contents `contents` emits: counted first, for its
 * length. */
static int emit_superbox(sink *k, uint32_t type, int (*contents)(sink *, const headers *),
                         const headers *h) {
    sink count = {k->w, 1, 0};
    if (contents(&count, h) != 0)
        return -1;
    if (k->counting) {
        k->size += hv_box_header_size(count.size) + count.size;
        return 0;
    }
    return hv_write_box_header(&k->w->out, type, count.size) != 0 ? -1 : contents(k, h);
}

/* The JP2 Header box of the first input, with the APPROX fixed. */
static int jp2h_contents(sink *k, const headers *h) {
    const source *s = h->s;
    hv_boxes it;
    hv_box box;
    const char *message;
    size_t at;
    hv_boxes_children(&it, s->in.buf, &s->jp2h);
    while (hv_boxes_next(&it, &box, &message, &at) == 1)
        if ((box.type == HV_BOX_COLR ? emit_colr(k, s, &box) : emit_box(k, s, &box)) != 0)
            return -1;
    return 0;
}

/* The Component Mapping box for an input without the palette of the
 * first, in out: one entry per component, NC of them (the checked Csiz,
 * at most 16,384), each used directly (CMP = i, MTYP = 0, PCOL = 0). 1,
 * or 0 when the first input's cmap has these entries and none is needed;
 * -1 with out->error set. */
static int generated_cmap(hv_out *out, const source *s, const source *first) {
    const hv_box *c0 = &first->cmap;
    unsigned i;
    for (i = 0; i < s->nc; i++) {
        CmapEntry entry = {i, 0, 0};
        if (hv_write_cmap_entry(out, &entry) != 0)
            return -1;
    }
    return !(c0->type != 0 && c0->end - c0->payload == out->size &&
             memcmp(first->in.buf + c0->payload, out->data, out->size) == 0);
}

static int emit_generated_cmap(sink *k, const source *s, const source *first) {
    hv_out entries;
    int status;
    hv_out_init(&entries);
    status = generated_cmap(&entries, s, first);
    if (status < 0)
        hv_fail(k->w->error, k->w->error_size, "%s", entries.error);
    else if (status > 0)
        status = emit_header(k, HV_BOX_CMAP, entries.size) != 0 ||
                 emit_small(k, entries.data, entries.size) != 0 ? -1 : 0;
    hv_out_free(&entries);
    return status < 0 ? -1 : 0;
}

static int emit_differing(sink *k, const source *s, const hv_box *box,
                          const source *first, const hv_box *original) {
    return box->type != 0 && !same_box(s, box, first, original) ? emit_box(k, s, box) : 0;
}

/* The Codestream Header box: where the input's JP2 Header box is the
 * first input's, only its IPR boxes; otherwise its Image Header and what
 * differs from the first (hvJP2K's write_jpch_jplh), then the IPR boxes. */
static int jpch_contents(sink *k, const headers *h) {
    const source *s = h->s, *first = h->first;
    if (!h->same) {
        if (emit_box(k, s, &s->ihdr) != 0)
            return -1;
        if (emit_differing(k, s, &s->bpcc, first, &first->bpcc) != 0)
            return -1;
        if (emit_differing(k, s, &s->pclr, first, &first->pclr) != 0)
            return -1;
        if (s->pclr.type == 0 && first->pclr.type != 0) {
            if (emit_generated_cmap(k, s, first) != 0)
                return -1;
        } else if (emit_differing(k, s, &s->cmap, first, &first->cmap) != 0) {
            return -1;
        }
    }
    return emit_ipr(k, s);
}

static int cgrp_contents(sink *k, const headers *h) {
    int i;
    for (i = 0; i < h->s->ncolr; i++)
        if (emit_colr(k, h->s, &h->s->colr[i]) != 0)
            return -1;
    return 0;
}

/* The Compositing Layer Header box: empty where the input's JP2 Header
 * box is the first input's; otherwise the colors (in a cgrp), cdef and
 * res, where they differ from the first. */
static int jplh_contents(sink *k, const headers *h) {
    const source *s = h->s, *first = h->first;
    if (h->same)
        return 0;
    if (!same_colrs(s, first) && emit_superbox(k, HV_BOX_CGRP, cgrp_contents, h) != 0)
        return -1;
    if (emit_differing(k, s, &s->cdef, first, &first->cdef) != 0)
        return -1;
    if (emit_differing(k, s, &s->res, first, &first->res) != 0)
        return -1;
    return 0;
}

static int write_jp2h(writer *w, const source *first) {
    sink k = {w, 0, 0};
    headers h = {first, first, 1};
    return emit_superbox(&k, HV_BOX_JP2H, jp2h_contents, &h);
}

/* An input's Codestream and Compositing Layer Header boxes. */
static int write_headers(writer *w, const source *s, const source *first) {
    sink k = {w, 0, 0};
    const hv_box *j = &s->jp2h, *j0 = &first->jp2h;
    headers h = {s, first, 0};
    h.same = j->end - j->start == j0->end - j0->start &&
             memcmp(s->in.buf + j->start, first->in.buf + j0->start, j->end - j->start) == 0;
    return emit_superbox(&k, HV_BOX_JPCH, jpch_contents, &h) != 0 ? -1
         : emit_superbox(&k, HV_BOX_JPLH, jplh_contents, &h);
}

/* Reader Requirements (T.801 M.11.1, Table M.14), as hvJP2K's
 * reader_requirements: standard features 1 (no extensions), 2 (more than
 * one codestream), 4 and 5 (the codestreams' Rsiz: Profile 1, a T.800
 * codestream without profile), 9 and 10 (opacity channels in cdef), 15
 * (linked codestreams), each needed for both the Fully Understand and the
 * Display expressions: feature i sets mask bit i, FUAM and DCM all of
 * them. Rsiz 1 (Profile 0) needs no feature: 3, its own, is deprecated.
 * read_source admits only Rsiz 0, 1 or 2 for these Reader Requirements.
 * Part 2 declarations in JP2 also fail header consistency (jp2.rsiz).
 * No extension feature is emitted, so feature 1 always holds.
 * At most 7 features, so every mask is one byte (ML 1), and no vendor
 * features. 0, or -1 with
 * out->error set. */
static int write_rreq(hv_out *out, const int has[16]) {
    RreqStandardFeature standard[8];      /* 1, 2, 4, 5, 9, 10 and 15 at most */
    RreqHeader header;
    int i;
    size_t k = 0;

    memset(&header, 0, sizeof header);
    header.fuam.nCount = header.dcm.nCount = 1;
    for (i = 0; i < 16; i++) {
        if (!has[i])
            continue;
        standard[k] = (RreqStandardFeature){
            .sf = (FeatureCode)i, .sm = {.nCount = 1, .arr = {(byte)(0x80 >> k)}}
        };
        header.fuam.arr[0] |= (byte)(0x80 >> k);
        k++;
    }
    header.dcm.arr[0] = header.fuam.arr[0];
    header.nsf = (FeatureCount)k;
    return hv_write_rreq(out, &header, standard, k, NULL, 0);
}

/* The XML box of input i as read, associated with codestream i and
 * compositing layer i by an asoc holding an nlst of the two. An XML box
 * running to the end of its file gets an explicit length. The asoc's
 * length is known before its XML box, which may be copied straight to the
 * file. */
static int write_xml(writer *w, const source *s, size_t i, int transcode) {
    const hv_box *xml = &s->xml;
    size_t payload = xml->end - xml->payload,
           box = xml->to_end ? hv_box_header_size(payload) + payload : xml->end - xml->start;
    const uint8_t *nul = transcode ? memchr(s->in.buf + xml->payload, 0, payload) : NULL;
    if (nul != NULL) {
        payload = (size_t)(nul - (s->in.buf + xml->payload));
        box = hv_box_header_size(payload) + payload;
    }
    const NlstEntry nlst[2] = {{NLST_CODESTREAM, i}, {NLST_LAYER, i}};
    const size_t nlst_payload = sizeof nlst / sizeof *nlst * HV_FIXED(NlstEntry),
                 nlst_box = hv_box_header_size(nlst_payload) + nlst_payload;
    if (hv_write_box_header(&w->out, HV_BOX_ASOC, nlst_box + (uint64_t)box) != 0 ||
        hv_write_nlst(&w->out, nlst, sizeof nlst / sizeof *nlst) != 0)
        return -1;
    return nul != NULL || xml->to_end ? hv_write_box_header(&w->out, HV_BOX_XML, payload) != 0 ? -1
                       : write_through(w, s->in.buf + xml->payload, payload)
                       : write_through(w, s->in.buf + xml->start, box);
}

/* The URL of a linked file: HV_FILE_SCHEME and its absolute path,
 * resolved, with the bytes Python's urllib.parse.quote_from_bytes escapes
 * (as pathlib's as_uri, which hvJP2K uses), as %XX, which hv_url_path
 * decodes. NULL, with a message in error. */
static char *link_url(const char *path, char *error, size_t size) {
    static const char hex[] = "0123456789ABCDEF", scheme[] = HV_FILE_SCHEME;
    char *real = realpath(path, NULL), *url, *p;
    const char *q;
    if (real == NULL) {
        hv_fail(error, size, "%s: cannot resolve the path", path);
        return NULL;
    }
    if ((url = malloc(sizeof scheme + 3 * strlen(real))) == NULL) {
        free(real);
        hv_fail(error, size, "out of memory");
        return NULL;
    }
    memcpy(url, scheme, sizeof scheme - 1);
    for (p = url + sizeof scheme - 1, q = real; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-' || c == '~' || c == '/') {
            *p++ = (char)c;
        } else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        }
    }
    *p = 0;
    free(real);
    return url;
}

/* Patches bytes already counted and written, then restores the append position. */
static int patch(writer *w, long position, const uint8_t *bytes, size_t size) {
    long end, landed;
    if (flush(w) != 0)
        return -1;
    errno = 0;
    if (fflush(w->file) != 0 || (end = ftell(w->file)) < 0 ||
        fseek(w->file, position, SEEK_SET) != 0 ||
        fwrite(bytes, 1, size, w->file) != size || fflush(w->file) != 0 ||
        (landed = ftell(w->file)) < 0)
        goto failed;
    if (landed != position + (long)size) {
        errno = EIO;
        goto failed;
    }
    if (fseek(w->file, end, SEEK_SET) != 0)
        goto failed;
    return 0;
failed:
    /* A custom stream may fail a write without setting errno. */
    if (errno == 0)
        errno = EIO;
    return hv_fail(w->error, w->error_size, "cannot patch the JPX file: %s", strerror(errno));
}

/* Resolve and escape one URL at a time; only the input paths are retained. */
static int write_dtbl(writer *w, const char *const *paths, size_t n) {
    hv_out header;
    long position;
    uint64_t start;
    size_t i;
    int status;
    if (flush(w) != 0)
        return -1;
    if ((position = ftell(w->file)) < 0)
        return hv_fail(w->error, w->error_size, "cannot seek in the JPX file: %s", strerror(errno));
    start = w->written;
    if (hv_write_box_header(&w->out, HV_BOX_DTBL, 0) != 0 ||
        hv_write_ndr(&w->out, (uint16_t)n) != 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char *rule;
        char *url = link_url(paths[i], w->error, w->error_size);
        if (url == NULL)
            return -1;
        rule = hv_rule_url(0, 0, (const uint8_t *)url, strlen(url) + 1);
        if (rule != NULL) {
            free(url);
            return hv_fail(w->error, w->error_size, "%s: %s", paths[i], rule);
        }
        status = hv_write_url(&w->out, url);
        free(url);
        if (status != 0 || flush(w) != 0)
            return -1;
    }
    hv_out_init(&header);
    status = hv_write_box_header(&header, HV_BOX_DTBL, w->written - start - HV_BOX_HEADER);
    if (status == 0)
        status = patch(w, position, header.data, header.size);
    else
        hv_fail(w->error, w->error_size, "%s", header.error);
    hv_out_free(&header);
    return status;
}

static int open_input(const hv_merge_inputs *inputs, size_t i, source *s, int served,
                      hv_output_profile profile, char *error, size_t error_size) {
    hv_merge_input in = {0};
    if (inputs->open(inputs->context, i, &in, error, error_size) != 0)
        return -1;
    if (read_source(&in, s, served, profile, error, error_size) != 0) {
        inputs->close(inputs->context, i, &in);
        s->in.buf = NULL;
        return -1;
    }
    return 0;
}

static void close_input(const hv_merge_inputs *inputs, size_t i, source *s) {
    if (s->in.buf != NULL)
        inputs->close(inputs->context, i, &s->in);
    free(s->ipr);
    memset(s, 0, sizeof *s);
}

int hv_merge_files(const hv_merge_inputs *inputs, size_t n, int links, hv_output_profile profile,
                   int ppx, int ppy, FILE *file, char *error, size_t error_size) {
    /* Seven features need at most 36 bytes, leaving room for a free box header. */
    enum { RREQ_SPACE = 44 };
    static const uint8_t zeros[RREQ_SPACE] = {0};
    writer w = {0};
    source first = {0}, current = {0};
    const char **paths = NULL;
    hv_out transcoded, requirements;
    size_t i = 0, start;
    long rreq_position;
    int status = -1, transcode = ppx != 0 || ppy != 0;
    int has[16] = {0};

    if (profile != HV_OUTPUT_JPEG2000 && profile != HV_OUTPUT_JPIP)
        return hv_fail(error, error_size, "invalid output profile");
    if (transcode && profile == HV_OUTPUT_JPIP)
        return hv_fail(error, error_size, "transcoding cannot be combined with validation (HV_OUTPUT_JPIP)");
    if (transcode && (links || ppx < 1 || ppx > 15 || ppy < 1 || ppy > 15))
        return hv_fail(error, error_size, "transcoding requires embedded output and precinct exponents 1 to 15");
    if (n == 0)
        return hv_fail(error, error_size, "no JP2 input files");
    if (links && n > UINT16_MAX)
        return hv_fail(error, error_size, "a linked JPX file holds at most 65,535 links");
    if (n > NLST_INDEX_MAX)
        return hv_fail(error, error_size, "a JPX file holds at most 16,777,215 input files");
    if (links && (paths = calloc(n, sizeof *paths)) == NULL)
        return hv_fail(error, error_size, "out of memory");
    w.file = file;
    w.error = error;
    w.error_size = error_size;
    error[0] = 0;
    hv_out_init(&w.out);
    hv_out_init(&transcoded);
    hv_out_init(&requirements);
    has[1] = 1;
    has[2] = n > 1;
    has[15] = links != 0;
    if (write_start(&w.out, links) != 0 || flush(&w) != 0)
        goto done;
    if ((rreq_position = ftell(file)) < 0) {
        hv_fail(error, error_size, "cannot seek in the JPX file: %s", strerror(errno));
        goto done;
    }
    if (write_file(&w, zeros, sizeof zeros) != 0)
        goto done;
    for (i = 0; i < n; i++) {
        source *s = i == 0 ? &first : &current;
        const hv_box *cs = &s->jp2c;
        int copied_ipr;
        const char *missing;
        if (open_input(inputs, i, s, links || transcode,
                       transcode ? HV_OUTPUT_JPEG2000 : profile, error, error_size) != 0)
            goto done;
        if (placed(s, i == 0 ? &first.jp2h : &s->res, i == 0 ? 0 : HV_BOX_JPLH,
                   i == 0 ? "the JPX file's jp2h" : "its jplh", &copied_ipr, error, error_size) != 0)
            goto done;
        if ((s->ipr_flag != 0) != (s->nipr > 0 || copied_ipr > 0)) {
            hv_fail(error, error_size, "%s: ihdr.ipr at %zu", s->in.path, s->jp2h.start);
            goto done;
        }
        if (i == 0 ? check_jpx_colrs(&first, HV_BOX_JP2H, error, error_size) != 0
                   : !same_colrs(s, &first) && check_jpx_colrs(s, HV_BOX_CGRP, error, error_size) != 0)
            goto done;
        missing = first.cdef.type != 0 && s->cdef.type == 0 ? "cdef"
                : first.res.type != 0 && s->res.type == 0 ? "res" : NULL;
        if (missing != NULL) {
            hv_fail(error, error_size, "%s: no %s box, but the first input has one, which it "
                    "would inherit", s->in.path, missing);
            goto done;
        }
        has[9] |= s->opacity;
        has[10] |= s->premultiplied;
        has[4] |= s->rsiz == 2;
        has[5] |= s->rsiz == 0;
        if (i == 0 && write_jp2h(&w, &first) != 0)
            goto done;
        if (write_headers(&w, s, &first) != 0)
            goto done;
        if (links) {
            paths[i] = s->in.path;
            if (hv_begin_box(&w.out, HV_BOX_FTBL, 0, &start) != 0 ||
                hv_write_flst(&w.out, cs->payload, (uint32_t)(cs->end - cs->payload),
                              (uint16_t)(i + 1)) != 0 || hv_end_box(&w.out, start) != 0)
                goto done;
        } else {
            const uint8_t *bytes = s->in.buf + cs->payload;
            size_t size = cs->end - cs->payload;
            if (transcode) {
                char detail[512];
                hv_out_rewind(&transcoded, 0);
                if (hv_transcode_codestream(s->in.buf, cs->payload, cs->end, ppx, ppy,
                                            HV_OUTPUT_JPIP, &transcoded, detail, sizeof detail) != 0) {
                    hv_fail(error, error_size, "%s: %s", s->in.path, detail);
                    goto done;
                }
                bytes = transcoded.data;
                size = transcoded.size;
            }
            if (hv_write_box_header(&w.out, HV_BOX_JP2C, size) != 0 ||
                write_through(&w, bytes, size) != 0)
                goto done;
        }
        if (s->xml.type != 0 && write_xml(&w, s, i, transcode) != 0)
            goto done;
        if (flush(&w) != 0)
            goto done;
        if (i > 0)
            close_input(inputs, i, &current);
    }
    if (links && write_dtbl(&w, paths, n) != 0)
        goto done;
    if (write_rreq(&requirements, has) != 0)
        goto done;
    if (requirements.size > RREQ_SPACE - HV_BOX_HEADER) {
        hv_fail(error, error_size, "Reader Requirements of %zu bytes exceed the %d bytes reserved",
                requirements.size, RREQ_SPACE - HV_BOX_HEADER);
        goto done;
    }
    size_t pad = RREQ_SPACE - requirements.size;
    if (hv_write_box_header(&requirements, HV_BOX_FREE, pad - HV_BOX_HEADER) != 0 ||
        hv_write_bytes(&requirements, zeros, pad - HV_BOX_HEADER) != 0 ||
        patch(&w, rreq_position, requirements.data, requirements.size) != 0)
        goto done;
    status = 0;

done:
    if (status != 0 && error[0] == 0)
        hv_fail(error, error_size, "%s", w.out.error ? w.out.error
                                  : requirements.error ? requirements.error : "out of memory");
    close_input(inputs, 0, &first);
    close_input(inputs, i, &current);
    free(paths);
    hv_out_free(&transcoded);
    hv_out_free(&requirements);
    hv_out_free(&w.out);
    return status;
}

/* Inputs already in memory. */
static int buffer_open(void *context, size_t i, hv_merge_input *in, char *error,
                       size_t error_size) {
    (void)error;
    (void)error_size;
    *in = ((const hv_merge_input *)context)[i];
    return 0;
}

static void buffer_close(void *context, size_t i, hv_merge_input *in) {
    (void)context;
    (void)i;
    (void)in;
}

int hv_merge_buffers(const hv_merge_input *inputs, size_t n, int links, hv_output_profile profile, int ppx, int ppy,
                     FILE *file, char *error, size_t error_size) {
    hv_merge_inputs from = {buffer_open, buffer_close, NULL};
    from.context = (void *)inputs;
    return hv_merge_files(&from, n, links, profile, ppx, ppy, file, error, error_size);
}
