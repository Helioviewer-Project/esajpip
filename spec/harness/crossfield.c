/* crossfield.c — instantiates crossfield_impl.h for both struct families,
 * checks a file at layer 1, as a JP2 file (Jp2File) or a JPX file
 * (Jp2Family): its box tree (hv_rule_box_tree, on the file's bytes, at
 * every level) and its JP2 header boxes, which only layer 1 types; and
 * checks a .jp2 at layer 2, whose box types differ (Jp2File_Profile). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "asn1crt_encoding.h"
#include "crossfield.h"

/* Tile-part counts per tile, for hv_rule_tile_part: Isot addresses at most
 * 65,535 tiles. */
static hv_tile_count cf_tile_counts[65535];

/* Each component's state for hv_segments: Csiz is at most 16,384. */
static hv_component cf_components[16384];

/* Whether the file being checked has a top-level j2cx or jclx box (layer
 * 1; hv_rule_box_tree counts them), for hv_rule_jpx. */
static int cf_extensions;

/* T.800 I.5.1: the signature box's contents. */
static int cf_signature(const OpaqueBox *box) {
    return box->data.nCount == 4 && memcmp(box->data.arr, "\x0D\x0A\x87\x0A", 4) == 0;
}

/* The File Type box as hv_rules.h reads one. */
static hv_ftyp cf_ftyp_view(const Ftyp *ftyp) {
    hv_ftyp f;
    int i;
    memset(&f, 0, sizeof f);
    f.brand = ftyp->header.brand;
    f.minor = ftyp->header.minor;
    for (i = 0; i < ftyp->compat.nCount; ++i) {
        f.jp2 |= ftyp->compat.arr[i] == HV_BRAND_JP2;
        f.jpx |= ftyp->compat.arr[i] == HV_BRAND_JPX;
        f.jpxb |= ftyp->compat.arr[i] == HV_BRAND_JPXB;
    }
    return f;
}

/* T.800 I.5.2 / T.801 M.8: the brand and the compatibility list for the
 * file's kind (hv_rule_ftyp). */
static const char *cf_ftyp(const Ftyp *ftyp, cf_kind kind, cf_layer layer) {
    hv_ftyp f = cf_ftyp_view(ftyp);
    return hv_rule_ftyp(&f, kind == CF_JPX, layer >= CF_PROFILE);
}

/* Layer-1 family: boxes have `other`; codestream markers do not. */
#define CF_S
#define CF_FILE Jp2Family
#define CF_FN cf_check_family_
#include "crossfield_impl.h"
#undef CF_S
#undef CF_FILE
#undef CF_FN

/* Layer-2 family for a .jpx: *_Profile types, whose codestream has the
 * `other` marker alternative and whose box tree shares the standard
 * layer's `other` boxes. */
#define CF_S _Profile
#define CF_FILE JpxFile_Profile
#define CF_FN cf_check_profile_common_
#define CF_COD Cod_Profile
#define CF_MAIN_OTHER
#define CF_TILE_PLT_ONLY     /* TileSegment-Profile has the plt alternative only */
#include "crossfield_impl.h"
#undef CF_S
#undef CF_FILE
#undef CF_FN
#undef CF_COD
#undef CF_MAIN_OTHER
#undef CF_TILE_PLT_ONLY

/* ------------------------------------------------------------------------
 * JP2 header boxes (layer 1)
 * ------------------------------------------------------------------------ */

/* A child of a header box, of a Jp2Family (InnerBox) or of a Jp2File
 * (Jp2HeaderBox): its type as hv_rule_header_child distinguishes them (1:
 * any other), and its decoded contents. */
typedef struct {
    uint32_t type;
    const Ihdr *ihdr;
    const Bpcc *bpcc;
    const Colr *colr;
    const Pclr *pclr;
    const Cmap *cmap;
    const Cdef *cdef;
    const Res *res;
    const Cgrp *cgrp;
} cf_child;

static cf_child cf_inner_child(const InnerBox *b) {
    cf_child c;
    const InnerPayload *p = &b->payload;
    memset(&c, 0, sizeof c);
    switch (p->kind) {
        case InnerPayload_ihdr_PRESENT: c.type = HV_BOX_IHDR; c.ihdr = &p->u.ihdr; break;
        case InnerPayload_bpcc_PRESENT: c.type = HV_BOX_BPCC; c.bpcc = &p->u.bpcc; break;
        case InnerPayload_colr_PRESENT: c.type = HV_BOX_COLR; c.colr = &p->u.colr; break;
        case InnerPayload_pclr_PRESENT: c.type = HV_BOX_PCLR; c.pclr = &p->u.pclr; break;
        case InnerPayload_cmap_PRESENT: c.type = HV_BOX_CMAP; c.cmap = &p->u.cmap; break;
        case InnerPayload_cdef_PRESENT: c.type = HV_BOX_CDEF; c.cdef = &p->u.cdef; break;
        case InnerPayload_res_PRESENT:  c.type = HV_BOX_RES;  c.res = &p->u.res;   break;
        case InnerPayload_cgrp_PRESENT: c.type = HV_BOX_CGRP; c.cgrp = &p->u.cgrp; break;
        case InnerPayload_jp2i_PRESENT: c.type = HV_BOX_JP2I; break;
        case InnerPayload_creg_PRESENT: c.type = HV_BOX_CREG; break;
        default:                        c.type = 1; break;
    }
    return c;
}

static cf_child cf_jp2_child(const Jp2HeaderBox *b) {
    cf_child c;
    const Jp2HeaderPayload *p = &b->payload;
    memset(&c, 0, sizeof c);
    switch (p->kind) {
        case Jp2HeaderPayload_ihdr_PRESENT: c.type = HV_BOX_IHDR; c.ihdr = &p->u.ihdr; break;
        case Jp2HeaderPayload_bpcc_PRESENT: c.type = HV_BOX_BPCC; c.bpcc = &p->u.bpcc; break;
        case Jp2HeaderPayload_colr_PRESENT: c.type = HV_BOX_COLR; c.colr = &p->u.colr; break;
        case Jp2HeaderPayload_pclr_PRESENT: c.type = HV_BOX_PCLR; c.pclr = &p->u.pclr; break;
        case Jp2HeaderPayload_cmap_PRESENT: c.type = HV_BOX_CMAP; c.cmap = &p->u.cmap; break;
        case Jp2HeaderPayload_cdef_PRESENT: c.type = HV_BOX_CDEF; c.cdef = &p->u.cdef; break;
        case Jp2HeaderPayload_res_PRESENT:  c.type = HV_BOX_RES;  c.res = &p->u.res;   break;
        default:                            c.type = 1; break;
    }
    return c;
}

enum { CF_CHILDREN = sizeof ((Superbox *) 0)->children.arr / sizeof (InnerBox) };
_Static_assert(sizeof ((Jp2Header *) 0)->children.arr / sizeof (Jp2HeaderBox) == CF_CHILDREN,
               "Superbox and Jp2Header have the same bound");

/* A header box's state for hv_rules.c: hv_header, and the entries of its
 * first bpcc, cmap and cdef as the box holds them, where hv_rules.c reads
 * them. */
typedef struct {
    hv_header h;
    uint8_t bpcc[sizeof ((Bpcc *) 0)->depths.arr / sizeof ((Bpcc *) 0)->depths.arr[0]];
    uint8_t cmap[sizeof ((Cmap *) 0)->entries.arr / sizeof (CmapEntry) * 4];
    uint8_t cdef[sizeof ((Cdef *) 0)->entries.arr / sizeof (CdefEntry) * 6];
} cf_header_state;

/* An entry as the box holds it, from the model's encoder. */
#define CF_ENCODE(T, value, out, size)                                          \
    do {                                                                        \
        BitStream bs_;                                                          \
        int err_ = 0;                                                           \
        BitStream_Init(&bs_, (out), (size));                                    \
        if (!T##_ACN_Encode((value), &bs_, &err_, 0)) abort();                  \
    } while (0)

/* One header box (jp2h, jpch or jplh), child by child (hv_rules.c). jp2:
 * a JPX file's jp2h read as a JP2 file's too (T.801 M.3.6). */
static const char *cf_header(const cf_child *children, int n, uint32_t parent, cf_kind kind,
                             int jp2, cf_header_state *state) {
    hv_header *h = &state->h;
    int i, j;
    const char *r;
    hv_header_init(h, parent, kind == CF_JPX);
    h->jp2 |= jp2;
    for (i = 0; i < n; ++i) {
        const cf_child *c = &children[i];
        r = hv_rule_header_child(h, c->type);
        if (r == NULL && c->ihdr) r = hv_rule_ihdr(h, c->ihdr);
        if (r == NULL && c->bpcc) {
            /* The first bpcc's entries stay in state; a later one's
             * (header.one-bpcc) never reach here. */
            for (j = 0; j < c->bpcc->depths.nCount; ++j)
                state->bpcc[j] = (uint8_t) c->bpcc->depths.arr[j];
            hv_rule_bpcc(h, state->bpcc, (size_t) c->bpcc->depths.nCount);
        }
        if (r == NULL && c->colr)
            r = hv_rule_colr(h, &c->colr->header, c->colr->rest.arr,
                             (size_t) c->colr->rest.nCount);
        if (r == NULL && c->pclr) {
            const PclrHeader *ph = &c->pclr->header;
            hv_rule_pclr(h, ph->ne, (uint64_t) ph->depths.nCount);
            for (j = 0; j < ph->depths.nCount; ++j)
                hv_rule_pclr_column(h, ph->depths.arr[j]);
            r = hv_rule_pclr_end(h, c->pclr->entries.arr, (uint64_t) c->pclr->entries.nCount);
        }
        if (r == NULL && c->cmap) {
            for (j = 0; j < c->cmap->entries.nCount && r == NULL; ++j) {
                if (h->cmap == 1)
                    CF_ENCODE(CmapEntry, &c->cmap->entries.arr[j], state->cmap + 4 * j, 4);
                r = hv_rule_cmap_entry(h, &c->cmap->entries.arr[j]);
            }
            if (r == NULL) hv_rule_cmap_end(h, state->cmap, (size_t) c->cmap->entries.nCount);
        }
        if (r == NULL && c->cdef) {
            if (h->cdef == 1)
                for (j = 0; j < c->cdef->entries.nCount; ++j)
                    CF_ENCODE(CdefEntry, &c->cdef->entries.arr[j], state->cdef + 6 * j, 6);
            r = hv_rule_cdef(h, c->cdef->entries.arr, (size_t) c->cdef->entries.nCount,
                             state->cdef, NULL);
            if (r == NULL) r = hv_rule_extent(HV_BOX_CDEF, (uint64_t) c->cdef->extra.nCount);
        }
        if (r == NULL && c->cgrp && kind == CF_JPX) {
            /* Its colr boxes, as jp2h's (hv_rules.h); in a JP2 file, where
             * T.800 does not define it, a box to skip. */
            hv_header g;
            hv_header_init(&g, HV_BOX_CGRP, 1);
            for (j = 0; j < c->cgrp->children.nCount && r == NULL; ++j) {
                const CgrpPayload *p = &c->cgrp->children.arr[j].payload;
                int colr = p->kind == CgrpPayload_colr_PRESENT;
                hv_rule_cgrp_child(&g, colr ? HV_BOX_COLR : 1);
                if (r == NULL && colr)
                    r = hv_rule_colr(&g, &p->u.colr.header, p->u.colr.rest.arr,
                                     (size_t) p->u.colr.rest.nCount);
            }
            if (r == NULL) r = hv_rule_cgrp_end(h, &g);
        }
        if (r == NULL && c->res) {
            for (j = 0; j < c->res->children.nCount && r == NULL; ++j) {
                const ResPayload *p = &c->res->children.arr[j].payload;
                hv_rule_res_child(h, p->kind == resc_PRESENT ? HV_BOX_RESC
                                       : p->kind == resd_PRESENT ? HV_BOX_RESD : 1);
                if (r == NULL && p->kind == resc_PRESENT)
                    r = hv_rule_extent(HV_BOX_RESC, (uint64_t) p->u.resc.extra.nCount);
                if (r == NULL && p->kind == resd_PRESENT)
                    r = hv_rule_extent(HV_BOX_RESD, (uint64_t) p->u.resd.extra.nCount);
            }
            if (r == NULL) r = hv_rule_res_end(h);
        }
        if (r != NULL) return r;
    }
    return NULL;
}

/* A Jp2Family header box (jp2h, jpch, jplh). */
static const char *cf_superbox_header(const Superbox *sb, uint32_t parent, int jp2,
                                      cf_header_state *state) {
    cf_child children[CF_CHILDREN];
    int i;
    for (i = 0; i < sb->children.nCount; ++i) children[i] = cf_inner_child(&sb->children.arr[i]);
    return cf_header(children, sb->children.nCount, parent, CF_JPX, jp2, state);
}

/* A codestream's main COD's multiple component transform (-1 without a
 * COD). */
static int cf_mct(const Codestream *cs) {
    int i;
    for (i = 0; i < cs->segments.nCount; ++i)
        if (cs->segments.arr[i].exist.tilePart) break;
        else if (cs->segments.arr[i].code == HV_COD)
            return (int) cs->segments.arr[i].cod.body.sgcod.mct;
    return -1;
}

/* The box tree (hv_rule_box_tree), with room for the corpus's mdat boxes. */
static hv_extent cf_mdat[64];

static const char *cf_tree(const unsigned char *data, size_t len, int jpx, hv_box_tree *tree) {
    size_t at;
    const char *r;
    tree->mdat = cf_mdat;
    tree->mdat_cap = sizeof cf_mdat / sizeof *cf_mdat;
    if ((r = hv_rule_box_tree(data, len, jpx, tree, &at)) != NULL) return r;
    if (tree->mdat_count > tree->mdat_cap) abort();       /* a corpus bound */
    return NULL;
}

/* The JP2 rules on a JP2 file, or on a (jpx) JPX file that lists 'jp2 '
 * (T.800 I.5.2): jp2h placed, its header (read already, into jp2h)
 * against the first codestream, and in a JP2 file its IPR against the
 * file's IPR boxes (in a JPX file an ihdr's IPR is its codestreams',
 * T.801 M.11.5.1: jpx.ipr). */
static const char *cf_jp2_rules(const hv_box_tree *tree, const hv_header *jp2h,
                                const Codestream *first, int jpx) {
    hv_siz siz;
    const char *r;
    if ((r = hv_rule_jp2h_place(tree, 0, 0)) != NULL) return r;
    siz = cf_siz_view(&first->siz.body);
    if ((r = hv_rule_codestream_header(jp2h, NULL, &siz, cf_mct(first), 0)) != NULL) return r;
    return jpx ? NULL : hv_rule_ihdr_ipr(jp2h, tree->ipr);
}

/* A JPX file's header boxes, T.800 I.5.3 and T.801 M.11.5 to M.11.7, in
 * the reader's order (hv_check_jpx_headers): MinV, rreq, where jp2h is,
 * what each header box holds, each codestream's header against its SIZ
 * (a linked codestream's SIZ is in another file), creg, the baseline
 * rules where the file lists 'jpxb', and the JP2 rules where it lists
 * 'jp2 '. */
static const char *cf_jpx_headers(const Jp2Family *file, const hv_box_tree *tree) {
    static cf_header_state jp2h, jpch, jplh, first_jpch, first_jplh;
    hv_ftyp ftyp = cf_ftyp_view(&file->boxes.arr[1].payload.u.ftyp);
    const Superbox *jp2h_box = NULL;
    const Codestream *first = NULL;
    int i, j, stream = 0, jpchs = 0, jplhs = 0, cregs = 0, jpchs_read = 0;
    const char *r;

    /* T.801 M.8: MinV (the second box is ftyp, cf_check_family_). */
    if ((r = hv_rule_ftyp_minor(ftyp.minor, 1)) != NULL) return r;

    /* T.801 M.11.1: the Reader Requirements box, its count and place
     * checked by hv_rule_jpx. */
    for (i = 0; i < file->boxes.nCount; ++i) {
        const TopPayload *p = &file->boxes.arr[i].payload;
        if (p->kind != TopPayload_rreq_PRESENT) continue;
        for (j = 0; j < p->u.rreq.standard.nCount; ++j)
            if ((r = hv_rule_rreq_feature(p->u.rreq.standard.arr[j].sf)) != NULL) return r;
        if ((r = hv_rule_extent(HV_BOX_RREQ, (uint64_t) p->u.rreq.extra.nCount)) != NULL)
            return r;
    }

    for (i = 0; i < file->boxes.nCount; ++i) {
        const TopPayload *p = &file->boxes.arr[i].payload;
        if (p->kind == TopPayload_jp2h_PRESENT && jp2h_box == NULL) jp2h_box = &p->u.jp2h;
        if (p->kind == TopPayload_jpch_PRESENT) jpchs++;
        if (p->kind == TopPayload_jp2c_PRESENT && first == NULL) first = &p->u.jp2c;
    }
    if ((r = hv_rule_jp2h_place(tree, 1, ftyp.jpxb)) != NULL) return r;
    if (jp2h_box != NULL &&
        (r = cf_superbox_header(jp2h_box, HV_BOX_JP2H, ftyp.jp2, &jp2h)) != NULL)
        return r;

    /* In box order, as the reader: each jplh; codestream k against jpch k
     * (M.11.6; the counts agree, hv_rule_jpx), or only jp2h's defaults
     * when there is no jpch. */
    for (i = 0; i < file->boxes.nCount; ++i) {
        const TopPayload *p = &file->boxes.arr[i].payload;
        hv_siz view;
        const hv_siz *siz = NULL;
        const hv_header *own = NULL;
        int k, seen = 0, mct = -1;
        if (p->kind == TopPayload_jplh_PRESENT) {
            if ((r = cf_superbox_header(&p->u.jplh, HV_BOX_JPLH, 0, &jplh)) != NULL) return r;
            if (jplhs++ == 0) first_jplh = jplh;
            cregs += jplh.h.creg > 0;
            continue;
        }
        if (p->kind != TopPayload_jp2c_PRESENT && p->kind != TopPayload_ftbl_PRESENT) continue;
        if (p->kind == TopPayload_jp2c_PRESENT) {
            view = cf_siz_view(&p->u.jp2c.siz.body);
            siz = &view;
            mct = cf_mct(&p->u.jp2c);
        }
        if (jpchs > 0) {
            for (k = 0; k < file->boxes.nCount; ++k)
                if (file->boxes.arr[k].payload.kind == TopPayload_jpch_PRESENT && seen++ == stream)
                    break;
            if (k == file->boxes.nCount) break;     /* a j2cx or jclx holds the rest */
            if ((r = cf_superbox_header(&file->boxes.arr[k].payload.u.jpch, HV_BOX_JPCH, 0,
                                        &jpch)) != NULL)
                return r;
            if (jpchs_read++ == 0) first_jpch = jpch;
            own = &jpch.h;
        }
        r = hv_rule_codestream_header(own, jp2h_box ? &jp2h.h : NULL, siz, mct, 1);
        if (r == NULL) r = hv_rule_jpx_ipr(own, jp2h_box ? &jp2h.h : NULL, tree->ipr);
        if (r != NULL) return r;
        stream++;
    }
    if ((r = hv_rule_jpx_creg(jplhs, cregs)) != NULL) return r;
    if (ftyp.jpxb &&
        (r = hv_rule_jpxb_layer(jp2h_box ? &jp2h.h : NULL, jpchs_read ? &first_jpch.h : NULL,
                                jplhs ? &first_jplh.h : NULL)) != NULL)
        return r;
    return ftyp.jp2 ? cf_jp2_rules(tree, &jp2h.h, first, 1) : NULL;
}

const char *cf_check_jpx(const Jp2Family *file, const unsigned char *data, size_t len) {
    hv_box_tree tree;
    hv_boxes it;
    hv_box box;
    const char *error;
    size_t at;
    const char *r;

    /* The top-level j2cx and jclx boxes, which the model keeps opaque. */
    cf_extensions = 0;
    hv_boxes_file(&it, data, len);
    while (hv_boxes_next(&it, &box, &error, &at) == 1)
        cf_extensions |= box.type == HV_BOX_J2CX || box.type == HV_BOX_JCLX;
    if ((r = cf_check_family_(file, CF_STANDARD, CF_JPX)) != NULL) return r;
    if ((r = cf_tree(data, len, 1, &tree)) != NULL) return r;
    if ((r = hv_rule_fragments(data, len, &tree,
                               cf_ftyp_view(&file->boxes.arr[1].payload.u.ftyp).jpxb, &at)) != NULL)
        return r;
    return cf_jpx_headers(file, &tree);
}

/* ------------------------------------------------------------------------
 * A JP2 file (layer 1)
 * ------------------------------------------------------------------------ */

const char *cf_check_jp2(const Jp2File *file, const unsigned char *data, size_t len) {
    static cf_header_state jp2h;
    cf_child children[CF_CHILDREN];
    hv_box_tree tree;
    const Jp2Header *header = NULL;
    const Codestream *first = NULL;
    hv_ftyp ftyp;
    int i;
    const char *r;

    /* T.800 I.4, I.5.1, I.5.2, in the reader's order: the signature, a
     * second box, ftyp. */
    if (file->boxes.arr[0].payload.kind != Jp2Payload_jP_PRESENT ||
        !cf_signature(&file->boxes.arr[0].payload.u.jP))
        return "file.signature";
    if (file->boxes.nCount < 2) return "file.two-boxes";
    if (file->boxes.arr[1].payload.kind != Jp2Payload_ftyp_PRESENT) return "file.ftyp-second";
    ftyp = cf_ftyp_view(&file->boxes.arr[1].payload.u.ftyp);
    if ((r = hv_rule_ftyp(&ftyp, 0, 0)) != NULL) return r;
    /* The codestreams, as the template checks a Jp2Family's. */
    for (i = 0; i < file->boxes.nCount; ++i)
        if (file->boxes.arr[i].payload.kind == Jp2Payload_jp2c_PRESENT &&
            (r = cf_codestream(&file->boxes.arr[i].payload.u.jp2c, CF_STANDARD)) != NULL)
            return r;
    if ((r = cf_tree(data, len, 0, &tree)) != NULL) return r;
    if ((r = hv_rule_ftyp_minor(ftyp.minor, 0)) != NULL) return r;
    for (i = 0; i < file->boxes.nCount; ++i) {
        const Jp2Payload *p = &file->boxes.arr[i].payload;
        if (p->kind == Jp2Payload_jp2h_PRESENT && header == NULL) header = &p->u.jp2h;
        if (p->kind == Jp2Payload_jp2c_PRESENT && first == NULL) first = &p->u.jp2c;
    }
    if ((r = hv_rule_jp2h_place(&tree, 0, 0)) != NULL) return r;
    for (i = 0; i < header->children.nCount; ++i)
        children[i] = cf_jp2_child(&header->children.arr[i]);
    if ((r = cf_header(children, header->children.nCount, HV_BOX_JP2H, CF_JP2, 1, &jp2h)) != NULL)
        return r;
    return cf_jp2_rules(&tree, &jp2h.h, first, 0);
}

/* ------------------------------------------------------------------------
 * Layer 2
 * ------------------------------------------------------------------------ */

const char *cf_check_jpx_profile(const JpxFile_Profile *file) {
    cf_extensions = 0;
    return cf_check_profile_common_(file, CF_PROFILE, CF_JPX);
}

/* Layer 2 for a .jp2: ReadJP2 reads the signature, ftyp and jp2c boxes and
 * skips every other box, so the other boxes are opaque (Jp2Payload-Profile)
 * and only these rules apply. */
const char *cf_check_jp2_profile(const Jp2File_Profile *file) {
    int i, jp2c = 0;
    const char *r;
    /* The reader's order: the signature, then a second box. */
    if (file->boxes.arr[0].payload.kind != Jp2Payload_Profile_jP_PRESENT ||
        !cf_signature(&file->boxes.arr[0].payload.u.jP))
        return "file.signature";
    if (file->boxes.nCount < 2) return "file.two-boxes";
    if (file->boxes.arr[1].payload.kind != Jp2Payload_Profile_ftyp_PRESENT)
        return "file.ftyp-second";
    if ((r = cf_ftyp(&file->boxes.arr[1].payload.u.ftyp, CF_JP2, CF_PROFILE)) != NULL) return r;
    /* The count first, then the codestream, as the reader (hv_served_jp2,
     * then hv_codestream_check). */
    for (i = 0; i < file->boxes.nCount; ++i)
        jp2c += file->boxes.arr[i].payload.kind == Jp2Payload_Profile_jp2c_PRESENT;
    if (jp2c != 1) return "jp2.one-codestream";
    for (i = 0; i < file->boxes.nCount; ++i)
        if (file->boxes.arr[i].payload.kind == Jp2Payload_Profile_jp2c_PRESENT &&
            (r = cf_codestream_Profile(&file->boxes.arr[i].payload.u.jp2c, CF_PROFILE)) != NULL)
            return r;
    return NULL;
}
