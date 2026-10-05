/* hv_reader.c: see hv_reader.h. */
#include "hv_reader.h"
#include "hv_presentation.h"
#include "hv_served.h"

#include "hv_decode.h"
#include "j2k-codestream.h"   /* MainMarkerCode-Profile, TileMarkerCode-Profile */
#include "jp2-boxes.h"        /* Fragment, the JP2 header boxes */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* A marker code (A.1.1). */
#define MARKER HV_FIXED(MarkerCode)

/* The generated decoders on a bounded window (hv_decode.h): the reader
 * takes every field's position from the model's layout. */
HV_DEFINE_DECODE(BoxHeader)
HV_DEFINE_DECODE(MarkerCode)
HV_DEFINE_DECODE(SegmentLength)
HV_DEFINE_DECODE(SotSegment)
HV_DEFINE_DECODE(SizFixed)
HV_DEFINE_DECODE(Component)
HV_DEFINE_DECODE(CodSegment)
HV_DEFINE_DECODE(QcdSegment)
HV_DEFINE_DECODE(Zplt)
HV_DEFINE_DECODE(Rcom)
HV_DEFINE_DECODE(DataReferenceCount)
HV_DEFINE_DECODE(UrlHeader)
HV_DEFINE_DECODE(FragmentCount)
HV_DEFINE_DECODE(Fragment)
HV_DEFINE_DECODE(Ihdr)
HV_DEFINE_DECODE(BitDepth)
HV_DEFINE_DECODE(ColrHeader)
HV_DEFINE_DECODE(PclrCounts)
HV_DEFINE_DECODE(CmapEntry)
HV_DEFINE_DECODE(CdefCount)
HV_DEFINE_DECODE(CdefEntry)
HV_DEFINE_DECODE(CrefType)
HV_DEFINE_DECODE(Resolution)
HV_DEFINE_DECODE(RreqHeader)
HV_DEFINE_DECODE(RreqStandardFeature)
HV_DEFINE_DECODE(FeatureCount)
HV_DEFINE_DECODE(RreqVendorFeature)

/* hv_codes.h's box header sizes are the model's. */
_Static_assert(HV_LARGEST(BoxHeader) == HV_BOX_HEADER_XL, "BoxHeader is LBox, TBox, XLBox");

/* And its fixed sizes (HV_FIXED). */
#define HV_FIXED_IS_MODEL(T) _Static_assert(HV_FIXED(T) == HV_LARGEST(T), "HV_FIXED(" #T ")")
HV_FIXED_IS_MODEL(MarkerCode);
HV_FIXED_IS_MODEL(SegmentLength);
HV_FIXED_IS_MODEL(SotSegment);
HV_FIXED_IS_MODEL(Component);
HV_FIXED_IS_MODEL(Brand);
HV_FIXED_IS_MODEL(FtypHeader);
HV_FIXED_IS_MODEL(BitDepth);
HV_FIXED_IS_MODEL(PclrCounts);
HV_FIXED_IS_MODEL(CmapEntry);
HV_FIXED_IS_MODEL(CdefEntry);
HV_FIXED_IS_MODEL(UrlHeader);
HV_FIXED_IS_MODEL(DataReferenceCount);
HV_FIXED_IS_MODEL(Fragment);
HV_FIXED_IS_MODEL(FeatureCode);
HV_FIXED_IS_MODEL(VendorId);
HV_FIXED_IS_MODEL(NlstEntry);
HV_FIXED_IS_MODEL(UuidCount);
HV_FIXED_IS_MODEL(UuidId);
HV_FIXED_IS_MODEL(CrefType);

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

/* ------------------------------------------------------------------------
 * Container access and serving checks: JP2 and JPX
 * ------------------------------------------------------------------------ */

/* The signature box, before any box is read: the whole box
 * is fixed (T.800 I.5.1), LBox 12 and TBox HV_BOX_JP, then these
 * contents. */
static const char *check_start(const uint8_t *buf, size_t size) {
    static const uint8_t contents[] = HV_SIGNATURE_BYTES;
    BoxHeader h;
    size_t used;
    if (HV_DECODE_USED(BoxHeader, &h, buf, 0, min_size(size, HV_BOX_HEADER_XL), &used) != 0 ||
        h.tbox != HV_BOX_JP || h.lbox != HV_BOX_HEADER + sizeof contents ||
        size - used < sizeof contents || memcmp(buf + used, contents, sizeof contents) != 0)
        return "file.signature";
    return NULL;
}

/* The second box: File Type (T.800 I.5.2, T.801 M.8), FtypHeader, then
 * at least one Brand, filling the box; its brand and compatibility list
 * for a JP2 or (jpx) JPX file (hv_rule_ftyp). */
static const char *check_ftyp(const uint8_t *buf, const hv_box *box, int jpx, int profile,
                              hv_ftyp *ftyp) {
    if (box->type != HV_BOX_FTYP)
        return "file.ftyp-second";
    if (hv_ftyp_decode(buf, box, ftyp) != 0)
        return "ftyp: not BR, MinV and one or more compatibility entries";
    return hv_rule_ftyp(ftyp, jpx, profile);
}

/* Signature and second-box File Type, with the remaining top-level boxes
 * available through the existing bounded iterator. No body/tree validation. */
static const char *open_file(const uint8_t *buf, size_t size, hv_boxes *it,
                             hv_ftyp *ftyp, size_t *at) {
    hv_box box;
    const char *error;
    int status, n;

    *at = 0;
    if ((error = check_start(buf, size)) != NULL)
        return error;
    hv_boxes_file(it, buf, size);
    for (n = 0; n < 2; n++)
        if ((status = hv_boxes_next(it, &box, &error, at)) != 1) {
            if (status < 0)
                return error;
            *at = size;
            return "file.two-boxes";
        }
    *at = box.start;
    if (box.type != HV_BOX_FTYP)
        return "file.ftyp-second";
    if (hv_ftyp_decode(buf, &box, ftyp) != 0)
        return "ftyp: not BR, MinV and one or more compatibility entries";
    return NULL;
}

const char *hv_container_open(const uint8_t *buf, size_t size, hv_boxes *boxes,
                              hv_ftyp *ftyp, size_t *at) {
    hv_boxes it;
    hv_ftyp type;
    size_t pos;
    const char *error = open_file(buf, size, &it, &type, &pos);
    if (error == NULL)
        error = hv_rule_ftyp(&type, hv_ftyp_is_jpx(&type), 0);
    if (error != NULL)
        *at = pos;
    else {
        *boxes = it;
        *ftyp = type;
    }
    return error;
}

/* Header/tree validation keeps its implementation limit independently of
 * prefix reading. Its caller selects JP2 or JPX validation explicitly. */
static const char *check_file_start(const uint8_t *buf, size_t size, int jpx, hv_ftyp *ftyp,
                                    size_t *at) {
    hv_boxes it;
    const char *error;
    *at = 0;
    if (size > INT_MAX)
        return "file.size-limit";
    if ((error = open_file(buf, size, &it, ftyp, at)) != NULL)
        return error;
    return hv_rule_ftyp(ftyp, jpx, 0);
}

/* Shared JP2 discovery. The profile adds the primary brand and size
 * restrictions; framing, compatibility and the single codestream are shared. */
static const char *read_jp2(const uint8_t *buf, size_t size, hv_box *jp2c, int profile, size_t *at) {
    hv_boxes it;
    hv_box box;
    hv_ftyp ftyp;
    const char *error;
    size_t n = 0;
    int status;
    size_t codestreams = 0;

    *at = 0;
    if (profile && size > INT_MAX)
        return "file.size-limit";
    if ((error = check_start(buf, size)) != NULL)
        return error;
    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &error, at)) == 1) {
        *at = box.start;
        if (n == 1 && (error = check_ftyp(buf, &box, 0, profile, &ftyp)) != NULL)
            return error;
        if (box.type == HV_BOX_JP2C && codestreams++ == 0)
            *jp2c = box;
        n++;
    }
    if (status < 0)
        return error;
    *at = size;
    if (n < 2)
        return "file.two-boxes";
    if (codestreams != 1)
        return "jp2.one-codestream";
    return NULL;
}

const char *hv_served_jp2(const uint8_t *buf, size_t size, hv_box *jp2c, size_t *at) {
    size_t pos;
    const char *error = read_jp2(buf, size, jp2c, 1, &pos);
    if (error != NULL)
        *at = pos;
    return error;
}

const char *hv_read_jp2(const uint8_t *buf, size_t size, hv_box *jp2c, size_t *at) {
    hv_box box;
    size_t pos;
    const char *error = read_jp2(buf, size, &box, 0, &pos);
    if (error != NULL)
        *at = pos;
    else
        *jp2c = box;
    return error;
}

const char *hv_fragments_open(const uint8_t *buf, const hv_box *ftbl,
                              hv_fragments *fragments, size_t *at) {
    hv_boxes children;
    hv_box child, flst = {0};
    FragmentCount count;
    const char *error;
    size_t pos = ftbl->start, used, lists = 0;
    int status;

    if (ftbl->type != HV_BOX_FTBL) {
        error = "not a fragment table box";
        goto fail;
    }
    hv_boxes_children(&children, buf, ftbl);
    while ((status = hv_boxes_next(&children, &child, &error, &pos)) == 1) {
        pos = child.start;
        if ((error = hv_rule_child(HV_BOX_FTBL, child.type)) != NULL)
            goto fail;
        if (child.type == HV_BOX_FLST) {
            flst = child;
            lists++;
        }
    }
    if (status < 0)
        goto fail;
    pos = ftbl->start;
    if (lists != 1) {
        error = "ftbl.one-flst";
        goto fail;
    }
    pos = flst.start;
    if (HV_DECODE_USED(FragmentCount, &count, buf, flst.payload,
                       flst.end - flst.payload, &used) != 0) {
        error = "flst: shorter than NF";
        goto fail;
    }
    if ((flst.end - flst.payload - used) / HV_FIXED(Fragment) != count ||
        (flst.end - flst.payload - used) % HV_FIXED(Fragment) != 0) {
        error = "flst.nf-count";
        goto fail;
    }
    fragments->buf = buf;
    fragments->start = flst.payload + used;
    fragments->count = (size_t)count;
    return NULL;
fail:
    *at = pos;
    return error;
}

const char *hv_fragment_read(const hv_fragments *fragments, size_t index,
                             Fragment *fragment, size_t *at) {
    Fragment entry;
    size_t pos;
    if (index >= fragments->count) {
        *at = fragments->start;
        return "fragment index out of range";
    }
    pos = fragments->start + index * HV_FIXED(Fragment);
    if (HV_DECODE(Fragment, &entry, fragments->buf, pos, HV_FIXED(Fragment)) != 0) {
        *at = pos;
        return "flst: fragment offset (OFF) below 12";
    }
    *fragment = entry;
    return NULL;
}

const char *hv_references_open(const uint8_t *buf, const hv_box *dtbl,
                               hv_boxes *references, size_t *count, size_t *at) {
    hv_box entries = *dtbl, child;
    hv_boxes it, first;
    DataReferenceCount ndr;
    size_t used, n = 0, pos = dtbl->start;
    const char *error;
    int status;

    if (dtbl->type != HV_BOX_DTBL) {
        error = "not a data reference box";
        goto fail;
    }
    if (HV_DECODE_USED(DataReferenceCount, &ndr, buf, dtbl->payload,
                       dtbl->end - dtbl->payload, &used) != 0) {
        error = "dtbl: shorter than NDR";
        goto fail;
    }
    entries.payload += used;
    hv_boxes_children(&it, buf, &entries);
    first = it;
    while ((status = hv_boxes_next(&it, &child, &error, &pos)) == 1) {
        pos = child.start;
        if ((error = hv_rule_child(HV_BOX_DTBL, child.type)) != NULL)
            goto fail;
        n++;
    }
    if (status < 0)
        goto fail;
    pos = dtbl->start;
    if (n != ndr) {
        error = "dtbl.ndr-count";
        goto fail;
    }
    *references = first;
    *count = n;
    return NULL;
fail:
    *at = pos;
    return error;
}

const char *hv_url_read(const uint8_t *buf, const hv_box *box,
                        const uint8_t **loc, size_t *loc_size, size_t *at) {
    UrlHeader header;
    size_t n = box->end - box->payload, used;
    const uint8_t *text;
    const char *error = NULL;
    if (box->type != HV_BOX_URL)
        error = "not a URL box";
    else if (n < HV_FIXED(UrlHeader))
        error = "url: shorter than VERS and FLAG";
    else if (HV_DECODE_USED(UrlHeader, &header, buf, box->payload, n, &used) != 0)
        error = "url.version-flags";
    else {
        text = buf + box->payload + used;
        n -= used;
        if (n == 0 || memchr(text, 0, n) != text + n - 1)
            error = "url.terminator";
    }
    if (error != NULL)
        *at = box->start;
    else {
        *loc = text;
        *loc_size = n - 1;
    }
    return error;
}

/* Room for element n in p, an array of *cap elements of `size` bytes: p,
 * or p grown (realloc, *cap updated), or NULL when out of memory, with p
 * as it was. */
static void *grow(void *p, size_t *cap, size_t n, size_t size) {
    size_t c = *cap ? 2 * *cap : 16;
    if (n < *cap)
        return p;
    if ((p = realloc(p, c * size)) != NULL)
        *cap = c;
    return p;
}

void hv_served_sources_free(hv_served_sources *jpx) {
    free(jpx->jp2c);
    free(jpx->links);
    jpx->jp2c = NULL;
    jpx->links = NULL;
    jpx->count = 0;
}

/* One url box of a dtbl: LOC recorded in url. */
static const char *check_url(const uint8_t *buf, const hv_box *box, hv_served_source *url) {
    const uint8_t *loc;
    size_t n, at;
    const char *error = hv_url_read(buf, box, &loc, &n, &at);
    if (error != NULL || (error = hv_rule_url(0, 0, loc, n + 1)) != NULL)
        return error;
    url->loc = loc;
    url->loc_size = n;
    return NULL;
}

/* One flst box of an ftbl: its fragment recorded in link. */
static const char *check_flst(const uint8_t *buf, const hv_box *box, hv_served_source *link) {
    Fragment f;
    FragmentCount nf = 0;
    const char *error;
    size_t n = box->end - box->payload, used = n, p;
    int fragments = 0;
    /* NF, then as many whole fragments as the box holds (0 if it holds a
     * part of one), for hv_rule_flst. Fragment is a fixed-size type. */
    if (HV_DECODE_USED(FragmentCount, &nf, buf, box->payload, n, &used) == 0) {
        for (p = box->payload + used; p < box->end; p += HV_FIXED(Fragment)) {
            if (box->end - p < HV_FIXED(Fragment)) {
                fragments = 0;
                break;
            }
            fragments++;
        }
    }
    if ((error = hv_rule_flst(nf, fragments)) != NULL)
        return error;
    /* NF is 1 and the box holds one whole fragment. Of its fields, LEN and
     * DR take every value their bytes can hold (Fragment, T.801 Table
     * M.17), so the decoder can only fail on OFF below 12. */
    if (HV_DECODE(Fragment, &f, buf, box->payload + used, HV_FIXED(Fragment)) != 0)
        return "flst: fragment offset (OFF) below 12";
    link->offset = f.off;
    link->length = f.len;
    link->dr = f.dr;
    return NULL;
}

/* The rules of hv_rules.c, and the file rules as
 * ../spec/harness/crossfield_impl.h names them. */
static const char *check_jpx(const uint8_t *buf, size_t size, hv_served_sources *jpx, size_t *at) {
    hv_boxes it, children;
    hv_box box, child;
    hv_ftyp ftyp;
    hv_jpx_boxes count = {0, 0, 0, 0, 0, 0, 0};
    hv_served_source *urls = NULL;
    size_t *flst_at = NULL;     /* the flst box of each link, for flst.dr-* */
    size_t n = 0, nurls = 0, url_cap = 0, jp2c_cap = 0, link_cap = 0, flst_cap = 0, i;
    uint64_t ndr = 0;
    const char *error;
    int status;

    memset(jpx, 0, sizeof *jpx);
    *at = 0;
    if (size > INT_MAX)
        return "file.size-limit";
    if ((error = check_start(buf, size)) != NULL)
        return error;
    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &error, at)) == 1) {
        *at = box.start;
        if (n == 1 && (error = check_ftyp(buf, &box, 1, 1, &ftyp)) != NULL)
            goto fail;
        n++;
        if ((error = hv_rule_top_box(box.type)) != NULL)
            goto fail;
        switch (box.type) {               /* rreq is the standard layer's */
        case HV_BOX_JP2C: {
            hv_box *jp2c = grow(jpx->jp2c, &jp2c_cap, jpx->count, sizeof *jpx->jp2c);
            if (jp2c == NULL)
                goto oom;
            jpx->jp2c = jp2c;
            count.jp2c++;
            jpx->jp2c[jpx->count++] = box;
            break;
        }
        case HV_BOX_JPCH:
        case HV_BOX_FTBL: {
            int flsts = 0;
            if (box.type == HV_BOX_JPCH)
                count.jpch++;
            else
                count.ftbl++;
            hv_boxes_children(&children, buf, &box);
            while ((status = hv_boxes_next(&children, &child, &error, at)) == 1) {
                size_t k;
                *at = child.start;
                if ((error = hv_rule_child(box.type, child.type)) != NULL)
                    goto fail;
                if (child.type != HV_BOX_FLST || box.type != HV_BOX_FTBL)
                    continue;               /* hv_rule_child: an flst only in an ftbl */
                k = (size_t)count.ftbl - 1;
                if (flsts++ == 0) {
                    hv_served_source *links = grow(jpx->links, &link_cap, k, sizeof *jpx->links);
                    size_t *offsets;
                    if (links == NULL)
                        goto oom;
                    jpx->links = links;
                    if ((offsets = grow(flst_at, &flst_cap, k, sizeof *flst_at)) == NULL)
                        goto oom;
                    flst_at = offsets;
                    if ((error = check_flst(buf, &child, &jpx->links[k])) != NULL)
                        goto fail;
                    flst_at[k] = child.start;
                }
            }
            if (status < 0)
                goto fail;
            *at = box.start;
            if (box.type == HV_BOX_FTBL && flsts != 1) {
                error = "ftbl.one-flst";        /* T.801 M.11.3 */
                goto fail;
            }
            break;
        }
        case HV_BOX_DTBL: {
            DataReferenceCount c;
            hv_box refs = box;
            size_t used;
            count.dtbl++;
            if (HV_DECODE_USED(DataReferenceCount, &c, buf, box.payload, box.end - box.payload,
                               &used) != 0) {
                error = "dtbl: shorter than NDR";
                goto fail;
            }
            ndr = c;
            nurls = 0;          /* a second dtbl is jpx.one-dtbl, below */
            /* The url boxes follow NDR: walk them as the children of a box
             * whose payload starts after it. */
            refs.payload += used;
            hv_boxes_children(&children, buf, &refs);
            while ((status = hv_boxes_next(&children, &child, &error, at)) == 1) {
                hv_served_source *grown;
                *at = child.start;
                if ((error = hv_rule_child(box.type, child.type)) != NULL)
                    goto fail;
                if ((grown = grow(urls, &url_cap, nurls, sizeof *urls)) == NULL)
                    goto oom;
                urls = grown;
                if ((error = check_url(buf, &child, &urls[nurls++])) != NULL)
                    goto fail;
            }
            if (status < 0)
                goto fail;
            *at = box.start;
            if (nurls != ndr) {
                error = "dtbl.ndr-count";
                goto fail;
            }
            break;
        }
        default:
            break;                              /* asoc and the rest: opaque */
        }
    }
    if (status < 0)
        goto fail;
    *at = size;
    if (n < 2) {
        error = "file.two-boxes";
        goto fail;
    }
    if ((error = hv_rule_jpx(&count, 1)) != NULL)
        goto fail;
    /* Linked (links, no jp2c: jpx.mixed-sources) or embedded (jp2c, no
     * links, which only an ftbl gives). */
    if (count.ftbl > 0) {
        jpx->count = (size_t)count.ftbl;
        for (i = 0; i < jpx->count; i++) {
            *at = flst_at[i];
            if ((error = hv_rule_fragment_dr(jpx->links[i].dr, ndr, 1)) != NULL)
                goto fail;
            jpx->links[i].loc = urls[jpx->links[i].dr - 1].loc;
            jpx->links[i].loc_size = urls[jpx->links[i].dr - 1].loc_size;
        }
    }
    free(urls);
    free(flst_at);
    return NULL;

oom:
    error = "out of memory";
fail:
    free(urls);
    free(flst_at);
    hv_served_sources_free(jpx);
    return error;
}

const char *hv_served_jpx(const uint8_t *buf, size_t size, hv_served_sources *jpx, size_t *at) {
    size_t pos;
    const char *error = check_jpx(buf, size, jpx, &pos);
    if (error != NULL)
        *at = pos;
    return error;
}

const char *hv_served_path(const hv_served_source *link, const char *jpx_path, char *out,
                         size_t out_size) {
    size_t n;
    if (out_size == 0)
        return "no room for the linked path";
    out[0] = 0;
    {
        const char *error = hv_file_url_path(link->loc, link->loc_size, out, out_size);
        if (error != NULL) {
            out[0] = 0;
            return error;
        }
    }
    n = strlen(out);
    if (out[0] != '/' && jpx_path != NULL) {
        const char *slash = strrchr(jpx_path, '/');
        size_t dir = slash ? (size_t)(slash - jpx_path) + 1 : 0;
        if (dir > out_size - n - 1) {
            out[0] = 0;
            return "linked path longer than the caller's buffer";
        }
        memmove(out + dir, out, n + 1);
        memcpy(out, jpx_path, dir);
    }
    return NULL;
}

const char *hv_codestream_check(const uint8_t *buf, size_t start, size_t end, hv_read_mode mode,
                                size_t *at) {
    hv_codestream cs;
    hv_item item;
    const char *error = NULL;
    int status;
    if (mode == HV_READ_JPIP_INDEX) {
        *at = start;
        return "hv_codestream_check requires full PLT validation";
    }
    status = hv_codestream_open(&cs, buf, start, end, mode);
    if (status == 0)
        while ((status = hv_codestream_next(&cs, &item)) == 1)
            ;                           /* 0 after HV_END */
    if (status < 0) {
        error = cs.error;
        *at = cs.error_at;
    }
    hv_codestream_close(&cs);
    return error;
}

const char *hv_served_link(const uint8_t *buf, size_t size, const hv_served_source *link, size_t *at) {
    hv_box jp2c;
    const char *error;
    if ((error = hv_served_jp2(buf, size, &jp2c, at)) != NULL ||
        (error = hv_codestream_check(buf, jp2c.payload, jp2c.end, HV_READ_JPIP, at)) != NULL)
        return error;
    if (link->offset != jp2c.payload || link->length != jp2c.end - jp2c.payload) {
        *at = jp2c.payload;
        return "flst.source-extent";
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * JP2 header boxes (standard layer)
 * ------------------------------------------------------------------------ */

/* The header box type for hv_rule_header_child (1: any other). */
static uint32_t header_type(uint32_t type) {
    switch (type) {
    case HV_BOX_IHDR: case HV_BOX_BPCC: case HV_BOX_COLR: case HV_BOX_PCLR:
    case HV_BOX_CMAP: case HV_BOX_CDEF: case HV_BOX_RES: case HV_BOX_JP2I: case HV_BOX_CREG:
    case HV_BOX_CGRP:
        return type;
    default:
        return 1;
    }
}

/* One child of a header box, decoded with the model's types. *at is the
 * child box (read_header sets it), or the entry at fault in a box of
 * entries (bpcc, cmap, cdef) or the child at fault in res. The types
 * that end in `extra` (Ihdr, Resolution) are decoded from at most their
 * largest encoding, so that any number of bytes after the fields gives
 * <box>.extent. */
static const char *read_header_child(const uint8_t *buf, const hv_box *box, hv_header *h,
                                     int input, size_t *at) {
    size_t n = box->end - box->payload, p = box->payload, i;
    const char *error = NULL;

    switch (box->type) {
    case HV_BOX_IHDR: {
        Ihdr ihdr;
        if (HV_DECODE(Ihdr, &ihdr, buf, p, min_size(n, HV_LARGEST(Ihdr))) != 0)
            return "ihdr: shorter than its fields, or a field out of range";
        return hv_rule_ihdr(h, &ihdr);
    }
    case HV_BOX_BPCC:
        if (n == 0)
            return "bpcc: empty";
        /* Each entry a BitDepth of one byte, which hv_rule_bpcc reads in
         * place. */
        for (i = 0; i < n; i += HV_FIXED(BitDepth)) {
            BitDepth depth;
            *at = p + i;
            if (HV_DECODE(BitDepth, &depth, buf, p + i, HV_FIXED(BitDepth)) != 0)
                return "bpcc: reserved bit depth";
        }
        hv_rule_bpcc(h, buf + p, n);
        return NULL;
    case HV_BOX_COLR: {
        ColrHeader colr;
        size_t used;
        if (HV_DECODE_USED(ColrHeader, &colr, buf, p, n, &used) != 0)
            return "colr: shorter than its fields";
        /* I.5.3.3: readers ignore PREC and APPROX in a JP2 header. */
        if (input) colr.prec = colr.approx = 0;
        return hv_rule_colr(h, &colr, buf + p + used, n - used);
    }
    case HV_BOX_PCLR: {
        /* The rules point at the box: *at is left as it is. */
        PclrCounts counts;
        size_t used = HV_FIXED(PclrCounts);
        if (n < used || HV_DECODE(PclrCounts, &counts, buf, p, used) != 0)
            return "pclr: no NE and NPC, or one out of range";
        hv_rule_pclr(h, counts.ne, counts.npc);
        for (i = 0; i < counts.npc; i++) {
            BitDepth depth;
            if (n - used < HV_FIXED(BitDepth))
                return "pclr: shorter than its NPC bit depths";
            if (HV_DECODE(BitDepth, &depth, buf, p + used, HV_FIXED(BitDepth)) != 0)
                return "pclr: reserved bit depth";
            used += HV_FIXED(BitDepth);
            hv_rule_pclr_column(h, depth);
        }
        return hv_rule_pclr_end(h, buf + p + used, n - used);
    }
    case HV_BOX_CMAP:
        if (n == 0 || n % HV_FIXED(CmapEntry) != 0)
            return "cmap: not a whole number of entries";
        for (i = 0; i < n && error == NULL; i += HV_FIXED(CmapEntry)) {
            CmapEntry entry;
            *at = p + i;
            if (HV_DECODE(CmapEntry, &entry, buf, p + i, HV_FIXED(CmapEntry)) != 0)
                return "cmap: invalid entry";
            error = hv_rule_cmap_entry(h, &entry);
        }
        if (error == NULL)
            hv_rule_cmap_end(h, buf + p, n / HV_FIXED(CmapEntry));
        return error;
    case HV_BOX_CDEF: {
        CdefCount count;
        CdefEntry *entries;
        uint64_t *scratch;
        size_t used, first;
        if (HV_DECODE_USED(CdefCount, &count, buf, p, n, &used) != 0)
            return "cdef: no N, or N is 0";
        if ((n - used) / HV_FIXED(CdefEntry) < count)
            return "cdef: shorter than its N entries";
        /* The entries, and room for hv_rule_cdef to sort that many. */
        entries = malloc((size_t)count * (sizeof *entries + sizeof *scratch));
        if (entries == NULL)
            return "out of memory";
        scratch = (uint64_t *)(void *)(entries + count);
        first = used;
        for (i = 0; i < count && error == NULL; i++, used += HV_FIXED(CdefEntry)) {
            *at = p + used;
            if (HV_DECODE(CdefEntry, &entries[i], buf, p + used, HV_FIXED(CdefEntry)) != 0)
                error = "cdef: invalid entry";
        }
        if (error == NULL) {
            *at = box->start;
            if ((error = hv_rule_cdef(h, entries, (size_t)count, buf + p + first, scratch)) ==
                NULL)
                error = hv_rule_extent(HV_BOX_CDEF, n - used);
        }
        free(entries);
        return error;
    }
    case HV_BOX_RES: {
        hv_boxes it;
        hv_box child;
        int status;
        hv_boxes_children(&it, buf, box);
        while ((status = hv_boxes_next(&it, &child, &error, at)) == 1) {
            Resolution r;
            int resolution = child.type == HV_BOX_RESC || child.type == HV_BOX_RESD;
            *at = child.start;
            if (resolution && HV_DECODE(Resolution, &r, buf, child.payload,
                                        min_size(child.end - child.payload,
                                              HV_LARGEST(Resolution))) != 0)
                return "res: resc or resd shorter than its fields, or a field out of range";
            hv_rule_res_child(h, child.type);
            if (resolution &&
                (error = hv_rule_extent(child.type, (uint64_t)r.extra.nCount)) != NULL)
                return error;
        }
        if (status < 0)
            return error;
        *at = box->start;
        return hv_rule_res_end(h);
    }
    case HV_BOX_CGRP: {
        /* Its colr boxes, as jp2h's. In a JP2 file, where T.800 does not
         * define it, a box to skip. */
        hv_boxes it;
        hv_box child;
        hv_header g;
        int status;
        if (!h->jpx)
            return NULL;
        hv_header_init(&g, HV_BOX_CGRP, 1);
        hv_boxes_children(&it, buf, box);
        while ((status = hv_boxes_next(&it, &child, &error, at)) == 1) {
            ColrHeader colr;
            size_t used;
            *at = child.start;
            hv_rule_cgrp_child(&g, child.type);
            if (child.type != HV_BOX_COLR)
                continue;               /* T.800 I.8 */
            if (HV_DECODE_USED(ColrHeader, &colr, buf, child.payload, child.end - child.payload,
                               &used) != 0)
                return "cgrp: colr shorter than its fields";
            if ((error = hv_rule_colr(&g, &colr, buf + child.payload + used,
                                      child.end - child.payload - used)) != NULL)
                return error;
        }
        if (status < 0)
            return error;
        *at = box->start;
        return hv_rule_cgrp_end(h, &g);
    }
    default:
        return NULL;
    }
}

/* A header box (jp2h, jpch or jplh), child by child; in a JPX file that
 * lists 'jp2 ' (jp2), a jp2h is also read as a JP2 file's. */
static const char *read_header(const uint8_t *buf, const hv_box *box, uint32_t parent, int jpx,
                               int jp2, int input, hv_header *h, size_t *at) {
    hv_boxes it;
    hv_box child;
    const char *error;
    int status;

    hv_header_init(h, parent, jpx);
    h->jp2 |= jp2;
    hv_boxes_children(&it, buf, box);
    while ((status = hv_boxes_next(&it, &child, &error, at)) == 1) {
        *at = child.start;
        if ((error = hv_rule_header_child(h, header_type(child.type))) != NULL ||
            (error = read_header_child(buf, &child, h, input, at)) != NULL)
            return error;
    }
    return status < 0 ? error : NULL;
}

/* hv_rule_box_tree, with room for every mdat box (tree->mdat, which the
 * caller frees): a first walk counts them, a second records them. */
static const char *check_tree(const uint8_t *buf, size_t size, int jpx, hv_box_tree *tree,
                              size_t *at) {
    const char *error;
    tree->mdat = NULL;
    tree->mdat_cap = 0;
    if ((error = hv_rule_box_tree(buf, size, jpx, tree, at)) != NULL || tree->mdat_count == 0)
        return error;
    if ((tree->mdat = malloc(tree->mdat_count * sizeof *tree->mdat)) == NULL)
        return "out of memory";
    tree->mdat_cap = tree->mdat_count;
    return hv_rule_box_tree(buf, size, jpx, tree, at);
}

static int read_main_header(hv_codestream *cs, const uint8_t *buf, size_t start, size_t end,
                            int jpx, int *mct);

/* Compare header boxes with SIZ and JP2 MCT information. Other coding
 * fields remain opaque; full codestream validation is separate. */
static const char *check_codestream_header(const uint8_t *buf, size_t start, size_t end,
                                           const hv_header *h, const hv_header *defaults,
                                           int jpx, size_t header_at, size_t *at) {
    hv_codestream cs;
    const char *error;
    int mct;
    if (read_main_header(&cs, buf, start, end, jpx, &mct) != 0) {
        error = cs.error;
        *at = cs.error_at;
    } else {
        error = hv_rule_codestream_header(h, defaults, &cs.siz, mct, jpx);
        *at = header_at;
    }
    hv_codestream_close(&cs);
    return error;
}

/* The JP2 rules on the boxes of a JP2 file, or of a JPX file that lists
 * 'jp2 ' (jp2h was read so, into h: not `read`), the box tree walked:
 * jp2h placed, its header against the first codestream, and in a JP2
 * file its IPR (in a JPX file an ihdr's IPR is its codestreams', T.801
 * M.11.5.1: jpx.ipr). */
static const char *check_jp2_rules(const uint8_t *buf, size_t size, const hv_box_tree *tree,
                                   const hv_box *jp2h, const hv_box *jp2c, hv_header *h,
                                   int read, int input, size_t *at) {
    const char *error;
    *at = size;
    if ((error = hv_rule_jp2h_place(tree, 0, 0)) != NULL)
        return error;
    *at = jp2h->start;
    if (read && (error = read_header(buf, jp2h, HV_BOX_JP2H, 0, 1, input, h, at)) != NULL)
        return error;
    if ((error = check_codestream_header(buf, jp2c->payload, jp2c->end, h, NULL, 0, jp2h->start,
                                         at)) != NULL)
        return error;
    return read ? hv_rule_ihdr_ipr(h, tree->ipr) : NULL;   /* *at is jp2h->start */
}

static const char *check_jp2h(const uint8_t *buf, size_t size, hv_header *h, int input,
                              size_t *at) {
    hv_boxes it;
    hv_box box, jp2h = {0}, jp2c = {0};
    hv_ftyp ftyp;
    hv_box_tree tree;
    const char *error;
    size_t ftyp_at;
    int status, jp2hs = 0, codestreams = 0;

    if ((error = check_file_start(buf, size, 0, &ftyp, at)) != NULL)
        return error;
    ftyp_at = *at;
    *at = 0;
    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &error, at)) == 1) {
        if (box.type == HV_BOX_JP2H && jp2hs++ == 0)
            jp2h = box;
        else if (box.type == HV_BOX_JP2C && codestreams++ == 0)
            jp2c = box;
    }
    if (status < 0)
        return error;
    if ((error = check_tree(buf, size, 0, &tree, at)) != NULL)
        return error;
    free(tree.mdat);
    /* MinV (T.800 I.5.2), of the File Type box check_file_start read. */
    *at = ftyp_at;
    if (!input && (error = hv_rule_ftyp_minor(ftyp.minor, 0)) != NULL)
        return error;
    return check_jp2_rules(buf, size, &tree, &jp2h, &jp2c, h, 1, input, at);
}

const char *hv_read_jp2h_boxes(const uint8_t *buf, const hv_box *jp2h,
                               const hv_box *jp2c, hv_header *h, size_t *at) {
    hv_header header;
    size_t pos = jp2h->start;
    const char *error = read_header(buf, jp2h, HV_BOX_JP2H, 0, 1, 1, &header, &pos);
    if (error == NULL)
        error = check_codestream_header(buf, jp2c->payload, jp2c->end, &header, NULL,
                                         0, jp2h->start, &pos);
    if (error != NULL)
        *at = pos;
    else
        *h = header;
    return error;
}

const char *hv_read_jp2h(const uint8_t *buf, size_t size, hv_header *h, size_t *at) {
    size_t pos;
    const char *error = check_jp2h(buf, size, h, 1, &pos);
    if (error != NULL)
        *at = pos;
    return error;
}

const char *hv_check_jp2h(const uint8_t *buf, size_t size, size_t *at) {
    hv_header h;
    size_t pos;
    const char *error = check_jp2h(buf, size, &h, 0, &pos);
    if (error != NULL) *at = pos;
    return error;
}

/* The contents of a Reader Requirements box, one part at a time:
 * RreqHeader (ML, FUAM, DCM, NSF), NSF standard features, NVF
 * (FeatureCount) and NVF vendor features, each feature handed exactly its
 * code and ML bytes, the mask's size (RreqMask, size deduced); no
 * deprecated standard feature (rreq.deprecated-feature); and nothing
 * after them (rreq.extent). */
static const char *check_rreq(const uint8_t *buf, const hv_box *box) {
    static const char invalid[] =
        "rreq: contents are not ML, FUAM, DCM, NSF standard and NVF vendor features";
    RreqHeader h;
    RreqStandardFeature sf;
    FeatureCount nvf;
    RreqVendorFeature vf;
    const char *error;
    size_t p = box->payload, used, ml, i;

    if (HV_DECODE_USED(RreqHeader, &h, buf, p, box->end - p, &used) != 0)
        return invalid;
    p += used;
    ml = (size_t)h.fuam.nCount;
    for (i = 0; i < h.nsf; i++, p += HV_FIXED(FeatureCode) + ml) {
        if (box->end - p < HV_FIXED(FeatureCode) + ml ||
            HV_DECODE(RreqStandardFeature, &sf, buf, p, HV_FIXED(FeatureCode) + ml) != 0)
            return invalid;
        if ((error = hv_rule_rreq_feature(sf.sf)) != NULL)
            return error;
    }
    if (HV_DECODE_USED(FeatureCount, &nvf, buf, p, box->end - p, &used) != 0)
        return invalid;
    p += used;
    for (i = 0; i < nvf; i++, p += HV_FIXED(VendorId) + ml)
        if (box->end - p < HV_FIXED(VendorId) + ml ||
            HV_DECODE(RreqVendorFeature, &vf, buf, p, HV_FIXED(VendorId) + ml) != 0)
            return invalid;
    return hv_rule_extent(HV_BOX_RREQ, (uint64_t)(box->end - p));
}

/* Linear passes over the top-level boxes. The first counts them for the
 * file rules (hv_rule_jpx at the standard layer); hv_rule_box_tree walks
 * the tree, and hv_rule_fragments the fragments of the file's own
 * codestreams. The last reads, in box order, each jplh for its own rules
 * (its header describes a compositing layer, which nothing else here
 * checks) and each codestream against its header: codestream k (jp2c or
 * ftbl) against jpch k, which a second iterator finds by moving on from
 * jpch k - 1, over the jp2h defaults. A file that lists 'jp2 ' is then
 * checked as a JP2 file too (check_jp2_rules). */
static const char *check_jpx_headers(const uint8_t *buf, size_t size, size_t *at) {
    hv_boxes it, next_jpch;
    hv_box box, jpch, jp2h = {0}, rreq = {0}, ftyp_box = {0}, jplh = {0}, jp2c = {0};
    hv_jpx_boxes count = {0, 0, 0, 0, 0, 0, 0};
    hv_header h[2], *defaults, first_jpch, first_jplh;  /* h[0]: the jpch or jplh read */
    hv_ftyp ftyp;
    hv_box_tree tree = {0};
    const char *error = NULL;
    int status, boxes = 0, jplhs = 0, cregs = 0, jpchs_read = 0;

    if ((error = check_file_start(buf, size, 1, &ftyp, at)) != NULL)
        return error;
    *at = 0;
    hv_boxes_file(&it, buf, size);
    while ((status = hv_boxes_next(&it, &box, &error, at)) == 1) {
        if (++boxes == 2)
            ftyp_box = box;
        if (boxes == 3)
            count.rreq_third = box.type == HV_BOX_RREQ;
        switch (box.type) {
        case HV_BOX_RREQ:
            if (count.rreq++ == 0)
                rreq = box;
            break;
        case HV_BOX_JP2H:
            if (jp2h.end == 0)
                jp2h = box;
            break;
        case HV_BOX_JPCH: count.jpch++; break;
        case HV_BOX_JP2C:
            if (count.jp2c++ == 0)
                jp2c = box;
            break;
        case HV_BOX_FTBL: count.ftbl++; break;
        case HV_BOX_DTBL: count.dtbl++; break;
        case HV_BOX_J2CX: case HV_BOX_JCLX: count.extensions = 1; break;
        default: break;
        }
    }
    if (status < 0)
        return error;
    *at = size;
    if ((error = hv_rule_jpx(&count, 0)) != NULL ||
        (error = check_tree(buf, size, 1, &tree, at)) != NULL ||
        (error = hv_rule_fragments(buf, size, &tree, ftyp.jpxb, at)) != NULL)
        goto done;
    /* MinV (T.801 M.8), of the File Type box check_file_start read. */
    *at = ftyp_box.start;
    if ((error = hv_rule_ftyp_minor(ftyp.minor, 1)) != NULL)
        goto done;
    *at = rreq.start;
    if ((error = check_rreq(buf, &rreq)) != NULL)
        goto done;
    *at = size;
    if ((error = hv_rule_jp2h_place(&tree, 1, ftyp.jpxb)) != NULL)
        goto done;
    defaults = tree.jp2h ? &h[1] : NULL;
    if (defaults != NULL) {
        *at = jp2h.start;
        if ((error = read_header(buf, &jp2h, HV_BOX_JP2H, 1, ftyp.jp2, 0, defaults, at)) != NULL)
            goto done;
    }
    hv_boxes_file(&it, buf, size);
    hv_boxes_file(&next_jpch, buf, size);
    while (error == NULL && hv_boxes_next(&it, &box, &error, at) == 1) {
        const hv_header *own = NULL;
        size_t header_at = defaults ? jp2h.start : box.start;
        if (box.type == HV_BOX_JPLH) {
            *at = box.start;
            error = read_header(buf, &box, HV_BOX_JPLH, 1, 0, 0, h, at);
            if (jplhs++ == 0) {
                jplh = box;
                first_jplh = *h;
            }
            cregs += h->creg > 0;
            continue;
        }
        if (box.type != HV_BOX_JP2C && box.type != HV_BOX_FTBL)
            continue;
        if (count.jpch > 0) {
            /* Its header: the next jpch. hv_rule_jpx has counted as many
             * as codestreams (or found a j2cx or jclx, whose codestreams
             * and headers are not read); without one left, the codestream
             * has no header here. */
            while ((status = hv_boxes_next(&next_jpch, &jpch, &error, at)) == 1 &&
                   jpch.type != HV_BOX_JPCH)
                ;
            if (status != 1) {
                if (status == 0 && !count.extensions) {
                    *at = box.start;
                    error = "jpx.codestream-count";
                }
                break;
            }
            *at = jpch.start;
            if ((error = read_header(buf, &jpch, HV_BOX_JPCH, 1, 0, 0, h, at)) != NULL)
                break;
            if (jpchs_read++ == 0)
                first_jpch = *h;
            own = h;
            header_at = jpch.start;
        }
        if (box.type == HV_BOX_JP2C) {
            error = check_codestream_header(buf, box.payload, box.end, own, defaults, 1,
                                            header_at, at);
        } else {
            *at = header_at;
            error = hv_rule_codestream_header(own, defaults, NULL, -1, 1);
        }
        if (error == NULL && (error = hv_rule_jpx_ipr(own, defaults, tree.ipr)) != NULL)
            *at = header_at;
    }
    if (error == NULL && (error = hv_rule_jpx_creg(jplhs, cregs)) != NULL)
        *at = jplh.start;
    if (error == NULL && ftyp.jpxb &&
        (error = hv_rule_jpxb_layer(defaults, jpchs_read ? &first_jpch : NULL,
                                    jplhs ? &first_jplh : NULL)) != NULL)
        *at = size;
    if (error == NULL && ftyp.jp2)
        error = check_jp2_rules(buf, size, &tree, &jp2h, &jp2c, &h[1], 0, 0, at);
done:
    free(tree.mdat);
    return error;
}

const char *hv_check_jpx_headers(const uint8_t *buf, size_t size, size_t *at) {
    size_t pos;
    const char *error = check_jpx_headers(buf, size, &pos);
    if (error != NULL)
        *at = pos;
    return error;
}

const char *hv_check_headers(const uint8_t *buf, size_t size, size_t *at) {
    hv_ftyp ftyp;
    return hv_ftyp_read(buf, size, &ftyp) == 0 && hv_ftyp_is_jpx(&ftyp)
               ? hv_check_jpx_headers(buf, size, at) : hv_check_jp2h(buf, size, at);
}

/* ------------------------------------------------------------------------
 * Codestream
 * ------------------------------------------------------------------------ */

enum { ST_SIZ, ST_MAIN, ST_TILE_HEADER, ST_AFTER_DATA, ST_DONE };

static int fail(hv_codestream *cs, const char *error, size_t at) {
    cs->error = error;
    cs->error_at = at;
    cs->state = ST_DONE;
    return -1;
}

static int packets_only(const hv_codestream *cs) {
    return cs->mode == HV_READ_PACKETS || cs->mode == HV_READ_PACKETS_JPIP;
}
static int profile_full(const hv_codestream *cs) {
    return cs->mode == HV_READ_JPIP || cs->mode == HV_READ_JPIP_INDEX;
}
static int profile_headers(const hv_codestream *cs) {
    return profile_full(cs) || cs->mode == HV_READ_PACKETS_JPIP;
}

/* SIZ: the shared cross-field rules (hv_rules.c), and in the profile the
 * model's Siz-Profile type (SizFixed-Profile; Component-Profile, unit
 * sampling, is siz.component-sampling); then the tiles Isot can address. */
static const char *check_siz(const hv_codestream *cs, const hv_siz *s, uint32_t *tiles) {
    const char *error;
    int err;
    error = packets_only(cs) && !profile_headers(cs)
                ? hv_rule_siz_geometry(s, NULL)
                : hv_rule_siz(s, NULL, profile_headers(cs));
    if (error != NULL)
        return error;
    /* What Siz-Profile adds to the rules: the image extent. */
    if (profile_headers(cs) && !SizFixed_Profile_IsConstraintValid(s->fixed, &err))
        return "SIZ: Xsiz or Ysiz above 2,147,483,647 (Siz-Profile)";
    /* Both rule sets bound the grid to the tiles Isot can address. */
    *tiles = hv_rule_tiles(s);
    return NULL;
}

/* The SIZ segment in buf[start, end) after its code and Lsiz: SizFixed,
 * then components one at a time up to the end of the segment, as many as
 * a Csiz can count (the model's `count == csiz` is siz.csiz-count, in
 * hv_rule_siz). -1 when they do not decode ("invalid SIZ"). */
static int decode_siz(hv_codestream *cs, size_t start, size_t end) {
    SizFixed_csiz count;
    size_t used, n, i;
    int err;
    if (HV_DECODE_USED(SizFixed, &cs->siz_fixed, cs->buf, start, end - start, &used) != 0)
        return -1;
    start += used;
    /* Component is a fixed-size type: the segment holds n whole ones. */
    n = (end - start) / HV_FIXED(Component);
    count = n;
    if ((end - start) % HV_FIXED(Component) != 0 || !SizFixed_csiz_IsConstraintValid(&count, &err))
        return -1;
    if ((cs->components = malloc(n * sizeof *cs->components)) == NULL)
        return -2;
    for (i = 0; i < n; i++, start += HV_FIXED(Component))
        if (HV_DECODE(Component, &cs->components[i], cs->buf, start, HV_FIXED(Component)) != 0)
            return -1;
    cs->siz.fixed = &cs->siz_fixed;
    cs->siz.components = cs->components;
    cs->siz.ncomponents = n;
    return 0;
}

const char *hv_read_siz(const uint8_t *buf, size_t size, SizFixed *fixed,
                        Component *components, size_t capacity, size_t *at) {
    SizFixed fields;
    Component component;
    size_t length, count, used, start = 6;
    const char *error = NULL;
    if (size < 6 || buf[0] != 0xff || buf[1] != 0x4f ||
        buf[2] != 0xff || buf[3] != 0x51)
        error = "expected SOC followed by SIZ";
    else {
        length = ((size_t)buf[4] << 8) | buf[5];
        if (length < 2 || length > size - 4)
            error = "truncated SIZ";
        else if (HV_DECODE_USED(SizFixed, &fields, buf, start, length - 2, &used) != 0)
            error = "invalid SIZ fields";
        else {
            start += used;
            count = (length - 2 - used) / HV_FIXED(Component);
            if ((length - 2 - used) % HV_FIXED(Component) != 0 ||
                count != fields.csiz)
                error = "siz.csiz-count";
            else if (components != NULL && capacity < count)
                error = "component buffer too small";
            else {
                /* Validate before publishing any output. */
                for (size_t i = 0; i < count; i++)
                    if (HV_DECODE(Component, &component, buf, start + i * HV_FIXED(Component),
                                  HV_FIXED(Component)) != 0) {
                        error = "invalid SIZ component";
                        break;
                    }
                if (error == NULL) {
                    if (components != NULL)
                        for (size_t i = 0; i < count; i++)
                            HV_DECODE(Component, &components[i], buf, start + i * HV_FIXED(Component),
                                      HV_FIXED(Component));
                    *fixed = fields;
                    return NULL;
                }
            }
        }
    }
    *at = 2;
    return error;
}

/* COD: the shared cross-field rules, with the SIZ they depend on. SOP is
 * outside the profile, but only the full scope checks it: a transcoder's
 * input may carry SOP, its output may not. */
static const char *check_cod(const hv_codestream *cs, const Cod *c) {
    const char *error = hv_rule_cod(&c->scod, &c->spcod, profile_full(cs));
    if (error != NULL) return error;
    return packets_only(cs) && !profile_headers(cs)
               ? hv_rule_siz_geometry(&cs->siz, &c->sgcod)
               : hv_rule_siz(&cs->siz, &c->sgcod, profile_headers(cs));
}

/* The Iplt entry at *pos, within end: its value, and *pos past it. NULL;
 * "invalid PLT" when it does not decode, or plt.value-overflow
 * (hv_rule_iplt), with *pos unchanged. The profile's entries are at most
 * ten bytes (hv_rule_packet_length). */
static const char *plt_entry(const uint8_t *buf, size_t *pos, size_t end, int profile,
                             uint64_t *value) {
    const char *error = hv_rule_packet_length(buf, pos, end, profile, value);
    return error != NULL && strcmp(error, "decode") == 0 ? "invalid PLT" : error;
}

int hv_plt_next(const uint8_t *buf, size_t *pos, size_t end, uint64_t *value) {
    if (*pos >= end)
        return 0;
    return plt_entry(buf, pos, end, 0, value) == NULL ? 1 : -1;
}

void hv_plt_init(hv_plt_reader *r, hv_plt_mode mode) {
    memset(r, 0, sizeof *r);
    r->mode = mode;
    r->cached_byte = 256;
    if (mode != HV_PLT_STANDARD && mode != HV_PLT_PADDING && mode != HV_PLT_PROFILE)
        r->error = "invalid PLT mode";
}

static void plt_reset_tile(hv_plt_reader *r) {
    memset(&r->zplt, 0, sizeof r->zplt);
    r->zeros = 0;
    r->sum = 0;
}

const char *hv_plt_begin(hv_plt_reader *r, const hv_plt *plt) {
    if (r->error != NULL) return r->error;
    if (r->pos != r->end)
        return r->error = "PLT segment not consumed";
    if (plt->start >= plt->end)
        return r->error = "invalid PLT";
    r->pos = plt->start;
    r->end = plt->end;
    r->rule_error = hv_rule_zplt(&r->zplt, plt->zplt,
                                 r->mode != HV_PLT_STANDARD);
    return NULL;
}

int hv_plt_read(hv_plt_reader *r, const uint8_t *buf, uint64_t *value) {
    int profile = r->mode == HV_PLT_PROFILE;
    int per_tile_part = r->mode == HV_PLT_PADDING;
    if (r->error != NULL) return -1;
    while (r->pos < r->end) {
        uint64_t v;
        const char *error = NULL;
        size_t start = r->pos;
        /* Reuse the generated decoder's result for an identical one-byte
         * entry. This is common for trailing padding. Changed bytes and
         * multi-byte entries still go through the decoder, and entry rules
         * below still run for every occurrence. No buffer pointer is saved. */
        if (r->cached_byte == buf[start]) {
            v = r->cached_value;
            r->pos++;
        } else {
            error = plt_entry(buf, &r->pos, r->end, profile, &v);
            if (error == NULL && r->pos - start == 1) {
                r->cached_byte = buf[start];
                r->cached_value = (uint8_t)v;
            }
        }
        if (error != NULL) {
            if (strcmp(error, "invalid PLT") == 0) {
                r->error = error;
                return -1;
            }
            if (r->rule_error == NULL) r->rule_error = error;
            /* Overflow leaves pos unchanged; continue decoding the suffix. */
            while (r->pos < r->end && (buf[r->pos++] & 0x80))
                ;
            continue;
        }
        if (r->rule_error != NULL) continue;
        r->rule_error = per_tile_part
            ? (v != 0 && r->zeros != 0 ? "plt.padding-position" : NULL)
            : hv_rule_plt_entry(&r->count, v, profile);
        if (r->rule_error != NULL) continue;
        if (v == 0)
            r->zeros++;
        else
            r->sum = v > UINT64_MAX - r->sum ? UINT64_MAX : r->sum + v;
        *value = v;
        return 1;
    }
    r->error = r->rule_error;
    return r->error != NULL ? -1 : 0;
}

const char *hv_plt_check_segments(const hv_plt *segments, size_t count, size_t *at) {
    hv_zplt z = {0};
    size_t i;
    for (i = 0; i < count; ++i) {
        const char *error = hv_rule_zplt(&z, segments[i].zplt, 1);
        if (error != NULL) {
            *at = segments[i].start;
            return error;
        }
    }
    return count == 0 ? "codestream.no-plt" : NULL;
}

int hv_plt_packet(hv_plt_reader *r, const uint8_t *buf, uint64_t data_size,
                  int last_segment, uint64_t packets, uint64_t *offset, uint64_t *length) {
    int status;
    if (r->error != NULL) return -1;
    if (r->mode != HV_PLT_PROFILE) {
        r->error = "PLT packet indexing requires HV_PLT_PROFILE";
        return -1;
    }
    do {
        status = hv_plt_read(r, buf, length);
    } while (status == 1 && *length == 0);
    if (status != 1) return status;
    /* Before the final logical packet, the PLT and data must end together.
     * The final packet's remaining padding is checked by the caller's drain
     * and completion, before that packet is published. */
    if (r->sum > data_size ||
        (r->count.packets != packets &&
         (r->pos == r->end && last_segment) != (r->sum == data_size))) {
        r->error = "plt.coverage";
        return -1;
    }
    *offset = r->sum - *length;
    return 1;
}

const char *hv_plt_end_tile(hv_plt_reader *r, size_t data_size) {
    if (r->error != NULL) return r->error;
    if (r->mode != HV_PLT_PROFILE)
        return r->error = "PLT completion requires HV_PLT_PROFILE";
    if (r->pos != r->end)
        return r->error = "PLT segment not consumed";
    if (r->rule_error != NULL) return r->error = r->rule_error;
    if (r->zplt.count == 0) return r->error = "codestream.no-plt";
    if ((r->error = hv_rule_plt_coverage(r->sum, data_size, 0)) != NULL)
        return r->error;
    plt_reset_tile(r);
    return NULL;
}

const char *hv_plt_end(hv_plt_reader *r, uint64_t packets) {
    if (r->error != NULL) return r->error;
    if (r->mode != HV_PLT_PROFILE)
        return r->error = "PLT completion requires HV_PLT_PROFILE";
    if (r->pos != r->end || r->zplt.count != 0)
        return r->error = "PLT tile-part not completed";
    return r->error = hv_rule_plt_packets(&r->count, packets, 1);
}

/* T.800 Table A.2: marker framing and header placement. A decode name
 * also marks the segments whose contents hv_segments checks. Unknown codes
 * retain the reader's skip behavior. */
enum {
    NO_SEGMENT = 1, MAIN_FORBIDDEN = 2, TILE_FORBIDDEN = 4,
    MAIN_LAYOUT = 8, TILE_LAYOUT = 16
};
typedef struct {
    unsigned properties;
    const char *decode_error;
} marker_info;

static const marker_info markers[256] = {
    [HV_SOC & 0xFF] = {NO_SEGMENT | MAIN_FORBIDDEN | TILE_FORBIDDEN, NULL},
    [HV_SIZ & 0xFF] = {MAIN_FORBIDDEN | TILE_FORBIDDEN, NULL},
    [HV_SOT & 0xFF] = {NO_SEGMENT, NULL},
    [HV_SOD & 0xFF] = {NO_SEGMENT | MAIN_FORBIDDEN, NULL},
    [HV_EOC & 0xFF] = {NO_SEGMENT, NULL},
    [HV_SOP & 0xFF] = {NO_SEGMENT | MAIN_FORBIDDEN | TILE_FORBIDDEN, NULL},
    [HV_EPH & 0xFF] = {NO_SEGMENT | MAIN_FORBIDDEN | TILE_FORBIDDEN, NULL},
    [HV_COD & 0xFF] = {TILE_LAYOUT, "invalid COD"},
    [HV_QCD & 0xFF] = {0, "invalid QCD"},
    [HV_COC & 0xFF] = {MAIN_LAYOUT | TILE_LAYOUT, "invalid COC"},
    [HV_QCC & 0xFF] = {0, "invalid QCC"},
    [HV_RGN & 0xFF] = {0, "invalid RGN"},
    [HV_POC & 0xFF] = {MAIN_LAYOUT | TILE_LAYOUT, "invalid POC"},
    [HV_TLM & 0xFF] = {TILE_FORBIDDEN, "invalid TLM"},
    [HV_PLM & 0xFF] = {TILE_FORBIDDEN, "invalid PLM"},
    [HV_PPM & 0xFF] = {MAIN_LAYOUT | TILE_FORBIDDEN, "invalid PPM"},
    [HV_PPT & 0xFF] = {MAIN_FORBIDDEN | TILE_LAYOUT, "invalid PPT"},
    [HV_CRG & 0xFF] = {TILE_FORBIDDEN, "invalid CRG"},
    [HV_PLT & 0xFF] = {MAIN_FORBIDDEN, NULL},
};

static const marker_info *marker(uint16_t code) {
    static const marker_info reserved = {NO_SEGMENT, NULL};
    if (code >= HV_NO_SEGMENT_FIRST && code <= HV_NO_SEGMENT_LAST)
        return &reserved;
    return &markers[code >> 8 == 0xFF ? code & 0xFF : 0];
}

/* Translate a generated decoder's failure at the segment's offset. */
static int segment_fail(hv_codestream *cs, const char *error, uint16_t code, size_t pos) {
    const char *name = marker(code)->decode_error;
    return fail(cs, name != NULL && strcmp(error, "decode") == 0 ? name : error, pos);
}

/* SIZ requirements of each reader operation. */
enum siz_mode { SIZ_VALIDATION, SIZ_HEADER };

static int open_codestream_siz(hv_codestream *cs, const uint8_t *buf, size_t start, size_t end,
                                hv_read_mode operation, enum siz_mode mode) {
    MarkerCode m;
    SegmentLength l;
    const char *error;

    memset(cs, 0, sizeof *cs);
    cs->buf = buf;
    cs->end = end;
    cs->mode = operation;
    if (operation < HV_READ_VALIDATE || operation > HV_READ_PACKETS_JPIP)
        return fail(cs, "invalid codestream read mode", start);
    hv_plt_init(&cs->plt_reader, profile_full(cs) ? HV_PLT_PROFILE
                : operation == HV_READ_PADDED ? HV_PLT_PADDING : HV_PLT_STANDARD);
    if (start > end)
        return fail(cs, "codestream ends before it starts", start);
    if (HV_DECODE(MarkerCode, &m, buf, start, min_size(end - start, MARKER)) != 0 || m != HV_SOC)
        return fail(cs, "codestream does not start with SOC", start);
    cs->pos = start + MARKER;
    if (HV_DECODE(MarkerCode, &m, buf, cs->pos, min_size(end - cs->pos, MARKER)) != 0 ||
        m != HV_SIZ)
        return fail(cs, "SOC is not followed by SIZ", cs->pos);
    if (HV_DECODE(SegmentLength, &l, buf, cs->pos + MARKER,
                  min_size(end - cs->pos - MARKER, HV_FIXED(SegmentLength))) != 0 ||
        l > end - cs->pos - MARKER)
        return fail(cs, "truncated SIZ", cs->pos);
    switch (decode_siz(cs, cs->pos + MARKER + HV_FIXED(SegmentLength), cs->pos + MARKER + l)) {
    case 0:
        break;
    case -2:
        return fail(cs, "out of memory", cs->pos);
    default:
        return fail(cs, "invalid SIZ", cs->pos);
    }
    error = mode == SIZ_HEADER ? hv_rule_siz_header(&cs->siz, 0)
                        : check_siz(cs, &cs->siz, &cs->tiles);
    if (error != NULL)
        return fail(cs, error, cs->pos);
    cs->siz_end = cs->pos + MARKER + (size_t)l;
    cs->state = ST_SIZ;
    return 0;
}

int hv_codestream_open(hv_codestream *cs, const uint8_t *buf, size_t start, size_t end,
                       hv_read_mode mode) {
    if (open_codestream_siz(cs, buf, start, end, mode, SIZ_VALIDATION) != 0)
        return -1;
    if (packets_only(cs)) return 0;
    if (!profile_full(cs) &&
        (cs->segment_components = calloc(cs->siz.ncomponents, sizeof *cs->segment_components)) == NULL) {
        cs->siz_end = 0;
        return fail(cs, "out of memory", cs->pos);
    }
    hv_segments_init(&cs->segments, &cs->siz, cs->segment_components, profile_full(cs));
    return 0;
}

/* The tiles of a page of hv_codestream.tile_pages, and the pages of a
 * codestream. */
enum { TILE_PAGE = 256 };
static size_t tile_pages(const hv_codestream *cs) {
    return ((size_t)cs->tiles + TILE_PAGE - 1) / TILE_PAGE;
}
/* The tiles of page i: TILE_PAGE, but fewer in the last. */
static size_t page_tiles(const hv_codestream *cs, size_t i) {
    size_t left = (size_t)cs->tiles - i * TILE_PAGE;
    return left < TILE_PAGE ? left : TILE_PAGE;
}

/* Everything but the error goes: the accessors return NULL again. */
void hv_codestream_close(hv_codestream *cs) {
    const char *error = cs->error;
    size_t error_at = cs->error_at;
    size_t i;
    free(cs->components);
    free(cs->segment_components);
    for (i = 0; cs->tile_pages != NULL && i < tile_pages(cs); i++)
        free(cs->tile_pages[i]);
    free(cs->tile_pages);
    memset(cs, 0, sizeof *cs);
    cs->state = ST_DONE;
    cs->error = error;
    cs->error_at = error_at;
}

/* siz_end is set once hv_codestream_open succeeded, cods and qcds once the
 * main-header COD or QCD passed its checks; hv_codestream_close clears all
 * three. */
const hv_siz *hv_codestream_siz(const hv_codestream *cs) {
    return cs->siz_end ? &cs->siz : NULL;
}
const Cod *hv_codestream_cod(const hv_codestream *cs) {
    return cs->cods ? &cs->cod.body : NULL;
}
const Qcd *hv_codestream_qcd(const hv_codestream *cs) {
    return cs->qcds && !packets_only(cs) && !profile_full(cs) ? &cs->qcd.body : NULL;
}

static const char *read_marker_code(const uint8_t *buf, size_t pos, size_t limit,
                                     uint16_t *code) {
    MarkerCode m;
    if (pos > limit ||
        HV_DECODE(MarkerCode, &m, buf, pos, min_size(limit - pos, MARKER)) != 0)
        return "expected a marker";
    *code = (uint16_t)m;
    return NULL;
}

/* Called after the marker code was read within limit. */
static const char *read_marker_length(const uint8_t *buf, size_t pos, size_t limit,
                                       size_t *end) {
    SegmentLength l;
    size_t left = limit - pos - MARKER;
    if (HV_DECODE(SegmentLength, &l, buf, pos + MARKER,
                  min_size(left, HV_FIXED(SegmentLength))) != 0 || l > left)
        return "marker segment overruns its header";
    *end = pos + MARKER + (size_t)l;
    return NULL;
}

const char *hv_marker_read(const uint8_t *buf, size_t start, size_t limit, hv_marker *out) {
    hv_marker m;
    const char *error = read_marker_code(buf, start, limit, &m.code);
    if (error != NULL)
        return error;
    m.start = start;
    m.payload = m.end = start + MARKER;
    /* SOT and SOP delimit headers in hv_codestream but have segments. */
    if (!(marker(m.code)->properties & NO_SEGMENT) || m.code == HV_SOT || m.code == HV_SOP) {
        if ((error = read_marker_length(buf, start, limit, &m.end)) != NULL)
            return error;
        m.payload += HV_FIXED(SegmentLength);
    }
    *out = m;
    return NULL;
}

/* SOT is decoded by start_tile_part; SOP is rejected as out of place.
 * Other markers use the same framing as opaque access. */
static int read_segment(hv_codestream *cs, size_t limit, uint16_t *code, size_t *end) {
    size_t pos = cs->pos;
    const char *error = read_marker_code(cs->buf, pos, limit, code);
    if (error == NULL) {
        if (marker(*code)->properties & NO_SEGMENT)
            *end = pos + MARKER;
        else
            error = read_marker_length(cs->buf, pos, limit, end);
    }
    return error == NULL ? 0 : fail(cs, error, pos);
}

/* The counts of tile `tile`, its page allocated when first needed: NULL
 * when out of memory. */
static hv_tile_count *tile_count(hv_codestream *cs, uint32_t tile) {
    hv_tile_count **page;
    if (cs->tile_pages == NULL &&
        (cs->tile_pages = calloc(tile_pages(cs), sizeof *cs->tile_pages)) == NULL)
        return NULL;
    page = &cs->tile_pages[tile / TILE_PAGE];
    if (*page == NULL &&
        (*page = calloc(page_tiles(cs, tile / TILE_PAGE), sizeof **page)) == NULL)
        return NULL;
    return &(*page)[tile % TILE_PAGE];
}

static int start_tile_part(hv_codestream *cs, hv_item *item) {
    SotSegment *sot = &cs->sot;
    size_t pos = cs->pos;
    hv_tile_count *count;
    const char *error;

    if (cs->state == ST_MAIN && cs->cods != 1)
        return fail(cs, "codestream.one-cod-before-sot", pos);
    if (cs->state == ST_MAIN && cs->qcds != 1)
        return fail(cs, "codestream.one-qcd-before-sot", pos);
    if (HV_DECODE(SotSegment, sot, cs->buf, pos + MARKER,
                  min_size(cs->end - pos - MARKER, HV_FIXED(SotSegment))) != 0)
        return fail(cs, "invalid SOT", pos);
    if (profile_full(cs) &&
        (error = hv_rule_tile_part_count((uint64_t)cs->tile_parts + 1)) != NULL)
        return fail(cs, error, pos);
    if ((error = hv_rule_isot(cs->tiles, sot->isot)) != NULL)
        return fail(cs, error, pos);
    if ((count = tile_count(cs, (uint32_t)sot->isot)) == NULL)
        return fail(cs, "out of memory", pos);
    if ((error = hv_rule_tile_part(count, &cs->unfinished, sot->tpsot, sot->tnsot)) != NULL)
        return fail(cs, error, pos);
    if (sot->psot == 0) {
        /* A.4.2: the last tile-part; its data runs up to the EOC that ends
         * the codestream. */
        MarkerCode eoc;
        if (cs->end - pos < MARKER + HV_FIXED(SotSegment) + MARKER + MARKER ||   /* SOD, EOC */
            HV_DECODE(MarkerCode, &eoc, cs->buf, cs->end - MARKER, MARKER) != 0 || eoc != HV_EOC)
            return fail(cs, "Psot = 0 but the codestream does not end with EOC", pos);
        cs->tp_end = cs->end - MARKER;
    } else {
        if (sot->psot > cs->end - MARKER - pos)       /* EOC after it */
            return fail(cs, "tile-part overruns the codestream", pos);
        cs->tp_end = pos + (size_t)sot->psot;
    }
    if (!packets_only(cs) && (error = hv_segments_tile_part(&cs->segments, count, sot->isot, sot->tpsot,
                                       cs->tp_end - pos)) != NULL)
        return fail(cs, error, pos);
    cs->tile_parts++;
    cs->tp_start = pos;
    cs->tp_cod = cs->tp_qcd = cs->plts = 0;
    plt_reset_tile(&cs->plt_reader);
    cs->pos = pos + MARKER + HV_FIXED(SotSegment);
    cs->state = ST_TILE_HEADER;
    item->kind = HV_TILE_PART;
    item->code = HV_SOT;
    item->start = pos;
    item->end = cs->tp_end;
    item->sot = *sot;
    return 1;
}

/* COM at pos, ending at end: Rcom, then at least one byte of text, which
 * stays in place. */
static int decode_com(hv_codestream *cs, size_t pos, size_t end) {
    size_t body = pos + MARKER + HV_FIXED(SegmentLength), used;
    if (HV_DECODE_USED(Rcom, &cs->com.rcom, cs->buf, body, end - body, &used) != 0 ||
        end - body - used == 0)
        return fail(cs, "invalid COM", pos);
    cs->com.text = cs->buf + body + used;
    cs->com.size = end - body - used;
    return 0;
}

static int decode_cod(hv_codestream *cs, CodSegment *cod, size_t end, hv_item *item) {
    size_t pos = cs->pos;
    const char *error;
    if (HV_DECODE(CodSegment, cod, cs->buf, pos + MARKER, end - pos - MARKER) != 0)
        return fail(cs, "invalid COD", pos);
    if ((error = check_cod(cs, &cod->body)) != NULL)
        return fail(cs, error, pos);
    item->cod = cod;
    return 0;
}

/* Header boxes use SIZ and, for JP2 channel checks, COD's MCT byte.
 * Other coding fields stay opaque. Preserve framing and marker counts. */
static int read_main_header(hv_codestream *cs, const uint8_t *buf, size_t start, size_t end,
                            int jpx, int *mct) {
    hv_marker m;
    const char *error;
    if (open_codestream_siz(cs, buf, start, end, HV_READ_VALIDATE, SIZ_HEADER) != 0)
        return -1;
    cs->pos = cs->siz_end;
    *mct = 0;
    for (;;) {
        if ((error = hv_marker_read(buf, cs->pos, end, &m)) != NULL)
            return fail(cs, error, cs->pos);
        if (m.code == HV_SOT) {
            if (cs->cods != 1)
                return fail(cs, "codestream.one-cod-before-sot", m.start);
            if (cs->qcds != 1)
                return fail(cs, "codestream.one-qcd-before-sot", m.start);
            return 0;
        }
        if (m.code == HV_EOC)
            return fail(cs, "codestream.no-tile-part", m.start);
        if (marker(m.code)->properties & MAIN_FORBIDDEN)
            return fail(cs, "main.marker-code", m.start);
        if (m.code == HV_COD) {
            if (cs->cods != 0)
                return fail(cs, "codestream.one-cod-before-sot", m.start);
            if (!jpx) {
                /* SCod (one byte), progression (one), layers (two), MCT
                 * (one): T.800 Tables A.12-A.14. Do not decode SPcod. */
                enum { MCT_OFFSET = 4 };
                if (m.end - m.payload <= MCT_OFFSET || buf[m.payload + MCT_OFFSET] > 1)
                    return fail(cs, "invalid COD", m.start);
                *mct = buf[m.payload + MCT_OFFSET];
                if ((error = hv_rule_siz_header(&cs->siz, *mct)) != NULL)
                    return fail(cs, error, m.start);
            }
            cs->cods++;
        } else if (m.code == HV_QCD) {
            if (cs->qcds != 0)
                return fail(cs, "codestream.one-qcd-before-sot", m.start);
            cs->qcds++;
        }
        cs->pos = m.end;
    }
}

static int main_segment(hv_codestream *cs, uint16_t code, size_t end, hv_item *item) {
    size_t pos = cs->pos, len = end - pos - MARKER, body = pos + MARKER + HV_FIXED(SegmentLength);
    const char *error;
    MainMarkerCode_Profile profile_code = code;
    int err;
    const marker_info *info = marker(code);

    if (profile_headers(cs) && !MainMarkerCode_Profile_IsConstraintValid(&profile_code, &err))
        return fail(cs, "main.marker-code", pos);   /* MainMarkerCode-Profile */
    if (info->properties & MAIN_FORBIDDEN)
        return fail(cs, "main.marker-code", pos);
    if (info->properties & MAIN_LAYOUT)
        cs->layout_override = 1;
    switch (code) {
    case HV_COD:
        if (cs->cods > 0)
            return fail(cs, "codestream.one-cod-before-sot", pos);
        if (decode_cod(cs, &cs->cod, end, item) != 0)
            return -1;
        break;
    case HV_QCD:
        if (cs->qcds > 0)
            return fail(cs, "codestream.one-qcd-before-sot", pos);
        if (!packets_only(cs) && !profile_full(cs)) {
            if (HV_DECODE(QcdSegment, &cs->qcd, cs->buf, pos + MARKER, len) != 0)
                return fail(cs, "invalid QCD", pos);
            item->qcd = &cs->qcd;
        }
        break;
    case HV_COM:
        if (!packets_only(cs) && !profile_full(cs)) {
            if (decode_com(cs, pos, end) != 0)
                return -1;
            item->com = &cs->com;
        }
        break;
    default:
        break;                        /* unknown: reported and skipped */
    }
    if (!packets_only(cs) && info->decode_error != NULL &&
        (error = hv_segments_main(&cs->segments, code, code == HV_COD ? &cs->cod.body : NULL,
                                  code == HV_QCD ? &cs->qcd.body : NULL, cs->buf + body,
                                  end - body)) != NULL)
        return segment_fail(cs, error, code, pos);
    cs->cods += code == HV_COD;       /* the accessors give them from here */
    cs->qcds += code == HV_QCD;
    item->kind = HV_SEGMENT;
    item->code = code;
    item->start = pos;
    item->end = end;
    cs->pos = end;
    return 1;
}

static int tile_segment(hv_codestream *cs, uint16_t code, size_t end, hv_item *item) {
    size_t pos = cs->pos, len = end - pos - MARKER, body = pos + MARKER + HV_FIXED(SegmentLength);
    const char *error;
    TileMarkerCode_Profile profile_code = code;
    int err;
    const marker_info *info = marker(code);

    /* A header that runs into the next tile-part or the end gets the same
     * message with every flag. */
    if (code == HV_SOT || code == HV_EOC)
        return fail(cs, "tile-part header without SOD", pos);
    if (profile_full(cs) && !TileMarkerCode_Profile_IsConstraintValid(&profile_code, &err))
        return fail(cs, "tile.marker-code", pos);   /* TileMarkerCode-Profile */
    if (info->properties & TILE_FORBIDDEN)
        return fail(cs, "tile.marker-code", pos);
    if (!packets_only(cs)) {
        switch (code) {
        case HV_COD:
            if (cs->sot.tpsot != 0 || cs->tp_cod++)
                return fail(cs, "tile.cod-once", pos);
            if (decode_cod(cs, &cs->tile_cod, end, item) != 0)
                return -1;
            break;
        case HV_QCD:
            if (cs->sot.tpsot != 0 || cs->tp_qcd++)
                return fail(cs, "tile.qcd-once", pos);
            if (HV_DECODE(QcdSegment, &cs->tile_qcd, cs->buf, pos + MARKER, len) != 0)
                return fail(cs, "invalid QCD", pos);
            item->qcd = &cs->tile_qcd;
            break;
        case HV_PLT: {
            size_t used;
            if (HV_DECODE_USED(Zplt, &cs->plt.zplt, cs->buf, body, end - body, &used) != 0 ||
                body + used == end)
                return fail(cs, "invalid PLT", pos);
            cs->plt.start = body + used;
            cs->plt.end = end;
            cs->plts++;
            if (cs->mode != HV_READ_JPIP_INDEX) {
                uint64_t v;
                int status;
                if ((error = hv_plt_begin(&cs->plt_reader, &cs->plt)) != NULL)
                    return fail(cs, error, pos);
                while ((status = hv_plt_read(&cs->plt_reader, cs->buf, &v)) == 1)
                    ;
                if (status < 0)
                    return fail(cs, cs->plt_reader.error, pos);
            }
            item->plt = &cs->plt;
            break;
        }
        case HV_COM:
            if (decode_com(cs, pos, end) != 0)
                return -1;
            item->com = &cs->com;
            break;
        default:
            break;
        }
        if (info->properties & TILE_LAYOUT)
            cs->layout_override = 1;
        if (info->decode_error != NULL &&
            (error = hv_segments_tile(&cs->segments, code, code == HV_COD ? &cs->tile_cod.body : NULL,
                                      code == HV_QCD ? &cs->tile_qcd.body : NULL, cs->buf + body,
                                      end - body)) != NULL)
            return segment_fail(cs, error, code, pos);
    }
    item->kind = HV_TILE_SEGMENT;
    item->code = code;
    item->start = pos;
    item->end = end;
    item->sot = cs->sot;
    cs->pos = end;
    return 1;
}

/* Outside the profile, the packet count rule applies where the main COD
 * alone lays the packets out and every one is listed (the end of
 * ../spec/j2k-codestream.asn1): a single tile, zero image and tile origins,
 * unit sampling, no layout override, and PLT in every tile-part. Not with
 * HV_READ_PADDED, which counts no entries. */
static int packets_listed(const hv_codestream *cs) {
    const SizFixed *f = cs->siz.fixed;
    size_t i;
    if (cs->plt_reader.mode == HV_PLT_PADDING || cs->part_without_plt || cs->layout_override ||
        f->xosiz != 0 || f->yosiz != 0 ||     /* and so the tile origin (siz.tile-origin) */
        f->xtsiz < f->xsiz || f->ytsiz < f->ysiz)
        return 0;
    for (i = 0; i < cs->siz.ncomponents; i++)
        if (cs->siz.components[i].xrsiz != 1 || cs->siz.components[i].yrsiz != 1)
            return 0;
    return 1;
}

int hv_codestream_next(hv_codestream *cs, hv_item *item) {
    uint16_t code;
    size_t end;
    const char *error;

    memset(item, 0, sizeof *item);
    switch (cs->state) {
    case ST_DONE:
        return cs->error ? -1 : 0;

    case ST_SIZ:
        item->kind = HV_SEGMENT;
        item->code = HV_SIZ;
        item->start = cs->pos;
        item->end = cs->siz_end;
        item->siz = &cs->siz;
        cs->pos = item->end;
        cs->state = ST_MAIN;
        return 1;

    case ST_MAIN:
    case ST_AFTER_DATA:
        if (read_segment(cs, cs->end, &code, &end) != 0)
            return -1;
        if (code == HV_SOT)
            return start_tile_part(cs, item);
        if (code == HV_EOC) {
            if (cs->tile_parts == 0)
                return fail(cs, "codestream.no-tile-part", cs->pos);
            if (end != cs->end)
                return fail(cs, "codestream.extent", end);
            if ((error = hv_rule_tile_parts_end(cs->unfinished)) != NULL ||
                (!packets_only(cs) && (error = hv_segments_end(&cs->segments)) != NULL))
                return fail(cs, error, cs->pos);
            if (!packets_only(cs) && cs->mode != HV_READ_JPIP_INDEX && (profile_full(cs) || packets_listed(cs)) &&
                (error = hv_rule_plt_packets(&cs->plt_reader.count,
                    hv_rule_packets(&cs->siz, &cs->cod.body.sgcod, &cs->cod.body.spcod),
                    profile_full(cs))) != NULL)
                return fail(cs, error, cs->pos);
            item->kind = HV_END;
            item->code = HV_EOC;
            item->start = cs->pos;
            item->end = end;
            cs->pos = end;
            cs->state = ST_DONE;
            return 1;
        }
        if (cs->state == ST_AFTER_DATA)
            return fail(cs, "codestream.segment-after-sot", cs->pos);
        return main_segment(cs, code, end, item);

    case ST_TILE_HEADER:
        if (read_segment(cs, cs->tp_end, &code, &end) != 0)
            return -1;
        if (code == HV_SOD) {
            if (profile_full(cs) && cs->plts == 0)
                return fail(cs, "codestream.no-plt", cs->tp_start);
            cs->part_without_plt |= cs->plts == 0;
            /* The PLT coverage, and at the standard layer the rules at the
             * end of a tile-part header (hv_segments_data); zero entries
             * HV_READ_PADDED has accepted are not passed on. */
            if (!packets_only(cs) && cs->mode != HV_READ_JPIP_INDEX && (error = hv_segments_data(
                     &cs->segments, cs->buf + end, cs->tp_end - end, (unsigned)cs->plts,
                     cs->plt_reader.sum,
                     cs->plt_reader.mode == HV_PLT_PADDING ? 0 : cs->plt_reader.zeros)) !=
                NULL)
                return fail(cs, error, cs->tp_start);
            item->kind = HV_TILE_DATA;
            item->code = 0;
            item->plt_padding = cs->plt_reader.zeros;
            item->start = end;
            item->end = cs->tp_end;
            item->sot = cs->sot;
            cs->pos = cs->tp_end;
            cs->state = ST_AFTER_DATA;
            return 1;
        }
        return tile_segment(cs, code, end, item);
    }
    return fail(cs, "internal error", cs->pos);
}

/* ------------------------------------------------------------------------
 * Independent JPX layers and codestream registration (T.801 M.11)
 * ------------------------------------------------------------------------ */

static uint16_t field_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

const char *hv_registration_read(const hv_registration *r, size_t index,
                                 hv_registration_entry *entry) {
    hv_registration_entry value;
    if (index >= r->count)
        return "registration index out of range";
    if (r->entries == NULL) {
        value = (hv_registration_entry){r->implicit_codestream, 1, 1, 0, 0};
    } else {
        const uint8_t *p = r->entries + index * 6;
        value = (hv_registration_entry){field_u16(p), p[2], p[3], p[4], p[5]};
    }
    *entry = value;
    return NULL;
}

const char *hv_registration_open(const uint8_t *buf, const hv_box *box,
                                 hv_registration *registration, size_t *at) {
    hv_registration value = {0};
    size_t length = box->end - box->payload;
    const char *error = NULL;
    if (box->type != HV_BOX_CREG || length < 10 || (length - 4) % 6 != 0)
        error = "creg.extent";
    else {
        value.denominator_x = field_u16(buf + box->payload);
        value.denominator_y = field_u16(buf + box->payload + 2);
        value.entries = buf + box->payload + 4;
        value.count = (length - 4) / 6;
        if (!value.denominator_x || !value.denominator_y)
            error = "creg.denominator";
        for (size_t i = 0; error == NULL && i < value.count; i++) {
            hv_registration_entry entry;
            hv_registration_read(&value, i, &entry);
            if (!entry.sampling_x || !entry.sampling_y)
                error = "creg.sampling";
            else if (entry.alignment_x >= value.denominator_x ||
                     entry.alignment_y >= value.denominator_y)
                error = "creg.alignment";
            if (error != NULL) {
                *at = box->payload + 4 + i * 6;
                return error;
            }
        }
    }
    if (error != NULL)
        *at = box->start;
    else
        *registration = value;
    return error;
}

const char *hv_component_mapping_open(const uint8_t *buf, const hv_box *box,
                                      hv_component_mapping *mapping, size_t *at) {
    if (box->type != HV_BOX_CMAP || box->end <= box->payload ||
        (box->end - box->payload) % 4 != 0) {
        *at = box->start;
        return "cmap.extent";
    }
    *mapping = (hv_component_mapping){buf + box->payload, (box->end - box->payload) / 4};
    return NULL;
}

const char *hv_component_mapping_read(const hv_component_mapping *mapping, size_t index,
                                      hv_component_mapping_entry *entry) {
    if (index >= mapping->count)
        return "component mapping index out of range";
    const uint8_t *p = mapping->entries + index * 4;
    *entry = (hv_component_mapping_entry){field_u16(p), p[2], p[3]};
    return NULL;
}

const char *hv_channel_definition_open(const uint8_t *buf, const hv_box *box,
                                       hv_channel_definition *definition, size_t *at) {
    if (box->type != HV_BOX_CDEF || box->end < box->payload ||
        box->end - box->payload < 2) {
        *at = box->start;
        return "cdef.extent";
    }
    const uint8_t *p = buf + box->payload;
    size_t count = field_u16(p);
    if (!count || box->end - box->payload - 2 != count * 6) {
        *at = box->start;
        return "cdef.extent";
    }
    *definition = (hv_channel_definition){p + 2, count};
    return NULL;
}

const char *hv_channel_definition_read(const hv_channel_definition *definition, size_t index,
                                       hv_channel_definition_entry *entry) {
    if (index >= definition->count)
        return "channel definition index out of range";
    const uint8_t *p = definition->entries + index * 6;
    *entry = (hv_channel_definition_entry){field_u16(p), field_u16(p + 2),
                                           field_u16(p + 4)};
    return NULL;
}

const char *hv_colour_read(const uint8_t *buf, const hv_box *box,
                           hv_colour *colour, size_t *at) {
    if (box->type != HV_BOX_COLR || box->end < box->payload ||
        box->end - box->payload < 3) {
        *at = box->start;
        return "colr.extent";
    }
    const uint8_t *p = buf + box->payload;
    size_t used = p[0] == 1 ? 7 : 3;
    if (box->end - box->payload < used) {
        *at = box->start;
        return "colr.extent";
    }
    hv_colour value = {p[0], p[2], p[1] < 128 ? p[1] : (int)p[1] - 256,
                       0, p + used, box->end - box->payload - used};
    if (value.method == 1)
        value.colour_space = (uint32_t)field_u16(p + 3) << 16 | field_u16(p + 5);
    *colour = value;
    return NULL;
}

const char *hv_palette_open(const uint8_t *buf, const hv_box *box,
                            hv_palette *palette, size_t *at) {
    const char *error = NULL;
    hv_palette value = {0};
    if (box->type != HV_BOX_PCLR || box->end < box->payload ||
        box->end - box->payload < 3)
        error = "pclr: no NE and NPC";
    else {
        const uint8_t *p = buf + box->payload;
        size_t size = box->end - box->payload;
        value.entry_count = field_u16(p);
        value.column_count = p[2];
        if (!value.entry_count || value.entry_count > 1024 || !value.column_count ||
            size - 3 < value.column_count)
            error = "pclr: NE or NPC out of range";
        else {
            value.depths = p + 3;
            value.values = value.depths + value.column_count;
            for (size_t c = 0; c < value.column_count; c++) {
                unsigned bits = (value.depths[c] & 127u) + 1;
                if (bits > 38) {
                    *at = box->payload + 3 + c;
                    return "pclr: bit depth out of range";
                }
                value.column_offsets[c] = (uint16_t)value.row_bytes;
                value.row_bytes += (bits + 7) / 8;
            }
            /* At most 1024 rows of 255 five-byte columns. */
            if (size - 3 - value.column_count != value.entry_count * value.row_bytes)
                error = "pclr.entries-length";
        }
    }
    if (error) *at = box->start;
    else *palette = value;
    return error;
}

const char *hv_palette_read(const hv_palette *palette, size_t entry, size_t column,
                            hv_palette_sample *sample) {
    if (entry >= palette->entry_count || column >= palette->column_count)
        return "palette index out of range";
    size_t offset = entry * palette->row_bytes + palette->column_offsets[column];
    unsigned bits = (palette->depths[column] & 127u) + 1;
    uint64_t raw = 0;
    for (unsigned b = 0; b < (bits + 7) / 8; b++)
        raw = raw << 8 | palette->values[offset + b];
    raw &= (UINT64_C(1) << bits) - 1;
    int is_signed = (palette->depths[column] & 128u) != 0;
    int64_t value = (int64_t)raw;
    if (is_signed && (raw & (UINT64_C(1) << (bits - 1))))
        value -= INT64_C(1) << bits;
    *sample = (hv_palette_sample){value, bits, is_signed};
    return NULL;
}

void hv_presentation_free(hv_presentation *p) {
    free(p->codestream_headers);
    free(p->layer);
    memset(p, 0, sizeof *p);
}

static const char *presentation_open(const uint8_t *buf, size_t size, size_t detached_streams,
                                      hv_presentation *presentation, size_t *at) {
    hv_presentation value = {0};
    hv_boxes boxes, first, children;
    hv_ftyp ftyp;
    hv_box box, child;
    size_t pos = 0, headers = 0, explicit_layers = 0, cs = 0, layer = 0;
    const char *error = hv_container_open(buf, size, &boxes, &ftyp, &pos);
    int status, jpx;
    if (error != NULL)
        goto fail;
    jpx = hv_ftyp_is_jpx(&ftyp);
    first = boxes;
    while ((status = hv_boxes_next(&boxes, &box, &error, &pos)) == 1) {
        pos = box.start;
        switch (box.type) {
        case HV_BOX_J2CX:
        case HV_BOX_JCLX:
            error = "unsupported JPX codestream or repeated container";
            goto fail;
        case HV_BOX_JP2C:
        case HV_BOX_FTBL:
            value.codestreams++;
            break;
        case HV_BOX_JPCH:
            headers++;
            break;
        case HV_BOX_JPLH:
            explicit_layers++;
            break;
        case HV_BOX_JP2H:
            if (value.defaults.type) {
                error = "duplicate presentation defaults";
                goto fail;
            }
            value.defaults = box;
            break;
        }
    }
    if (status < 0)
        goto fail;
    pos = size;
    if (detached_streams) {
        if (value.codestreams) {
            error = "detached presentation contains codestream sources";
            goto fail;
        }
        value.codestreams = detached_streams;
    }
    if (!value.codestreams || headers > value.codestreams ||
        (!jpx && (value.codestreams != 1 || explicit_layers || headers))) {
        error = "presentation header or codestream count";
        goto fail;
    }
    value.is_jpx = jpx;
    value.layers = explicit_layers ? explicit_layers : value.codestreams;
    if (value.codestreams > SIZE_MAX / sizeof *value.codestream_headers ||
        value.layers > SIZE_MAX / sizeof *value.layer) {
        error = "presentation allocation size";
        goto fail;
    }
    value.codestream_headers = calloc(value.codestreams, sizeof *value.codestream_headers);
    value.layer = calloc(value.layers, sizeof *value.layer);
    if (!value.codestream_headers || !value.layer) {
        error = "out of memory";
        goto fail;
    }
    for (size_t i = 0; i < value.layers; i++)
        value.layer[i].registration = (hv_registration){NULL, 1, i, 1, 1};
    boxes = first;
    while ((status = hv_boxes_next(&boxes, &box, &error, &pos)) == 1) {
        hv_registration *registration = NULL;
        int have_registration = 0;
        if (box.type == HV_BOX_JPCH)
            value.codestream_headers[cs++] = box;
        else if (box.type == HV_BOX_JPLH) {
            value.layer[layer].header = box;
            registration = &value.layer[layer++].registration;
        } else if (box.type != HV_BOX_JP2H)
            continue;
        hv_boxes_children(&children, buf, &box);
        while ((status = hv_boxes_next(&children, &child, &error, &pos)) == 1) {
            if (registration && child.type == HV_BOX_CREG) {
                if (have_registration) {
                    pos = child.start;
                    error = "duplicate layer registration";
                    goto fail;
                }
                error = hv_registration_open(buf, &child, registration, &pos);
                if (error != NULL)
                    goto fail;
                have_registration = 1;
            }
        }
        if (status < 0)
            goto fail;
    }
    if (status < 0)
        goto fail;
    for (size_t i = 0; i < value.layers; i++) {
        const hv_registration *registration = &value.layer[i].registration;
        for (size_t n = 0; n < registration->count; n++) {
            hv_registration_entry entry;
            hv_registration_read(registration, n, &entry);
            if (entry.codestream >= value.codestreams) {
                pos = value.layer[i].header.type ? value.layer[i].header.start : size;
                error = "layer codestream reference out of range";
                goto fail;
            }
        }
    }
    *presentation = value;
    return NULL;
fail:
    hv_presentation_free(&value);
    *at = pos;
    return error;
}

const char *hv_presentation_open(const uint8_t *buf, size_t size,
                                 hv_presentation *presentation, size_t *at) {
    return presentation_open(buf, size, 0, presentation, at);
}

const char *hv_presentation_open_headers(const uint8_t *buf, size_t size, size_t codestreams,
                                         hv_presentation *presentation, size_t *at) {
    if (!codestreams) { *at = 0; return "presentation has no codestreams"; }
    return presentation_open(buf, size, codestreams, presentation, at);
}

static const char *presentation_reference(const uint8_t *buf, const hv_box *box,
                                           uint32_t type, size_t *at) {
    CrefType target;
    if (box->type != HV_BOX_CREF)
        return NULL;
    if (HV_DECODE(CrefType, &target, buf, box->payload, box->end - box->payload) != 0) {
        *at = box->start;
        return "cref: missing target type";
    }
    if (target == type) {
        *at = box->start;
        return "unsupported presentation cross-reference";
    }
    return NULL;
}

/* Find only the requested semantic box; other payloads stay opaque. */
static const char *presentation_box(const uint8_t *buf, const hv_box *header,
                                     uint32_t type, hv_box *found, size_t *at) {
    hv_boxes children;
    hv_box child, value = {0};
    const char *error;
    int status;
    if (!header->type) {
        *found = value;
        return NULL;
    }
    hv_boxes_children(&children, buf, header);
    while ((status = hv_boxes_next(&children, &child, &error, at)) == 1) {
        if (child.type == type) {
            if (value.type) {
                *at = child.start;
                return "duplicate presentation box";
            }
            value = child;
        } else if ((error = presentation_reference(buf, &child, type, at)) != NULL)
            return error;
    }
    if (status < 0)
        return error;
    *found = value;
    return NULL;
}

static const char *presentation_override(const uint8_t *buf, const hv_box *own,
                                          const hv_box *defaults, uint32_t type,
                                          hv_box *found, size_t *at) {
    const char *error = presentation_box(buf, own, type, found, at);
    if (error == NULL && !found->type)
        error = presentation_box(buf, defaults, type, found, at);
    return error;
}

const char *hv_presentation_read_headers(const uint8_t *buf, const hv_presentation *p,
                                         size_t layer, size_t source,
                                         hv_presentation_headers *headers, size_t *at) {
    hv_presentation_headers value = {0};
    hv_registration_entry entry;
    const hv_box *own, *defaults = &p->defaults;
    const char *error;
    size_t pos = 0;
    if (layer >= p->layers) {
        error = "presentation layer index out of range";
        goto fail;
    }
    error = hv_registration_read(&p->layer[layer].registration, source, &entry);
    if (error != NULL)
        goto fail;
    value.codestream = entry.codestream;
    own = &p->codestream_headers[entry.codestream];
    error = presentation_override(buf, own, defaults, HV_BOX_PCLR, &value.palette, &pos);
    if (error != NULL)
        goto fail;
    error = presentation_override(buf, own, defaults, HV_BOX_CMAP, &value.mapping, &pos);
    if (error != NULL)
        goto fail;
    own = &p->layer[layer].header;
    error = presentation_box(buf, own, HV_BOX_CGRP, &value.colour, &pos);
    if (error != NULL)
        goto fail;
    if (!value.colour.type)
        value.colour = *defaults;
    if (value.colour.type) {
        hv_boxes colours;
        hv_box colour;
        int status;
        /* Bound the chosen colour list without selecting or decoding methods. */
        hv_boxes_children(&colours, buf, &value.colour);
        while ((status = hv_boxes_next(&colours, &colour, &error, &pos)) == 1)
            if ((error = presentation_reference(buf, &colour, HV_BOX_COLR, &pos)) != NULL)
                goto fail;
        if (status < 0)
            goto fail;
    }
    error = presentation_box(buf, own, HV_BOX_CDEF, &value.channels, &pos);
    if (error != NULL)
        goto fail;
    error = presentation_box(buf, own, HV_BOX_OPCT, &value.opacity, &pos);
    if (error != NULL)
        goto fail;
    if (!value.channels.type && !value.opacity.type) {
        error = presentation_box(buf, defaults, HV_BOX_CDEF, &value.channels, &pos);
        if (error != NULL)
            goto fail;
    }
    if (value.channels.type && value.opacity.type) {
        pos = own->start;
        error = "presentation has both channel definition and opacity";
        goto fail;
    }
    error = presentation_override(buf, own, defaults, HV_BOX_PXFM, &value.pixel_format, &pos);
    if (error != NULL)
        goto fail;
    *headers = value;
    return NULL;
fail:
    *at = pos;
    return error;
}
