/* hv_rules.c: see hv_rules.h. */
#include "hv_rules.h"
#include "hv_grid.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "hv_decode.h"
#include "jpeg2000-io.h"

HV_DEFINE_DECODE(ComponentIndex)
HV_DEFINE_DECODE(ComponentIndex_Wide)
HV_DEFINE_DECODE(CocStyle)
HV_DEFINE_DECODE(Qcd)
HV_DEFINE_DECODE(RgnStyle)
HV_DEFINE_DECODE(PocChange)
HV_DEFINE_DECODE(PocChange_Wide)
HV_DEFINE_DECODE(TlmHeader)
HV_DEFINE_DECODE(Ttlm)
HV_DEFINE_DECODE(Ttlm_Wide)
HV_DEFINE_DECODE(Ptlm)
HV_DEFINE_DECODE(Ptlm_Wide)
HV_DEFINE_DECODE(Zplm)
HV_DEFINE_DECODE(Zppm)
HV_DEFINE_DECODE(Zppt)
HV_DEFINE_DECODE(CrgEntry)
HV_DEFINE_DECODE(Iplt)
HV_DEFINE_DECODE(BoxHeader)
HV_DEFINE_DECODE(FtypHeader)
HV_DEFINE_DECODE(Brand)
HV_DEFINE_DECODE(UrlHeader)
HV_DEFINE_DECODE(DataReferenceCount)
HV_DEFINE_DECODE(UuidCount)
HV_DEFINE_DECODE(CrefType)
HV_DEFINE_DECODE(CmapEntry)
HV_DEFINE_DECODE(CdefEntry)
HV_DEFINE_DECODE(FragmentCount)
HV_DEFINE_DECODE(Fragment)

/* What a rule returns where a body is not its type's (reason `decode`). */
static const char decode_error[] = "decode";

/* Rsiz (T.800 Table A.10; T.801 Table A.2: 1x00 xxxx xxxx xxxx). */
static int part2_rsiz(uint64_t rsiz) {
    return (rsiz & 0xB000) == 0x8000;
}

/* Recognized Part 1 declarations whose profile restrictions are not
 * implemented here (T.800:2019 Tables A.10, A.49, A.50, A.53 and A.54). */
static int other_part1_profile(uint64_t rsiz) {
    unsigned main = (unsigned)(rsiz & 15), sub = (unsigned)((rsiz >> 4) & 15);
    unsigned family = (unsigned)(rsiz >> 8);
    if ((rsiz >= 3 && rsiz <= 7) || rsiz == 0x0FFF)
        return 1;
    if ((family == 1 || family == 2) && sub == 0 && main <= 11)
        return 1;
    if (rsiz == 0x0306 || rsiz == 0x0307)
        return 1;
    if (family >= 4 && family <= 9 && main <= 11 && sub <= 9)
        return main == 0 || sub <= (main <= 3 ? 1 : main - 2);
    return 0;
}

static const char *mct_geometry(const hv_siz *siz, int mct) {
    const Component *k = siz->components;
    if (mct != 0) {
        if (siz->ncomponents < 3) return "siz.mct-components";
        for (size_t i = 1; i < 3; i++)
            if (k[i].depthMinus1 != k[0].depthMinus1 || k[i].xrsiz != k[0].xrsiz ||
                k[i].yrsiz != k[0].yrsiz)
                return "siz.mct-geometry";
    }
    return NULL;
}

const char *hv_rule_siz_header(const hv_siz *siz, int mct) {
    const SizFixed *s = siz->fixed;
    if (s->csiz != siz->ncomponents) return "siz.csiz-count";
    if (!(s->xosiz < s->xsiz && s->yosiz < s->ysiz)) return "siz.origin-inside";
    return mct_geometry(siz, mct);
}

static const char *siz_layout(const hv_siz *siz, const Sgcod *sgcod, int profile) {
    const SizFixed *s = siz->fixed;
    const Component *k = siz->components;
    size_t i;
    if (!(s->xosiz < s->xsiz && s->yosiz < s->ysiz)) return "siz.origin-inside";
    if (!(s->xtosiz <= s->xosiz && s->ytosiz <= s->yosiz)) return "siz.tile-origin";
    if (!(s->xtosiz + s->xtsiz > s->xosiz && s->ytosiz + s->ytsiz > s->yosiz))
        return "siz.tile-covers-origin";
    /* B.11 requires a tile-part for every tile; Isot can name 65,535
     * tiles (Table A.5). The validated dimensions are at most 32 bits. */
    if (!profile && hv_tile_axis_count(s->xsiz, s->xtosiz, s->xtsiz) *
                    hv_tile_axis_count(s->ysiz, s->ytosiz, s->ytsiz) > 65535)
        return "siz.tile-count";
    {
        const char *error = mct_geometry(siz, sgcod != NULL ? (int)sgcod->mct : 0);
        if (error != NULL) return error;
    }
    if (profile) {
        if (s->xosiz != 0 || s->yosiz != 0 || s->xtosiz != 0 || s->ytosiz != 0)
            return "siz.zero-origin";
        for (i = 0; i < siz->ncomponents; i++)
            if (k[i].xrsiz != 1 || k[i].yrsiz != 1)
                return "siz.component-sampling";
        if (!(s->xtsiz >= s->xsiz && s->ytsiz >= s->ysiz)) return "siz.single-tile";
    }
    return NULL;
}

const char *hv_rule_rsiz(uint64_t rsiz) {
    if (rsiz <= 2 || part2_rsiz(rsiz)) return NULL;
    if (other_part1_profile(rsiz)) return "siz.unsupported-profile";
    if (rsiz & 0x4000) return "siz.unsupported-capabilities";
    return "siz.rsiz";
}

const char *hv_rule_siz(const hv_siz *siz, const Sgcod *sgcod, int profile) {
    const SizFixed *s = siz->fixed;
    if (s->csiz != siz->ncomponents) return "siz.csiz-count";
    if (!profile) {
        const char *error = hv_rule_rsiz(s->rsiz);
        if (error != NULL) return error;
    }
    return siz_layout(siz, sgcod, profile);
}

const char *hv_rule_siz_geometry(const hv_siz *siz, const Sgcod *sgcod) {
    if (siz->fixed->csiz != siz->ncomponents) return "siz.csiz-count";
    return siz_layout(siz, sgcod, 0);
}

/* SPcod or SPcoc (A.6.1, A.6.2, Table A.15): the precinct sizes, present
 * exactly with the flag, and the code-block area; the names by segment. */
static const char *spcod_rule(int custom, const Spcod *spcod, int coc) {
    int i;
    int expected = custom ? (int)spcod->levels + 1 : 0;
    if (spcod->precincts.nCount != expected)
        return coc ? "coc.precincts-count" : "cod.precincts-count";
    for (i = 1; i < spcod->precincts.nCount; i++)
        if (spcod->precincts.arr[i].ppx == 0 || spcod->precincts.arr[i].ppy == 0)
            return coc ? "coc.precincts-higher-zero" : "cod.precincts-higher-zero";
    if (spcod->cbWidthExp + spcod->cbHeightExp > 8)
        return coc ? "coc.codeblock-area" : "cod.codeblock-area";
    return NULL;
}

const char *hv_rule_cod(const Scod *scod, const Spcod *spcod, int profile) {
    const char *error = spcod_rule(scod->customPrecincts, spcod, 0);
    if (error == NULL && profile && scod->sopMarkers) return "cod.sop-markers";
    return error;
}

uint32_t hv_rule_tiles(const hv_siz *siz) {
    const SizFixed *s = siz->fixed;
    uint64_t nx, ny;
    if (s->xsiz <= s->xtosiz || s->ysiz <= s->ytosiz || s->xtsiz == 0 || s->ytsiz == 0)
        return 0;
    nx = hv_tile_axis_count(s->xsiz, s->xtosiz, s->xtsiz);
    ny = hv_tile_axis_count(s->ysiz, s->ytosiz, s->ytsiz);
    return nx > 65535 || ny > 65535 || nx * ny > 65535 ? 65535 : (uint32_t)(nx * ny);
}

const char *hv_rule_isot(uint32_t tiles, uint64_t isot) {
    return isot >= tiles ? "sot.isot-range" : NULL;
}

/* Whether TNsot gives t a count its tile-parts have not reached. */
static int unfinished_tile(const hv_tile_count *t) {
    return t->tnsot != 0 && t->parts != t->tnsot;
}

const char *hv_rule_tile_part(hv_tile_count *t, uint32_t *unfinished, uint64_t tpsot,
                              uint64_t tnsot) {
    int was = unfinished_tile(t);
    if (tpsot != t->parts) return "sot.tpsot-sequence";
    if (tnsot != 0) {
        if (tpsot >= tnsot) return "sot.tpsot-below-tnsot";
        if (t->tnsot != 0 && t->tnsot != tnsot) return "sot.tnsot-inconsistent";
        t->tnsot = (uint8_t)tnsot;
    }
    t->parts++;
    *unfinished = *unfinished - (uint32_t)was + (uint32_t)unfinished_tile(t);
    return NULL;
}

const char *hv_rule_tile_parts_end(uint32_t unfinished) {
    return unfinished != 0 ? "sot.tnsot-count" : NULL;
}

const char *hv_rule_tile_part_count(uint64_t parts) {
    return parts > HV_PROFILE_TILE_PARTS ? "codestream.tile-part-limit" : NULL;
}

const char *hv_rule_iplt(const Iplt *e, uint64_t *value) {
    uint64_t v = e->b0.bits;
#define HV_IPLT_STEP(n)                                                      \
    if (e->exist.b##n) {                                                     \
        if (v > (UINT64_MAX >> 7)) return "plt.value-overflow";              \
        v = (v << 7) | (uint64_t)e->b##n.bits;                               \
    }
    HV_IPLT_STEP(1) HV_IPLT_STEP(2) HV_IPLT_STEP(3) HV_IPLT_STEP(4) HV_IPLT_STEP(5)
    HV_IPLT_STEP(6) HV_IPLT_STEP(7) HV_IPLT_STEP(8) HV_IPLT_STEP(9)
#undef HV_IPLT_STEP
    *value = v;
    return NULL;
}

const char *hv_rule_plt_entry(hv_plt_count *count, uint64_t value, int profile) {
    if (value == 0) {
        if (profile) count->padding = 1;
    } else {
        if (count->padding) return "plt.padding-position";
        count->packets++;
    }
    return NULL;
}

const char *hv_rule_zplt(hv_zplt *z, uint64_t zplt, int profile) {
    unsigned bit = (unsigned)zplt;
    if (profile) return zplt != z->count++ ? "plt.zplt-sequence" : NULL;
    z->count++;
    if (z->seen[bit / 8] & (1u << (bit % 8))) return "plt.zplt-index";
    z->seen[bit / 8] |= (uint8_t)(1u << (bit % 8));
    return NULL;
}

const char *hv_rule_packet_length(const uint8_t *body, size_t *pos, size_t end, int profile,
                                  uint64_t *value) {
    Iplt entry;
    size_t p = *pos, used;
    const char *error;
    /* A leading 0x80 is a zero group that continues. */
    while (!profile && p + 1 < end && body[p] == 0x80)
        p++;
    if (p >= end || HV_DECODE_USED(Iplt, &entry, body, p, end - p, &used) != 0)
        return decode_error;
    if ((error = hv_rule_iplt(&entry, value)) != NULL) return error;
    *pos = p + used;
    return NULL;
}

uint64_t hv_rule_packets(const hv_siz *siz, const Sgcod *sgcod, const Spcod *spcod) {
    const SizFixed *s = siz->fixed;
    uint64_t total = 0;
    int levels = (int)spcod->levels, r;
    for (r = 0; r <= levels; r++) {
        uint64_t w = hv_resolution_coord(s->xsiz, (unsigned)(levels - r));
        uint64_t h = hv_resolution_coord(s->ysiz, (unsigned)(levels - r));
        int ppx = 15, ppy = 15;
        if (spcod->precincts.nCount != 0) {
            ppx = (int)spcod->precincts.arr[r].ppx;
            ppy = (int)spcod->precincts.arr[r].ppy;
        }
        w = hv_precinct_count(0, w, (uint64_t)1 << ppx);
        h = hv_precinct_count(0, h, (uint64_t)1 << ppy);
        total += w * h;
        if (total > 2147483647u) return 0;
    }
    total *= (uint64_t)s->csiz;
    if (total > 2147483647u) return 0;
    total *= (uint64_t)sgcod->layers;
    return total > 2147483647u ? 0 : total;
}

const char *hv_rule_plt_coverage(uint64_t sum, uint64_t data_size, uint64_t sops) {
    return sum != data_size && (sops == 0 || sum != data_size - 6 * sops)
        ? "plt.coverage" : NULL;
}

const char *hv_rule_plt_packets(const hv_plt_count *count, uint64_t packets, int profile) {
    if (packets == 0)
        return profile ? "codestream.packet-count" : NULL;
    if (profile && count->padding && count->packets < packets) return "plt.zero-length";
    if (count->packets != packets) return "plt.packet-count";
    return NULL;
}

/* ------------------------------------------------------------------------
 * The codestream's segments across headers
 * ------------------------------------------------------------------------ */

/* The quantization style of Sqcd or Sqcc (Table A.28): bits 4..0. */
static unsigned quant_style(uint64_t sqcd) {
    return (unsigned)(sqcd & 0x1F);
}

/* Sqcd or Sqcc and the n bytes of SPqcd or SPqcc (A.6.4, A.6.5, Tables
 * A.28 to A.30): a style T.800 defines; one byte per sub-band without
 * quantization, its three low bits zero (Table A.29, A.1.3); two bytes for
 * the NLLL sub-band alone (derived); two per sub-band (expounded). The
 * sub-bands are 1 + 3 x NL, NL at most 32 (Equations A-4, A-5; their NOTE
 * permits surplus entries after truncation). Coverage is checked against
 * the effective decomposition at SOD, after tile overrides. */
static const char *quant_rule(uint64_t sqcd, const uint8_t *sp, size_t n, int qcc,
                               uint8_t *coverage) {
    size_t i;
    unsigned style = quant_style(sqcd);
    switch (style) {
    case 0:
        if (n < 1 || n > 97 || (n - 1) % 3 != 0) return qcc ? "qcc.length" : "qcd.length";
        for (i = 0; i < n; i++)
            if (sp[i] & 7) return qcc ? "qcc.reserved-bits" : "qcd.reserved-bits";
        break;
    case 1:
        if (n != 2) return qcc ? "qcc.length" : "qcd.length";
        break;
    case 2:
        if (n % 2 != 0 || n / 2 < 1 || n / 2 > 97 || (n / 2 - 1) % 3 != 0)
            return qcc ? "qcc.length" : "qcd.length";
        break;
    default:
        return qcc ? "qcc.style" : "qcd.style";
    }
    *coverage = style == 1 ? 32 : (uint8_t)((n / (style == 2 ? 2 : 1) - 1) / 3);
    return NULL;
}

/* The transform class of Table A.20 (1: 5-3 reversible) against the
 * quantization style: no quantization with the reversible transform only,
 * scalar quantization with the irreversible one (the titles of Tables A.29
 * and A.30; E.1.1.1, E.1.2.1). */
static int quant_fits(unsigned transform, unsigned style) {
    return (transform == 1) == (style == 0);
}

static void set_coding(hv_coding *c, int custom, const Spcod *sp) {
    int i;
    memset(c, 0, sizeof *c);
    c->levels = (uint8_t)sp->levels;
    c->transform = (uint8_t)sp->transform;
    c->custom = (uint8_t)custom;
    for (i = 0; custom && i < sp->precincts.nCount && i < 33; i++)
        c->precincts[i] = (uint8_t)(sp->precincts.arr[i].ppy << 4 | sp->precincts.arr[i].ppx);
}

/* Table A.45 on one COD or COC's code-blocks: Profile 0, xcb = ycb = 5 or
 * 6 and a style of 00sp vtra with a = r = v = 0; Profile 1, xcb and ycb at
 * most 6 (xcb and ycb are the exponents in SPcod plus 2). */
static const char *codeblock_profile(const hv_segments *s, const Spcod *sp) {
    uint64_t w = sp->cbWidthExp, h = sp->cbHeightExp;
    if (s->rsiz_profile == 1 && (w != h || (w != 3 && w != 4) || (sp->cbStyle & 0x0B) != 0))
        return "codestream.profile-0";
    if (s->rsiz_profile == 2 && (w > 4 || h > 4)) return "codestream.profile-1";
    return NULL;
}

/* The component index at the start of a COC, QCC or RGN body, and *used
 * its bytes: -1 when it does not decode. */
static int component_index(const hv_segments *s, const uint8_t *body, size_t n, uint32_t *c,
                           size_t *used) {
    if (s->wide) {
        ComponentIndex_Wide v;
        if (HV_DECODE_USED(ComponentIndex_Wide, &v, body, 0, n, used) != 0) return -1;
        *c = (uint32_t)v;
    } else {
        ComponentIndex v;
        if (HV_DECODE_USED(ComponentIndex, &v, body, 0, n, used) != 0) return -1;
        *c = (uint32_t)v;
    }
    return 0;
}

/* Whether the current header is the main header. */
static int in_main(const hv_segments *s) {
    return s->tile == NULL;
}

/* Adds component c to the list of those the current tile-part's header
 * sets, once. */
static void touch(hv_segments *s, uint32_t c) {
    hv_component *k = &s->components[c];
    if (k->coc_header != s->header && k->qcc_header != s->header) {
        k->next = (uint16_t)s->touched;
        s->touched = c + 1;
    }
}

/* COC (A.6.2): Ccoc a component, SPcoc as SPcod, one per component in a
 * header and only in a tile's first tile-part. */
static const char *coc_segment(hv_segments *s, const uint8_t *body, size_t n) {
    CocStyle style;
    uint32_t c;
    size_t used, rest;
    hv_component *k;
    const char *error;
    if (component_index(s, body, n, &c, &used) != 0 ||
        HV_DECODE_USED(CocStyle, &style, body, used, n - used, &rest) != 0 || used + rest != n)
        return decode_error;
    if (c >= s->siz->ncomponents) return "coc.component";
    if ((error = spcod_rule(style.scoc.customPrecincts, &style.spcoc, 1)) != NULL) return error;
    k = &s->components[c];
    if (k->coc_header == s->header || (!in_main(s) && s->tpsot != 0)) return "coc.once";
    if ((error = codeblock_profile(s, &style.spcoc)) != NULL) return error;
    if (in_main(s)) {
        k->main_coc = 1;
        set_coding(&k->coc, style.scoc.customPrecincts, &style.spcoc);
    } else {
        if (s->rsiz_profile == 1) return "codestream.profile-0";   /* main header only */
        touch(s, c);
        k->tile_transform = (uint8_t)style.spcoc.transform;
        k->tile_levels = (uint8_t)style.spcoc.levels;
    }
    k->coc_header = s->header;
    return NULL;
}

/* QCC (A.6.5): Cqcc a component, then Sqcc and SPqcc as QCD's, one per
 * component in a header and only in a tile's first tile-part. */
static const char *qcc_segment(hv_segments *s, const uint8_t *body, size_t n) {
    Qcd q;
    uint8_t coverage;
    uint32_t c;
    size_t used, rest;
    hv_component *k;
    const char *error;
    if (component_index(s, body, n, &c, &used) != 0 ||
        HV_DECODE_USED(Qcd, &q, body, used, n - used, &rest) != 0 || used + rest != n)
        return decode_error;
    if (c >= s->siz->ncomponents) return "qcc.component";
    if ((error = quant_rule(q.sqcd, q.spqcd.arr, (size_t)q.spqcd.nCount, 1, &coverage)) != NULL) return error;
    k = &s->components[c];
    if (k->qcc_header == s->header || (!in_main(s) && s->tpsot != 0)) return "qcc.once";
    if (in_main(s)) {
        k->main_qcc = 1;
        k->qcc_style = (uint8_t)quant_style(q.sqcd);
        k->qcc_coverage = coverage;
    } else {
        if (s->rsiz_profile == 1) return "codestream.profile-0";   /* main header only */
        touch(s, c);
        k->tile_qcc_style = (uint8_t)quant_style(q.sqcd);
        k->tile_qcc_coverage = coverage;
    }
    k->qcc_header = s->header;
    return NULL;
}

/* RGN (A.6.3): Crgn a component, one per component in a header and only in
 * a tile's first tile-part; SPrgn at most 37 in Profiles 0 and 1. */
static const char *rgn_segment(hv_segments *s, const uint8_t *body, size_t n) {
    RgnStyle style;
    uint32_t c;
    size_t used, rest;
    hv_component *k;
    if (component_index(s, body, n, &c, &used) != 0 ||
        HV_DECODE_USED(RgnStyle, &style, body, used, n - used, &rest) != 0 || used + rest != n)
        return decode_error;
    if (c >= s->siz->ncomponents) return "rgn.component";
    k = &s->components[c];
    if (k->rgn_header == s->header || (!in_main(s) && s->tpsot != 0)) return "rgn.once";
    k->rgn_header = s->header;
    if (style.sprgn > 37 && s->rsiz_profile != 0)
        return s->rsiz_profile == 1 ? "codestream.profile-0" : "codestream.profile-1";
    return NULL;
}

/* POC (A.6.6): whole progression changes, each ending above where it starts
 * (CEpoc 0 stands for 256); one POC in a header, and a tile that has one in
 * a later tile-part has one in its first. Profile 0: RSpoc and CSpoc 0 in
 * the first change. */
static const char *poc_segment(hv_segments *s, const uint8_t *body, size_t n) {
    size_t pos = 0, used;
    int *once = in_main(s) ? &s->main_poc : &s->tile_poc;
    if ((*once)++) return "poc.once";
    if (n == 0) return decode_error;
    while (pos < n) {
        uint64_t rs, cs, re, ce;
        if (s->wide) {
            PocChange_Wide p;
            if (HV_DECODE_USED(PocChange_Wide, &p, body, pos, n - pos, &used) != 0)
                return decode_error;
            rs = p.rspoc; cs = p.cspoc; re = p.repoc; ce = p.cepoc;
        } else {
            PocChange p;
            if (HV_DECODE_USED(PocChange, &p, body, pos, n - pos, &used) != 0)
                return decode_error;
            rs = p.rspoc; cs = p.cspoc; re = p.repoc; ce = p.cepoc;
        }
        if (re <= rs) return "poc.resolution";
        if ((ce == 0 ? 256 : ce) <= cs) return "poc.component";
        if (pos == 0 && s->rsiz_profile == 1 && (rs != 0 || cs != 0))
            return "codestream.profile-0";
        pos += used;
    }
    if (!in_main(s)) {
        if (s->tpsot == 0) s->tile->flags |= HV_TILE_POC;
        else if (!(s->tile->flags & HV_TILE_POC)) return "poc.first-tile-part";
    }
    return NULL;
}

/* A segment of a series by its Z (TLM, PLM, PPM): each Z once. */
static const char *series_add(hv_series *series, uint64_t z, const uint8_t *body, size_t n,
                              const char *index_rule) {
    if (series->body[z] != NULL) return index_rule;
    series->body[z] = body;
    series->size[z] = (uint16_t)n;
    if (z >= series->end) series->end = (unsigned)z + 1;
    return NULL;
}

/* To the next unread byte, past the segments read to their end: 0, or -1
 * at the end of the series. */
static int series_next(hv_series *series) {
    while (series->z < series->end && series->pos >= series->size[series->z]) {
        series->z++;
        series->pos = 0;
    }
    return series->z < series->end ? 0 : -1;
}

/* The next byte of a series: -1 at its end. */
static int series_byte(hv_series *series) {
    return series_next(series) != 0 ? -1 : series->body[series->z][series->pos++];
}

/* The unsigned value of the next `bytes` bytes of a series, most
 * significant first: -1 when it ends before them. */
static int series_value(hv_series *series, unsigned bytes, uint64_t *value) {
    int b;
    *value = 0;
    for (; bytes > 0; bytes--) {
        if ((b = series_byte(series)) < 0) return -1;
        *value = *value << 8 | (uint64_t)b;
    }
    return 0;
}

/* TLM (A.7.1): Ztlm once each, Stlm, and whole entries of the size it
 * gives, each of its type. */
static const char *tlm_segment(hv_segments *s, const uint8_t *body, size_t n) {
    TlmHeader h;
    size_t used, entry, pos;
    const char *error;
    if (HV_DECODE_USED(TlmHeader, &h, body, 0, n, &used) != 0) return decode_error;
    entry = (size_t)h.stlm.st + (h.stlm.sp ? 4 : 2);
    if (n - used == 0 || (n - used) % entry != 0) return decode_error;
    for (pos = used; pos < n; pos += entry) {
        size_t p = pos + (size_t)h.stlm.st;
        Ttlm t;
        Ttlm_Wide tw;
        Ptlm v;
        Ptlm_Wide vw;
        if ((h.stlm.st == 1 && HV_DECODE(Ttlm, &t, body, pos, 1) != 0) ||
            (h.stlm.st == 2 && HV_DECODE(Ttlm_Wide, &tw, body, pos, 2) != 0) ||
            (h.stlm.sp ? HV_DECODE(Ptlm_Wide, &vw, body, p, 4)
                       : HV_DECODE(Ptlm, &v, body, p, 2)) != 0)
            return decode_error;
    }
    if ((error = series_add(&s->tlm, h.ztlm, body + used, n - used, "tlm.index")) != NULL)
        return error;
    s->tlm_st[h.ztlm] = (uint8_t)h.stlm.st;
    s->tlm_sp[h.ztlm] = (uint8_t)h.stlm.sp;
    s->tlm_ordered |= h.stlm.st == 0;
    return NULL;
}

/* The TLM entry of the current tile-part (A.7.1): Ttlm its Isot, or with
 * ST 0 one tile-part per tile, the tiles in order; Ptlm its length, "the
 * same as the value in the corresponding Psot" (with Psot 0, the length it
 * stands for). */
static const char *tlm_entry(hv_segments *s, uint64_t isot, uint64_t tpsot, uint64_t length) {
    hv_series *t = &s->tlm;
    uint64_t v;
    unsigned st, sp;
    if (series_next(t) != 0) return "tlm.tile-parts";
    st = s->tlm_st[t->z];
    sp = s->tlm_sp[t->z];
    series_value(t, st, &v);
    if (st == 0 ? tpsot != 0 || isot != s->parts : v != isot) return "tlm.tile-parts";
    series_value(t, sp ? 4 : 2, &v);
    return v != length ? "tlm.tile-parts" : NULL;
}

/* The next run of a PLM series (A.7.2): Nplm, then Nplm bytes of whole
 * packet lengths in its segment, the sum and how many are zero. NULL; -1 as
 * *nplm at the end of the series. */
static const char *plm_run(hv_series *p, int *nplm, uint64_t *sum, uint64_t *zeros) {
    uint64_t left, value;
    *sum = *zeros = 0;
    if ((*nplm = series_byte(p)) < 0) return NULL;
    for (left = (uint64_t)*nplm; left > 0; ) {
        size_t start;
        const char *error;
        if (series_next(p) != 0) return "plm.length";
        start = p->pos;
        if ((error = hv_rule_packet_length(p->body[p->z], &p->pos, p->size[p->z], 0,
                                           &value)) != NULL)
            return strcmp(error, decode_error) == 0 ? "plm.length" : error;
        if (p->pos - start > left) return "plm.length";
        left -= p->pos - start;
        *sum = value > UINT64_MAX - *sum ? UINT64_MAX : *sum + value;
        *zeros += value == 0;
    }
    return NULL;
}

/* The next run of a PPM series (A.7.4): Nppm, then Nppm bytes of packet
 * headers. NULL; *more 0 at the end of the series. */
static const char *ppm_run(hv_series *p, int *more, uint64_t *nppm) {
    uint64_t left;
    *nppm = 0;
    *more = series_next(p) == 0;
    if (!*more) return NULL;
    if (series_value(p, 4, nppm) != 0) return "ppm.length";
    for (left = *nppm; left > 0; ) {
        size_t k;
        if (series_next(p) != 0) return "ppm.length";
        k = (size_t)(p->size[p->z] - p->pos < left ? p->size[p->z] - p->pos : left);
        p->pos += k;
        left -= k;
    }
    return NULL;
}

/* Rewind after counting the tile-part runs, preserving gaps in Z. */
static void series_rewind(hv_series *series) {
    series->z = 0;
    series->pos = 0;
}

void hv_segments_init(hv_segments *s, const hv_siz *siz, hv_component *components,
                      int profile) {
    const SizFixed *f = siz->fixed;
    memset(s, 0, sizeof *s);
    s->siz = siz;
    s->components = components;
    s->profile = profile;
    s->wide = f->csiz >= 257;
    s->header = 1;
    s->rsiz_profile = f->rsiz == 1 ? 1 : f->rsiz == 2 ? 2 : 0;
    s->grid = hv_tile_axis_count(f->xsiz, f->xtosiz, f->xtsiz) *
              hv_tile_axis_count(f->ysiz, f->ytosiz, f->ytsiz);
}

const char *hv_segments_main(hv_segments *s, uint16_t code, const Cod *cod, const Qcd *qcd,
                             const uint8_t *body, size_t n) {
    Zplm zplm;
    Zppm zppm;
    size_t used, i;
    if (s->profile) return NULL;
    switch (code) {
    case HV_COD:
        set_coding(&s->cod, cod->scod.customPrecincts, &cod->spcod);
        s->cod_sop = (uint8_t)cod->scod.sopMarkers;
        s->cod_eph = (uint8_t)cod->scod.ephMarkers;
        s->mct = (uint8_t)cod->sgcod.mct;
        return codeblock_profile(s, &cod->spcod);
    case HV_QCD:
        s->qcd_style = (uint8_t)quant_style(qcd->sqcd);
        return quant_rule(qcd->sqcd, qcd->spqcd.arr, (size_t)qcd->spqcd.nCount, 0,
                          &s->qcd_coverage);
    case HV_COC: return coc_segment(s, body, n);
    case HV_QCC: return qcc_segment(s, body, n);
    case HV_RGN: return rgn_segment(s, body, n);
    case HV_POC: return poc_segment(s, body, n);
    case HV_TLM: return tlm_segment(s, body, n);
    case HV_PLM:
        if (HV_DECODE_USED(Zplm, &zplm, body, 0, n, &used) != 0 || n - used == 0)
            return decode_error;
        return series_add(&s->plm, zplm, body + used, n - used, "plm.index");
    case HV_PPM:
        /* Lppm 7 to 65,535 (Table A.38): Zppm and at least four bytes. */
        if (HV_DECODE_USED(Zppm, &zppm, body, 0, n, &used) != 0 || n - used < 4)
            return decode_error;
        if (s->rsiz_profile == 1) return "codestream.profile-0";
        s->ppm = 1;
        return series_add(&s->ppm_series, zppm, body + used, n - used, "ppm.index");
    case HV_CRG:
        /* A.9.1: one, in the main header, Xcrg and Ycrg per component. */
        if (s->crg++) return "crg.once";
        if (n != 4 * s->siz->ncomponents) return decode_error;
        for (i = 0; i < n; i += 4) {
            CrgEntry e;
            if (HV_DECODE(CrgEntry, &e, body, i, 4) != 0) return decode_error;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* The effective coding of component c in the current tile, as far as its
 * transform and quantization class go: tile COC over tile COD over main
 * COC over main COD, and likewise QCC and QCD (A.6.1, A.6.4). */
typedef struct {
    unsigned transform, style, levels, coverage;
} coding_class;

static coding_class main_class(const hv_segments *s, uint32_t c) {
    const hv_component *k = &s->components[c];
    coding_class coding = {
        k->main_coc ? k->coc.transform : s->cod.transform,
        k->main_qcc ? k->qcc_style : s->qcd_style,
        k->main_coc ? k->coc.levels : s->cod.levels,
        k->main_qcc ? k->qcc_coverage : s->qcd_coverage
    };
    return coding;
}

static void count_class(uint64_t pairs[2][2], hv_quant_counts *quant,
                         coding_class coding, int delta) {
    if (delta > 0) {
        pairs[coding.transform != 0][coding.style != 0]++;
        quant->levels[coding.levels]++;
        quant->coverage[coding.coverage]++;
        quant->shortfall += coding.levels > coding.coverage;
    } else {
        pairs[coding.transform != 0][coding.style != 0]--;
        quant->levels[coding.levels]--;
        quant->coverage[coding.coverage]--;
        quant->shortfall -= coding.levels > coding.coverage;
    }
}

static coding_class tile_class(const hv_segments *s, uint32_t c) {
    const hv_component *k = &s->components[c];
    coding_class coding = main_class(s, c);
    if (k->coc_header == s->header) {
        coding.transform = k->tile_transform;
        coding.levels = k->tile_levels;
    } else if (s->tile_cod) {
        coding.transform = s->tile_coding.transform;
        coding.levels = s->tile_coding.levels;
    }
    if (k->qcc_header == s->header) {
        coding.style = k->tile_qcc_style;
        coding.coverage = k->tile_qcc_coverage;
    } else if (s->tile_qcd) {
        coding.style = s->tile_qcd_style;
        coding.coverage = s->tile_qcd_coverage;
    }
    return coding;
}

/* floor(a / 2^b). */
static uint64_t floor_shift(uint64_t a, unsigned b) { return b >= 64 ? 0 : a >> b; }

/* Profile 1's LL resolution for the tile of columns [x0, x1), rows [y0,
 * y1) (tx0, tx1, ty0, ty1 of B-7 to B-10), components 0 to 3, NL `levels`
 * each, as Table A.45 states it: floor(tx1 / D) - floor(tx0 / D) <= 128 and
 * floor(ty1 / D) - floor(ty0 / D) <= 128, D = 2^NL. */
static int ll_fits(uint64_t x0, uint64_t x1, uint64_t y0, uint64_t y1, unsigned levels) {
    return floor_shift(x1, levels) - floor_shift(x0, levels) <= 128 &&
           floor_shift(y1, levels) - floor_shift(y0, levels) <= 128;
}

/* The main header's Profile 0 or 1 restrictions (Table A.45) that do not
 * depend on a tile's header. */
static const char *main_profile(hv_segments *s) {
    const SizFixed *f = s->siz->fixed;
    const Component *k = s->siz->components;
    size_t c, n = s->siz->ncomponents;
    const uint64_t limit = 2147483647u;
    int one_tile = f->ytsiz + f->ytosiz >= f->ysiz && f->xtsiz + f->xtosiz >= f->xsiz;
    if (s->rsiz_profile == 1) {
        static const char p0[] = "codestream.profile-0";
        uint64_t w = f->xtsiz < f->xsiz ? f->xtsiz : f->xsiz;
        uint64_t h = f->ytsiz < f->ysiz ? f->ytsiz : f->ysiz;
        if (f->xsiz > limit || f->ysiz > limit) return p0;
        if (!(f->xtsiz == 128 && f->ytsiz == 128) && !one_tile) return p0;
        if (f->xosiz || f->yosiz || f->xtosiz || f->ytosiz) return p0;
        for (c = 0; c < n; c++) {
            const hv_coding *cd = s->components[c].main_coc ? &s->components[c].coc : &s->cod;
            unsigned r;
            if ((k[c].xrsiz != 1 && k[c].xrsiz != 2 && k[c].xrsiz != 4) ||
                (k[c].yrsiz != 1 && k[c].yrsiz != 2 && k[c].yrsiz != 4))
                return p0;
            /* The LL resolution, one tile for the whole image, components 0 to 3. */
            if (c < 4 && one_tile &&
                (f->xsiz - f->xosiz > (uint64_t)128 << cd->levels ||
                 f->ysiz - f->yosiz > (uint64_t)128 << cd->levels))
                return p0;
            /* One precinct in each resolution of at most 128 by 128. The
             * first tile has the largest resolutions, and with zero origins
             * its precinct grid starts at 0, so one precinct holds each
             * resolution just when the resolution fits it. */
            for (r = 0; r <= cd->levels; r++) {
                uint64_t rw = hv_resolution_coord((w + k[c].xrsiz - 1) / k[c].xrsiz, cd->levels - r);
                uint64_t rh = hv_resolution_coord((h + k[c].yrsiz - 1) / k[c].yrsiz, cd->levels - r);
                unsigned ppx = cd->custom ? cd->precincts[r] & 15 : 15;
                unsigned ppy = cd->custom ? cd->precincts[r] >> 4 : 15;
                if (rw <= 128 && rh <= 128 && (rw > (uint64_t)1 << ppx || rh > (uint64_t)1 << ppy))
                    return p0;
            }
        }
    } else if (s->rsiz_profile == 2) {
        static const char p1[] = "codestream.profile-1";
        if (f->xsiz > limit || f->ysiz > limit || f->xosiz > limit || f->yosiz > limit ||
            f->xtosiz > limit || f->ytosiz > limit)
            return p1;
        if (!one_tile) {
            if (f->xtsiz != f->ytsiz) return p1;
            for (c = 0; c < n; c++) {
                uint64_t m = k[c].xrsiz < k[c].yrsiz ? k[c].xrsiz : k[c].yrsiz;
                if (f->xtsiz < 1024 * m) return p1;
            }
        }
        /* LL dimensions depend on tile COD/COC overrides: tile_coding
         * checks them after the first tile-part header. */
    }
    return NULL;
}

/* The end of the main header, at the first SOT. */
static const char *main_end(hv_segments *s) {
    size_t c;
    const char *error;
    for (c = 0; c < s->siz->ncomponents; c++)
        count_class(s->pairs, &s->quant, main_class(s, (uint32_t)c), 1);
    if ((error = main_profile(s)) != NULL) return error;
    /* Count the runs of PLM and PPM, checking each, then read them from the
     * start again. */
    for (;;) {
        int nplm;
        uint64_t sum, zeros;
        if ((error = plm_run(&s->plm, &nplm, &sum, &zeros)) != NULL) return error;
        if (nplm < 0) break;
        s->plm.runs++;
    }
    for (;;) {
        int more;
        uint64_t nppm;
        if ((error = ppm_run(&s->ppm_series, &more, &nppm)) != NULL) return error;
        if (!more) break;
        s->ppm_series.runs++;
    }
    series_rewind(&s->plm);
    series_rewind(&s->ppm_series);
    return NULL;
}

const char *hv_segments_tile_part(hv_segments *s, hv_tile_count *tile, uint64_t isot,
                                  uint64_t tpsot, uint64_t length) {
    const char *error;
    if (s->profile) return NULL;
    if (s->tile == NULL && s->parts == 0 && (error = main_end(s)) != NULL) return error;
    s->header++;
    s->tile = tile;
    s->isot = (uint32_t)isot;
    s->tpsot = tpsot;
    s->psot = length;
    s->tile_cod = s->tile_qcd = s->tile_poc = 0;
    memset(s->ppt_seen, 0, sizeof s->ppt_seen);
    s->touched = 0;
    if (s->tlm.end > 0 && (error = tlm_entry(s, isot, tpsot, length)) != NULL) return error;
    if (s->rsiz_profile == 1) {
        /* Table A.45: the first tile-parts of the tiles in Isot order,
         * before any other tile-part. */
        if (tpsot == 0 ? s->later_parts || isot != s->first_parts : 0)
            return "codestream.profile-0";
        if (tpsot != 0) s->later_parts = 1;
    }
    s->first_parts += tpsot == 0;
    s->parts++;
    return NULL;
}

const char *hv_segments_tile(hv_segments *s, uint16_t code, const Cod *cod, const Qcd *qcd,
                             const uint8_t *body, size_t n) {
    Zppt z;
    size_t used;
    if (s->profile) return NULL;
    switch (code) {
    case HV_COD:
        if (s->rsiz_profile == 1) return "codestream.profile-0";   /* main header only */
        s->tile_cod = 1;
        set_coding(&s->tile_coding, cod->scod.customPrecincts, &cod->spcod);
        s->tile_mct = (uint8_t)cod->sgcod.mct;
        s->tile->flags = (uint8_t)((s->tile->flags & ~(HV_TILE_SOP | HV_TILE_EPH)) | HV_TILE_COD |
                                   (cod->scod.sopMarkers ? HV_TILE_SOP : 0) |
                                   (cod->scod.ephMarkers ? HV_TILE_EPH : 0));
        return codeblock_profile(s, &cod->spcod);
    case HV_QCD:
        if (s->rsiz_profile == 1) return "codestream.profile-0";
        s->tile_qcd = 1;
        s->tile_qcd_style = (uint8_t)quant_style(qcd->sqcd);
        return quant_rule(qcd->sqcd, qcd->spqcd.arr, (size_t)qcd->spqcd.nCount, 0,
                          &s->tile_qcd_coverage);
    case HV_COC: return coc_segment(s, body, n);
    case HV_QCC: return qcc_segment(s, body, n);
    case HV_RGN: return rgn_segment(s, body, n);
    case HV_POC: return poc_segment(s, body, n);
    case HV_PPT:
        /* A.7.5: Zppt once each in a header, then at least one byte (Lppt 4
         * to 65,535, Table A.39); not with PPM (A.7.4). */
        if (HV_DECODE_USED(Zppt, &z, body, 0, n, &used) != 0 || n - used == 0) return decode_error;
        if (s->ppm) return "ppm.ppt";
        if (s->rsiz_profile == 1) return "codestream.profile-0";
        if (s->ppt_seen[z / 8] & (1u << (z % 8))) return "ppt.index";
        s->ppt_seen[z / 8] |= (uint8_t)(1u << (z % 8));
        s->tile->flags |= HV_TILE_PPT;
        return NULL;
    default:
        return NULL;
    }
}

int hv_segments_packed(const hv_segments *s) {
    return s->ppm || (s->tile != NULL && (s->tile->flags & HV_TILE_PPT));
}

/* The tile's coding at the end of its first tile-part's header: each
 * tile-component's transform against its quantization (quant_fits), the
 * transform of components 0 to 2 under the multiple component transform
 * (G.2, G.3: the reversible one with the 5-3 filter, the irreversible one
 * with the 9-7), and Profile 1's LL resolution. The components the
 * header sets are checked one by one, the others by the main header's
 * transform/style classes and level/coverage counts. */
static const char *tile_coding(hv_segments *s) {
    uint64_t pairs[2][2];
    hv_quant_counts quant = s->quant;
    uint32_t c, remaining = (uint32_t)s->siz->ncomponents;
    unsigned t, q;
    memcpy(pairs, s->pairs, sizeof pairs);
    for (c = s->touched; c != 0; c = s->components[c - 1].next) {
        coding_class tile = tile_class(s, c - 1);
        count_class(pairs, &quant, main_class(s, c - 1), -1);
        remaining--;
        if (!quant_fits(tile.transform, tile.style))
            return "codestream.quantization-transform";
        if (tile.levels > tile.coverage)
            return "codestream.quantization-coverage";
    }
    for (t = 0; t < 2; t++)
        for (q = 0; q < 2; q++)
            if (pairs[t][q] > 0 &&
                !quant_fits(s->tile_cod ? s->tile_coding.transform : t,
                            s->tile_qcd ? s->tile_qcd_style : q))
                return "codestream.quantization-transform";
    /* Untouched components retain their main COC/QCC unless a tile
     * default replaces that side. Histograms avoid a component scan per
     * tile; only the overrides above require individual checks. */
    if (s->tile_cod && s->tile_qcd) {
        if (remaining && s->tile_coding.levels > s->tile_qcd_coverage)
            return "codestream.quantization-coverage";
    } else if (s->tile_cod) {
        for (q = 0; q < s->tile_coding.levels; q++)
            if (quant.coverage[q]) return "codestream.quantization-coverage";
    } else if (s->tile_qcd) {
        for (t = s->tile_qcd_coverage + 1; t <= 32; t++)
            if (quant.levels[t]) return "codestream.quantization-coverage";
    } else if (quant.shortfall) {
        return "codestream.quantization-coverage";
    }
    if ((s->tile_cod ? s->tile_mct : s->mct) && s->siz->ncomponents >= 3 &&
        (tile_class(s, 0).transform != tile_class(s, 1).transform ||
         tile_class(s, 0).transform != tile_class(s, 2).transform))
        return "codestream.mct-transform";
    if (s->rsiz_profile == 2) {
        const SizFixed *f = s->siz->fixed;
        uint64_t nx = hv_tile_axis_count(f->xsiz, f->xtosiz, f->xtsiz);
        hv_interval x = hv_tile_axis_extent(f->xosiz, f->xsiz, f->xtosiz, f->xtsiz, s->isot % nx);
        hv_interval y = hv_tile_axis_extent(f->yosiz, f->ysiz, f->ytosiz, f->ytsiz, s->isot / nx);
        for (c = 0; c < s->siz->ncomponents && c < 4; c++)
            if (!ll_fits(x.first, x.end, y.first, y.end, tile_class(s, c).levels)) return "codestream.profile-1";
    }
    return NULL;
}

/* The SOP and EPH markers in the data (A.8), which the coded data cannot
 * imitate (C.3.4: a byte above 0x8F after 0xFF is a marker; B.10.1 and
 * D.4's NOTE keep the packet headers and the coded data from making one):
 * SOP only where the tile's COD signals it (A.6.1), with Lsop 4 (Table
 * A.40); EPH only where the COD signals it and the packet headers are in
 * the data (A.8.2). *sops the SOP markers. */
static const char *markers_in_data(const hv_segments *s, const uint8_t *data, size_t n,
                                   uint64_t *sops) {
    int cod = s->tile->flags & HV_TILE_COD;
    int sop = cod ? (s->tile->flags & HV_TILE_SOP) != 0 : s->cod_sop;
    int eph = cod ? (s->tile->flags & HV_TILE_EPH) != 0 : s->cod_eph;
    const uint8_t *p = data, *end = data + n;
    *sops = 0;
    while (p < end && (p = memchr(p, 0xFF, (size_t)(end - p))) != NULL && end - p >= 2) {
        if (p[1] == 0x91) {
            if (!sop) return "codestream.sop-unsignalled";
            if (end - p < 6 || p[2] != 0 || p[3] != 4) return "sop.length";
            (*sops)++;
            p += 6;
            continue;
        }
        if (p[1] == 0x92) {
            if (hv_segments_packed(s)) return "codestream.eph-in-data";
            if (!eph) return "codestream.eph-unsignalled";
        }
        p++;
    }
    return NULL;
}

const char *hv_segments_data(hv_segments *s, const uint8_t *data, size_t n, unsigned plts,
                             uint64_t plt_sum, uint64_t plt_zeros) {
    const char *error;
    uint64_t sops = 0;
    if (!s->profile) {
        if (s->tpsot == 0 && (error = tile_coding(s)) != NULL) return error;
        if (plt_zeros != 0 && !hv_segments_packed(s)) return "plt.zero-length";
        if ((error = markers_in_data(s, data, n, &sops)) != NULL) return error;
    }
    /* A.7.3: the PLT entries of a tile-part list every packet in it. Where
     * its COD allows SOP markers, T.800 does not say whether a packet's
     * length counts the SOP before it; either sum is taken. */
    if (plts != 0 && (error = hv_rule_plt_coverage(plt_sum, n, sops)) != NULL)
        return error;
    if (s->profile) return NULL;
    if (s->plm.end > 0) {
        int nplm;
        uint64_t sum, zeros;
        if (s->plm.used++ == s->plm.runs) return "plm.tile-parts";
        if ((error = plm_run(&s->plm, &nplm, &sum, &zeros)) != NULL) return error;
        if (zeros != 0 && !hv_segments_packed(s)) return "plm.zero-length";
        if (sum != n && (sops == 0 || sum != n - 6 * sops)) return "plm.coverage";
    }
    if (s->ppm_series.end > 0) {
        int more;
        uint64_t nppm;
        if (s->ppm_series.used++ == s->ppm_series.runs) return "ppm.tile-parts";
        if ((error = ppm_run(&s->ppm_series, &more, &nppm)) != NULL) return error;
        /* A.7.4: with PPM no packet is in the data with its header, so data
         * without packet headers has no packets. */
        if (nppm == 0 && n != 0) return "ppm.packet-headers";
    }
    return NULL;
}

const char *hv_segments_end(hv_segments *s) {
    if (s->profile) return NULL;
    if (s->tlm.end > 0 &&
        (series_next(&s->tlm) == 0 || (s->tlm_ordered && s->parts != s->grid)))
        return "tlm.tile-parts";
    if (s->plm.used != s->plm.runs) return "plm.tile-parts";
    if (s->ppm_series.used != s->ppm_series.runs) return "ppm.tile-parts";
    if (s->first_parts != s->grid) return "codestream.tile-count";
    return NULL;
}

/* ------------------------------------------------------------------------
 * Boxes
 * ------------------------------------------------------------------------ */

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

void hv_boxes_file(hv_boxes *it, const uint8_t *buf, size_t size) {
    it->buf = buf;
    it->pos = 0;
    it->end = size;
    it->to_end = 1;
}

void hv_boxes_children(hv_boxes *it, const uint8_t *buf, const hv_box *parent) {
    it->buf = buf;
    it->pos = parent->payload;
    it->end = parent->end;
    it->to_end = parent->to_end;
}

int hv_boxes_next(hv_boxes *it, hv_box *box, const char **error, size_t *at) {
    BoxHeader h;
    size_t avail, header = 0, size = 0;
    const char *fault = NULL;

    if (it->pos == it->end)             /* also after a box with LBox = 0 */
        return 0;
    avail = it->end - it->pos;
    if (HV_DECODE_USED(BoxHeader, &h, it->buf, it->pos, min_size(avail, HV_BOX_HEADER_XL),
                       &header) != 0) {
        fault = "invalid or truncated box header";
    } else if (h.lbox == 1) {
        if (h.xlbox > avail)
            fault = "box overruns its container";
        size = (size_t)h.xlbox;
    } else if (h.lbox == 0) {
        /* I.4: LBox = 0 only for the last box, and a box inside a superbox
         * may use it only if the superbox does too. */
        if (!it->to_end)
            fault = "LBox = 0 inside a box that does not run to the end of the file";
        size = avail;
    } else {
        if (h.lbox > avail)
            fault = "box overruns its container";
        size = (size_t)h.lbox;
    }
    if (fault != NULL) {
        *error = fault;
        *at = it->pos;
        return -1;
    }
    box->type = (uint32_t)h.tbox;
    box->start = it->pos;
    box->payload = it->pos + header;
    box->end = it->pos + size;
    box->to_end = h.lbox == 0;
    it->pos = box->end;
    return 1;
}

int hv_is_superbox(uint32_t type) {
    switch (type) {
    case HV_BOX_JP2H: /* T.800 I.5.3 */
    case HV_BOX_RES:  /* T.800 I.5.3.7 */
    case HV_BOX_UINF: /* T.800 I.7.3 */
    case HV_BOX_FTBL: /* T.801 M.11.3 */
    case HV_BOX_JPCH: /* T.801 M.11.6 */
    case HV_BOX_JPLH: /* T.801 M.11.7 */
    case HV_BOX_CGRP: /* T.801 M.11.7.1 */
    case HV_BOX_COMP: /* T.801 M.11.10 */
    case HV_BOX_ASOC: /* T.801 M.11.11 */
    case HV_BOX_DREP: /* T.801 M.11.15 */
    case HV_BOX_JCLX: /* T.801 M.11.21 */
    case HV_BOX_J2CX: /* T.801 M.11.23 */
    case HV_BOX_GRP:  /* T.801 M.11.25 */
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------
 * The file
 * ------------------------------------------------------------------------ */

int hv_ftyp_decode(const uint8_t *buf, const hv_box *box, hv_ftyp *f) {
    FtypHeader h;
    Brand entry;
    size_t used, length = box->end - box->payload, i;

    memset(f, 0, sizeof *f);
    if (box->type != HV_BOX_FTYP ||
        HV_DECODE_USED(FtypHeader, &h, buf, box->payload, length, &used) != 0 ||
        length - used < HV_FIXED(Brand) || (length - used) % HV_FIXED(Brand) != 0)
        return -1;
    f->brand = h.brand;
    f->minor = h.minor;
    for (i = box->payload + used; i < box->end; i += HV_FIXED(Brand)) {
        /* Any 32 bits are a Brand: its decoder cannot fail. */
        if (HV_DECODE(Brand, &entry, buf, i, HV_FIXED(Brand)) != 0)
            return -1;
        f->jp2 |= entry == HV_BRAND_JP2;
        f->jpx |= entry == HV_BRAND_JPX;
        f->jpxb |= entry == HV_BRAND_JPXB;
    }
    return 0;
}

int hv_ftyp_read(const uint8_t *buf, size_t size, hv_ftyp *f) {
    hv_boxes it;
    hv_box box;
    const char *error;
    size_t at;
    int n;

    memset(f, 0, sizeof *f);
    hv_boxes_file(&it, buf, size);
    for (n = 0; n < 2; n++)
        if (hv_boxes_next(&it, &box, &error, &at) != 1)
            return -1;
    return hv_ftyp_decode(buf, &box, f);
}

int hv_ftyp_is_jpx(const hv_ftyp *f) {
    return f->brand == HV_BRAND_JPX;
}

const char *hv_rule_ftyp(const hv_ftyp *f, int jpx, int profile) {
    if ((jpx || profile) && f->brand != (jpx ? HV_BRAND_JPX : HV_BRAND_JP2))
        return "file.ftyp-brand";
    return (jpx ? f->jpx : f->jp2) ? NULL : "file.ftyp-compatibility";
}

const char *hv_rule_ftyp_minor(uint64_t minv, int jpx) {
    return minv != (jpx ? 1u : 0u) ? "file.ftyp-minor" : NULL;
}

/* A superbox the walk is in: its type and what its children so far
 * hold. */
typedef struct {
    uint32_t type;
    uint32_t first;             /* the type of its first child */
    int children;
    int opct, creg, pxfm, cdef, flst;  /* children of these types */
    /* A j2cx (M.11.23, M.11.24): its j2ci's Ncs and Ltbl, the codestreams
     * of its sub-boxes, whether an mdat or free has come (tail), the last
     * codestream sub-box so far, and the first one Ltbl does not describe
     * (+ 1). */
    uint64_t ncs, ltbl, codestreams, sub_length, sub_codestreams;
    size_t sub_at, ltbl_bad;
    int subs, tail;
    /* A jclx (M.11.21, M.11.22): its jlxi's M, C, L, T and F, its jpch and
     * jplh, its compositing groups, those with inst boxes (its threads),
     * whether the last group has one, and whether another box has come. */
    uint64_t m, c, l, t, f;
    uint64_t jpch, jplh, groups, threads;
    int group_inst, other;
} tree_level;

typedef struct {
    const uint8_t *buf;
    int jpx;
    hv_box_tree *tree;
    int signatures, ftyps, comps, dreps, gtsos, colrs;
    int top_codestream;                 /* a top-level jp2c */
    int top_baseline;                   /* a jp2c, ftbl, mdat, jpch or jplh, at any depth */
    int top_jpch, top_jplh, top_streams, cregs;   /* top_streams: top-level jp2c, ftbl */
    size_t jclx_at;                     /* the first top-level jclx */
    const char *jclx_last;              /* a rule for the last jclx only (M.11.22) */
    size_t jclx_last_at;
    tree_level level[HV_BOX_DEPTH_MAX];
} tree_walk;

/* Whether the walk reads a box of this type as boxes, in this kind of
 * file. */
static int tree_superbox(const tree_walk *w, uint32_t type) {
    if (!w->jpx)
        return type == HV_BOX_JP2H || type == HV_BOX_RES || type == HV_BOX_UINF;
    return hv_is_superbox(type) || type == HV_BOX_DTBL;
}

/* M.11.13: a Label box's string, UTF-8 without the characters it
 * excludes. */
static int label_ok(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t c = s[i], min;
        size_t k, more;
        if (c < 0x80) more = 0, min = 0;
        else if ((c & 0xE0) == 0xC0) more = 1, c &= 0x1F, min = 0x80;
        else if ((c & 0xF0) == 0xE0) more = 2, c &= 0x0F, min = 0x800;
        else if ((c & 0xF8) == 0xF0) more = 3, c &= 0x07, min = 0x10000;
        else return 0;
        if (n - i - 1 < more) return 0;
        for (k = 1; k <= more; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            c = c << 6 | (s[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0;
        if (c <= 0x1F || (c >= 0x7F && c <= 0x9F) || c == '/' || c == ';' || c == '?' ||
            c == ':' || c == '#')
            return 0;
        i += more + 1;
    }
    return 1;
}

/* A url box in a uinf or, in a JPX file, a dtbl (T.800 I.7.3.2, T.801
 * M.11.2). */
static const char *uinf_url(const uint8_t *buf, const hv_box *box) {
    UrlHeader h;
    size_t n = box->end - box->payload, used;
    const uint8_t *nul;
    if (HV_DECODE_USED(UrlHeader, &h, buf, box->payload, n, &used) != 0)
        return n < HV_FIXED(UrlHeader) ? "url.terminator" : "url.version-flags";
    if ((nul = memchr(buf + box->payload + used, 0, n - used)) == NULL) return "url.terminator";
    return nul != buf + box->end - 1 ? "box.extent" : NULL;
}

/* A ulst box (T.800 I.7.3.1): NU, then NU UUIDs filling it. */
static const char *ulst(const uint8_t *buf, const hv_box *box) {
    UuidCount nu;
    size_t n = box->end - box->payload, used;
    if (HV_DECODE_USED(UuidCount, &nu, buf, box->payload, n, &used) != 0 ||
        (n - used) % HV_FIXED(UuidId) != 0 || (n - used) / HV_FIXED(UuidId) != nu)
        return "ulst.nu-count";
    return NULL;
}

/* The children of a uinf (T.800 I.7.3): a ulst, then a url. */
static const char *uinf_contents(const uint8_t *buf, const hv_box *uinf, size_t *at) {
    hv_boxes it;
    hv_box c;
    const char *error;
    int status, ulsts = 0, urls = 0;
    hv_boxes_children(&it, buf, uinf);
    while ((status = hv_boxes_next(&it, &c, &error, at)) == 1) {
        if (c.type == HV_BOX_ULST && (ulsts++ > 0 || urls > 0)) break;
        if (c.type == HV_BOX_URL && urls++ > 0) break;
    }
    if (status < 0) return "box.framing";
    *at = status == 1 ? c.start : uinf->start;
    return status == 1 || ulsts != 1 || urls != 1 ? "uinf.contents" : NULL;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A codestream sub-box of the j2cx j, `length` bytes long and holding
 * `codestreams` codestreams: Ltbl (M.11.24), when not 0, gives the length
 * L and 2^R codestreams of every one but the last. */
static void j2cx_sub(tree_level *j, uint64_t length, uint64_t codestreams, size_t start) {
    uint64_t l = j->ltbl & ((1u << 26) - 1), r = j->ltbl >> 26;
    if (j->ltbl != 0 && j->subs > 0 && j->ltbl_bad == 0 &&
        (j->sub_length != l || j->sub_codestreams != (uint64_t)1 << r))
        j->ltbl_bad = j->sub_at + 1;
    j->subs++;
    j->sub_length = length;
    j->sub_codestreams = codestreams;
    j->sub_at = start;
    j->codestreams += codestreams;
}

/* The placement rules of one box, whose parent (0 at the top level) and
 * grandparent are given, and its own contents. */
static const char *tree_box(tree_walk *w, const hv_box *box, int depth, size_t *at) {
    const uint8_t *buf = w->buf;
    uint32_t t = box->type;
    uint32_t parent = depth > 0 ? w->level[depth - 1].type : 0;
    tree_level *up = depth > 0 ? &w->level[depth - 1] : NULL;
    size_t n = box->end - box->payload;
    const char *r;

    *at = box->start;
    if (t == HV_BOX_JP && (depth > 0 || w->signatures++ > 0)) return "file.one-signature";
    if (t == HV_BOX_FTYP && (depth > 0 || w->ftyps++ > 0)) return "file.one-ftyp";
    if (depth > 0 && (t == HV_BOX_JP2H || t == HV_BOX_UINF)) return "box.nested-top-level";
    if (t == HV_BOX_UUID && n < HV_FIXED(UuidId)) return "uuid.id";
    if (t == HV_BOX_ULST && (r = ulst(buf, box)) != NULL) return r;
    if (t == HV_BOX_URL && (parent == HV_BOX_UINF || (w->jpx && parent == HV_BOX_DTBL)) &&
        (r = uinf_url(buf, box)) != NULL)
        return r;
    if (t == HV_BOX_UINF && (r = uinf_contents(buf, box, at)) != NULL) return r;
    *at = box->start;
    if (t == HV_BOX_JP2I) w->tree->ipr++;
    /* T.800 I.2.2, Figure I.1: where the boxes T.800 defines are
     * contained; an Image Header box elsewhere "shall be ignored"
     * (I.5.3.1). T.801 puts colr in jp2h or cgrp, below. */
    if (depth == 0 && (t == HV_BOX_BPCC || t == HV_BOX_PCLR || t == HV_BOX_CMAP ||
                       t == HV_BOX_CDEF || t == HV_BOX_RES || (t == HV_BOX_COLR && !w->jpx)))
        return "box.top-level";
    if (!w->jpx) {
        if (depth > 0 && t == HV_BOX_JP2C) return "box.nested-jp2c";
        if (t == HV_BOX_URL && parent != HV_BOX_UINF) return "box.url-placement";
        /* Figure I.1: the header boxes in jp2h, resc and resd in res, ulst
         * in uinf. */
        if ((depth > 0 && parent != HV_BOX_JP2H &&
             (t == HV_BOX_BPCC || t == HV_BOX_COLR || t == HV_BOX_PCLR || t == HV_BOX_CMAP ||
              t == HV_BOX_CDEF || t == HV_BOX_RES)) ||
            ((t == HV_BOX_RESC || t == HV_BOX_RESD) && parent != HV_BOX_RES) ||
            (t == HV_BOX_ULST && parent != HV_BOX_UINF))
            return "box.containment";
    }
    if (depth == 0) {
        if (t == HV_BOX_JP2H) {
            w->tree->jp2h++;
            w->tree->jp2h_late |= w->top_codestream;
            w->tree->jp2h_late_baseline |= w->top_baseline;
        }
        w->tree->jp2c += t == HV_BOX_JP2C;
        w->top_codestream |= t == HV_BOX_JP2C;
    }
    /* M.9.2.7: at any depth (a j2cx holds jp2c and ftbl, a jclx jpch and
     * jplh). */
    w->top_baseline |= t == HV_BOX_JP2C || t == HV_BOX_FTBL || t == HV_BOX_MDAT ||
                       t == HV_BOX_JPCH || t == HV_BOX_JPLH;
    if (!w->jpx) return NULL;

    /* T.801 */
    if (depth == 0) {
        /* M.11.23: "all top-level Contiguous Codestream boxes and Fragment
         * Table boxes ... shall precede any Multiple Codestream boxes";
         * M.11.6, M.11.7, M.11.21: the top-level jpch and jplh, and comp,
         * precede any jclx. */
        if ((t == HV_BOX_JP2C || t == HV_BOX_FTBL) && w->tree->j2cx > 0) return "j2cx.order";
        if ((t == HV_BOX_JPCH || t == HV_BOX_JPLH || t == HV_BOX_COMP) && w->tree->jclx > 0)
            return "jclx.order";
        if (t == HV_BOX_JCLX && w->tree->jclx == 0) w->jclx_at = box->start;
        if (t == HV_BOX_JCLX && w->jclx_last != NULL) {
            *at = w->jclx_last_at;
            return w->jclx_last;
        }
        w->top_streams += t == HV_BOX_JP2C || t == HV_BOX_FTBL;
        w->top_jpch += t == HV_BOX_JPCH;
        w->top_jplh += t == HV_BOX_JPLH;
        w->tree->j2cx += t == HV_BOX_J2CX;
        w->tree->jclx += t == HV_BOX_JCLX;
    } else {
        if (t == HV_BOX_JP2C && parent != HV_BOX_J2CX)
            return parent == HV_BOX_JPCH ? "jpch.nested-jp2c" : "box.nested-jp2c";
        if (t == HV_BOX_DTBL || t == HV_BOX_RREQ || t == HV_BOX_COMP || t == HV_BOX_JCLX ||
            ((t == HV_BOX_JPCH || t == HV_BOX_JPLH) && parent != HV_BOX_JCLX) ||
            ((t == HV_BOX_FTBL || t == HV_BOX_J2CX) && parent != HV_BOX_J2CX))
            return "box.nested-top-level";
    }
    if (parent == HV_BOX_DTBL && t != HV_BOX_URL) return "dtbl.non-url";
    if (t == HV_BOX_FLST && parent != HV_BOX_FTBL) return "box.flst-placement";
    if (t == HV_BOX_URL && parent != HV_BOX_DTBL && parent != HV_BOX_UINF)
        return "box.url-placement";
    if (t == HV_BOX_CGRP && parent != HV_BOX_JPLH) return "cgrp.placement";
    if (t == HV_BOX_COLR && parent != HV_BOX_JP2H && parent != HV_BOX_CGRP)
        return "colr.placement";
    if (t == HV_BOX_OPCT && parent != HV_BOX_JPLH) return "opct.placement";
    if (t == HV_BOX_CREG && parent != HV_BOX_JPLH) return "creg.placement";
    if (t == HV_BOX_CREF && parent != HV_BOX_JPCH && parent != HV_BOX_JPLH &&
        parent != HV_BOX_ASOC)
        return "cref.placement";
    if (t == HV_BOX_COMP && w->comps++ > 0) return "comp.once";
    if (t == HV_BOX_COPT && (parent != HV_BOX_COMP || up->children != 0)) return "copt.placement";
    if (t == HV_BOX_INST && parent != HV_BOX_COMP && parent != HV_BOX_JCLX)
        return "inst.placement";
    if (t == HV_BOX_DREP && w->dreps++ > 0) return "drep.once";
    if (t == HV_BOX_GTSO && parent != HV_BOX_DREP) return "gtso.placement";
    if (t == HV_BOX_GTSO && w->gtsos++ > 0) return "gtso.once";
    if (t == HV_BOX_LBL && !label_ok(buf + box->payload, n)) return "lbl.characters";
    if (t == HV_BOX_CREF) {
        CrefType rtyp;
        if (HV_DECODE(CrefType, &rtyp, buf, box->payload, n) == 0 && rtyp == HV_BOX_JP2I)
            w->tree->ipr++;
    }
    w->colrs += t == HV_BOX_COLR;
    if (t == HV_BOX_MDAT) {
        hv_box_tree *tree = w->tree;
        if (tree->mdat != NULL && tree->mdat_count < tree->mdat_cap) {
            tree->mdat[tree->mdat_count].start = box->payload;
            tree->mdat[tree->mdat_count].end = box->end;
        }
        tree->mdat_count++;
    }
    w->cregs += t == HV_BOX_CREG;
    /* M.11.23, M.11.24: a j2cx holds a j2ci, first, of 8 bytes, then jp2c,
     * ftbl and j2cx boxes, then mdat and free boxes. */
    if (parent == HV_BOX_J2CX && up->children == 0) {
        if (t != HV_BOX_J2CI) return "j2ci.placement";
        if (n != 8) return "j2ci.length";
        up->ncs = be32(buf + box->payload);
        up->ltbl = be32(buf + box->payload + 4);
    } else if (t == HV_BOX_J2CI) {
        return "j2ci.placement";
    } else if (parent == HV_BOX_J2CX) {
        if (t == HV_BOX_MDAT || t == HV_BOX_FREE) up->tail = 1;
        if ((t == HV_BOX_JP2C || t == HV_BOX_FTBL || t == HV_BOX_J2CX) && up->tail)
            return "j2cx.contents";
        if (t == HV_BOX_JP2C || t == HV_BOX_FTBL)
            j2cx_sub(up, box->end - box->start, 1, box->start);
    }
    /* M.11.21, M.11.22: a jclx holds a jlxi, first, of 20 bytes, or 24
     * with LIFE-START when T is not 0; then its jpch boxes; then its
     * compositing groups, each jplh boxes followed by inst boxes, which
     * only the last group may lack; then other boxes. */
    if (parent == HV_BOX_JCLX && up->children == 0) {
        const uint8_t *p = buf + box->payload;
        if (t != HV_BOX_JLXI) return "jlxi.placement";
        if (n < 20 || n != (be32(p + 12) != 0 ? 24u : 20u)) return "jlxi.length";
        up->m = be32(p);
        up->c = be32(p + 4);
        up->l = be32(p + 8);
        up->t = be32(p + 12);
        up->f = be32(p + 16);
    } else if (t == HV_BOX_JLXI) {
        return "jlxi.placement";
    } else if (parent == HV_BOX_JCLX) {
        if (t == HV_BOX_JPCH && (up->jplh > 0 || up->other)) return "jclx.contents";
        if (t == HV_BOX_JPLH || t == HV_BOX_INST) {
            if (up->other || (t == HV_BOX_INST && up->jplh == 0)) return "jclx.contents";
            if (t == HV_BOX_JPLH && (up->jplh == 0 || up->group_inst)) {
                up->groups++;
                up->group_inst = 0;
            }
            if (t == HV_BOX_INST && !up->group_inst) {
                up->threads++;
                up->group_inst = 1;
            }
        }
        up->jpch += t == HV_BOX_JPCH;
        up->jplh += t == HV_BOX_JPLH;
        up->other |= t != HV_BOX_JPCH && t != HV_BOX_JPLH && t != HV_BOX_INST;
    }
    /* M.11.25: "The Grouping box shall not be the first box within an
     * Association box". */
    if (t == HV_BOX_GRP && parent == HV_BOX_ASOC && up->children == 0) return "grp.placement";
    if (up != NULL) {
        if (t == HV_BOX_OPCT && up->opct++ > 0) return "opct.once";
        if (t == HV_BOX_CREG && up->creg++ > 0) return "creg.once";
        if (t == HV_BOX_PXFM && (parent == HV_BOX_JP2H || parent == HV_BOX_JPLH) &&
            up->pxfm++ > 0)
            return "pxfm.once";
        up->cdef += t == HV_BOX_CDEF;
        up->flst += t == HV_BOX_FLST;
        if (up->opct > 0 && up->cdef > 0) return "opct.cdef";
    }
    return NULL;
}

/* The end of a superbox's children. */
static const char *tree_end(tree_walk *w, const hv_box *box, int depth, size_t *at) {
    const tree_level *l = &w->level[depth];
    *at = box->start;
    if (!w->jpx) return NULL;
    if (box->type == HV_BOX_DTBL && (uint64_t)l->children != w->tree->ndr)
        return "dtbl.ndr-count";
    if (box->type == HV_BOX_FTBL && l->flst != 1) return "ftbl.one-flst";   /* M.11.3 */
    if (box->type == HV_BOX_ASOC && l->children < 2) return "asoc.children";
    if (box->type == HV_BOX_COMP && (l->children == 0 || l->first != HV_BOX_COPT))
        return "comp.copt-first";
    if (box->type == HV_BOX_J2CX) {
        tree_level *up = depth > 0 ? &w->level[depth - 1] : NULL;
        if (l->children == 0) return "j2ci.placement";
        /* M.11.23: the codestreams "shall agree with" Ncs, 1 to 2^32 - 1. */
        if (l->ncs == 0 || l->ncs != l->codestreams) return "j2ci.ncs";
        if (l->ltbl_bad != 0) {
            *at = l->ltbl_bad - 1;
            return "j2ci.ltbl";
        }
        if (up != NULL && up->type == HV_BOX_J2CX)
            j2cx_sub(up, box->end - box->start, l->codestreams, box->start);
    }
    if (box->type == HV_BOX_JCLX) {
        if (l->children == 0) return "jlxi.placement";
        if (l->groups == 0) return "jclx.contents";         /* Gjclx at least 1 */
        if (l->c != l->jpch || l->l != l->jplh || l->t != l->threads) return "jlxi.counts";
        /* M.11.22: M 0 only with C not 0, and only in the last jclx, as F 0
         * with T not 0. */
        if (l->m == 0 && l->c == 0) return "jlxi.repetition";
        w->jclx_last = l->m == 0 ? "jlxi.repetition"
                     : l->f == 0 && l->t != 0 ? "jlxi.frames" : NULL;
        w->jclx_last_at = box->start;
    }
    return NULL;
}

static const char *tree_walk_boxes(tree_walk *w, hv_boxes *it, int depth, size_t *at);

/* One box at `depth`, and below. */
static const char *tree_walk_box(tree_walk *w, const hv_box *box, int depth, size_t *at) {
    tree_level *l;
    hv_boxes children;
    hv_box inner = *box;
    const char *error;
    if ((error = tree_box(w, box, depth, at)) != NULL) return error;
    if (depth > 0) {
        l = &w->level[depth - 1];
        if (l->children++ == 0) l->first = box->type;
    }
    if (!tree_superbox(w, box->type)) return NULL;
    if (depth >= HV_BOX_DEPTH_MAX) return "box.depth-limit";
    l = &w->level[depth];
    memset(l, 0, sizeof *l);
    l->type = box->type;
    if (box->type == HV_BOX_DTBL) {             /* the url boxes follow NDR */
        DataReferenceCount ndr;
        size_t used;
        if (HV_DECODE_USED(DataReferenceCount, &ndr, w->buf, box->payload,
                           box->end - box->payload, &used) != 0) {
            *at = box->start;
            return "dtbl.ndr-count";        /* no NDR */
        }
        w->tree->ndr = ndr;
        inner.payload += used;
    }
    hv_boxes_children(&children, w->buf, &inner);
    if ((error = tree_walk_boxes(w, &children, depth + 1, at)) != NULL) return error;
    return tree_end(w, box, depth, at);
}

/* The boxes in `container`, at `depth`, and below. */
static const char *tree_walk_boxes(tree_walk *w, hv_boxes *it, int depth, size_t *at) {
    hv_box box;
    const char *error;
    int status;
    while ((status = hv_boxes_next(it, &box, &error, at)) == 1)
        if ((error = tree_walk_box(w, &box, depth, at)) != NULL) return error;
    return status < 0 ? (depth > 0 ? "box.framing" : error) : NULL;
}

const char *hv_rule_box_placed(const uint8_t *buf, const hv_box *box, uint32_t parent,
                               int *ipr, size_t *at) {
    tree_walk w;
    hv_box_tree tree;
    const char *error;
    memset(&w, 0, sizeof w);
    memset(&tree, 0, sizeof tree);
    w.buf = buf;
    w.jpx = 1;
    w.tree = &tree;
    w.level[0].type = parent;
    error = tree_walk_box(&w, box, parent != 0, at);
    if (error == NULL) *ipr = tree.ipr;
    return error;
}

const char *hv_rule_box_tree(const uint8_t *buf, size_t size, int jpx, hv_box_tree *tree,
                             size_t *at) {
    tree_walk w;
    hv_boxes it;
    hv_extent *mdat = tree->mdat;
    size_t cap = tree->mdat_cap;
    const char *error;

    memset(tree, 0, sizeof *tree);
    tree->mdat = mdat;
    tree->mdat_cap = cap;
    memset(&w, 0, sizeof w);
    w.buf = buf;
    w.jpx = jpx;
    w.tree = tree;
    hv_boxes_file(&it, buf, size);
    if ((error = tree_walk_boxes(&w, &it, 0, at)) != NULL) return error;
    /* M.11.23: with a j2cx, a top-level codestream, and without creg
     * boxes, one for each top-level compositing layer. */
    if (jpx && tree->j2cx > 0 &&
        (w.top_streams == 0 || (w.cregs == 0 && w.top_jplh > w.top_streams))) {
        *at = size;
        return "j2cx.top-codestreams";
    }
    /* M.11.21: with a jclx, a comp, a top-level jpch and a top-level jplh. */
    if (jpx && tree->jclx > 0 && (w.comps == 0 || w.top_jpch == 0 || w.top_jplh == 0)) {
        *at = w.jclx_at;
        return "jclx.headers";
    }
    if (jpx && w.colrs == 0) {
        *at = size;
        return "jpx.colr";
    }
    return NULL;
}

/* The flst of each ftbl among the boxes of `it`, and in each j2cx. */
static const char *fragments_in(const uint8_t *buf, hv_boxes *it, const hv_box_tree *tree,
                                int jpxb, int *codestreams, size_t *at) {
    size_t n = tree->mdat_count < tree->mdat_cap ? tree->mdat_count : tree->mdat_cap;
    hv_box box;
    const char *error;
    while (hv_boxes_next(it, &box, &error, at) == 1) {
        hv_boxes children;
        hv_box flst;
        FragmentCount nf;
        Fragment f;
        uint64_t end = 0;
        size_t used, p, i;
        /* Codestream 0 (M.11.6: the numbering counts the jp2c and ftbl
         * boxes of a j2cx too). */
        int first = (box.type == HV_BOX_JP2C || box.type == HV_BOX_FTBL) &&
                    (*codestreams)++ == 0;
        /* The tree check has already rejected deeper superboxes. */
        if (box.type == HV_BOX_J2CX) {
            hv_boxes_children(&children, buf, &box);
            if ((error = fragments_in(buf, &children, tree, jpxb, codestreams, at)) !=
                NULL)
                return error;
            continue;
        }
        if (box.type != HV_BOX_FTBL) continue;
        hv_boxes_children(&children, buf, &box);
        /* The tree check established one flst and valid child framing. */
        while (hv_boxes_next(&children, &flst, &error, at) == 1) {
            if (flst.type != HV_BOX_FLST) continue;
            *at = flst.start;
            if (HV_DECODE_USED(FragmentCount, &nf, buf, flst.payload, flst.end - flst.payload,
                               &used) != 0)
                return decode_error;
            p = flst.payload + used;
            if ((flst.end - p) % HV_FIXED(Fragment) != 0 ||
                (flst.end - p) / HV_FIXED(Fragment) != nf)
                return "flst.nf-count";
            for (i = 0; i < nf; i++, p += HV_FIXED(Fragment)) {
                if (HV_DECODE(Fragment, &f, buf, p, HV_FIXED(Fragment)) != 0)
                    return decode_error;
                if ((error = hv_rule_fragment_dr(f.dr, tree->ndr, 0)) != NULL)
                    return error;
                if (f.dr == 0 &&
                    (error = hv_rule_fragment_here(tree->mdat, n, buf, f.off, f.len, i == 0)) != NULL)
                    return error;
                if (jpxb && first && (error = hv_rule_jpxb_fragment(&end, f.dr, f.off, f.len)) != NULL)
                    return error;
            }
        }
    }
    return NULL;
}

const char *hv_rule_fragments(const uint8_t *buf, size_t size, const hv_box_tree *tree,
                              int jpxb, size_t *at) {
    hv_boxes it;
    int codestreams = 0;
    hv_boxes_file(&it, buf, size);
    return fragments_in(buf, &it, tree, jpxb, &codestreams, at);
}

const char *hv_rule_fragment_here(const hv_extent *mdat, size_t n, const uint8_t *buf,
                                  uint64_t off, uint64_t len, int first) {
    size_t lo = 0, hi = n;
    /* The last mdat that starts at or before off. */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (mdat[mid].start <= off) lo = mid;
        else hi = mid;
    }
    if (n == 0 || mdat[lo].start > off || off > mdat[lo].end || len > mdat[lo].end - off)
        return "flst.mdat";
    if (first && (len == 0 || buf[off] != 0xFF || (len > 1 && buf[off + 1] != 0x4F)))
        return "flst.codestream-start";
    return NULL;
}

/* ------------------------------------------------------------------------
 * JPX boxes
 * ------------------------------------------------------------------------ */

const char *hv_rule_child(uint32_t parent, uint32_t child) {
    if (parent == HV_BOX_DTBL && child != HV_BOX_URL) return "dtbl.non-url";
    if (child == HV_BOX_JP2C) return parent == HV_BOX_JPCH ? "jpch.nested-jp2c" : "box.nested-jp2c";
    if (child == HV_BOX_JPCH || child == HV_BOX_FTBL || child == HV_BOX_DTBL)
        return "box.nested-top-level";
    if (child == HV_BOX_FLST && parent != HV_BOX_FTBL) return "box.flst-placement";
    if (child == HV_BOX_URL && parent != HV_BOX_DTBL) return "box.url-placement";
    return NULL;
}

const char *hv_rule_top_box(uint32_t type) {
    return type == HV_BOX_FLST ? "box.flst-placement"
         : type == HV_BOX_URL ? "box.url-placement" : NULL;
}

const char *hv_file_url_path(const uint8_t *loc, size_t n, char *out, size_t out_size) {
    int status;
    if (n <= HV_FILE_SCHEME_LENGTH) return "url.length";
    if (memcmp(loc, HV_FILE_SCHEME, HV_FILE_SCHEME_LENGTH) != 0) return "url.file-scheme";
    status = hv_url_path(loc + HV_FILE_SCHEME_LENGTH, n - HV_FILE_SCHEME_LENGTH, out, out_size);
    return status == -1 ? "url.percent-encoding"
         : status != 0 ? "linked path longer than the caller's buffer" : NULL;
}

const char *hv_rule_url(uint64_t vers, uint64_t flag, const uint8_t *loc, size_t n) {
    static const char jp2[] = ".jp2";
    if (vers != 0 || flag != 0) return "url.version-flags";
    if (n == 0 || memchr(loc, 0, n) != loc + n - 1) return "url.terminator";
    n--;                                           /* the characters */
    {
        const char *error = hv_file_url_path(loc, n, NULL, 0);
        if (error != NULL) return error;
    }
    if (n < sizeof jp2 - 1 || memcmp(loc + n - (sizeof jp2 - 1), jp2, sizeof jp2 - 1) != 0)
        return "url.jp2-target";
    return NULL;
}

static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int hv_url_path(const uint8_t *path, size_t n, char *out, size_t out_size) {
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        int c = path[i], hi, lo;
        if (c == '%') {
            if (n - i < 3 || (hi = hex_digit(path[i + 1])) < 0 ||
                (lo = hex_digit(path[i + 2])) < 0 || (c = hi * 16 + lo) == 0)
                return -1;
            i += 2;
        }
        if (out != NULL) {
            if (k + 1 >= out_size)
                return -2;
            out[k] = (char)c;
        }
        k++;
    }
    if (out != NULL) {
        if (out_size == 0)
            return -2;
        out[k] = 0;
    }
    return 0;
}

const char *hv_rule_flst(uint64_t nf, int fragments) {
    return nf == 1 && fragments == 1 ? NULL : "flst.one-fragment";
}

const char *hv_rule_fragment_dr(uint64_t dr, uint64_t ndr, int profile) {
    if (dr > ndr) return "flst.dr-range";
    if (profile && dr == 0) return "flst.dr-external";
    return NULL;
}

const char *hv_rule_jpx(const hv_jpx_boxes *b, int profile) {
    if (!profile && (b->rreq != 1 || !b->rreq_third)) return "jpx.reader-requirements";
    /* M.11.2: "A JPX file shall contain zero or one Data Reference boxes". */
    if (b->dtbl > 1) return "jpx.one-dtbl";
    /* M.11.6: as many codestreams (jp2c or ftbl) as codestream headers,
     * where there are any; without jpch, jp2h gives the header. */
    if (b->jpch > 0 && !b->extensions && b->jp2c + b->ftbl != b->jpch)
        return "jpx.codestream-count";
    if (profile) {
        if (b->jpch < 1) return "jpx.no-jpch";     /* ReadJPX needs jpch */
        if (b->jp2c > 0 && b->ftbl > 0) return "jpx.mixed-sources";
        if (b->ftbl > 0 && b->dtbl != 1) return "jpx.linked-shape";
    }
    return NULL;
}

const char *hv_rule_rreq_feature(uint64_t sf) {
    switch (sf) {                   /* Table M.14: "Deprecated" */
    case 3: case 8: case 12: case 18: case 19: case 20: case 22: case 24: case 27: case 29:
    case 31: case 42: case 43: case 45: case 46: case 63: case 64: case 65: case 66: case 70:
        return "rreq.deprecated-feature";
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------------
 * JP2 header boxes
 * ------------------------------------------------------------------------ */

const char *hv_rule_jp2h_place(const hv_box_tree *t, int jpx, int jpxb) {
    if (!jpx && t->jp2c == 0) return "jp2.one-codestream";
    if (!jpx && t->jp2h != 1) return "jp2.one-jp2h";
    if (jpx && t->jp2h > 1) return "jpx.one-jp2h";
    if (jpx ? jpxb && t->jp2h_late_baseline : t->jp2h_late) return "jp2h.position";
    return NULL;
}

void hv_header_init(hv_header *h, uint32_t parent, int jpx) {
    memset(h, 0, sizeof *h);
    h->parent = parent;
    h->jpx = jpx;
    h->jp2 = !jpx;
}

const char *hv_rule_header_child(hv_header *h, uint32_t type) {
    int *count = type == HV_BOX_IHDR ? &h->ihdr : type == HV_BOX_BPCC ? &h->bpcc
               : type == HV_BOX_PCLR ? &h->pclr : type == HV_BOX_CMAP ? &h->cmap
               : type == HV_BOX_CDEF ? &h->cdef : type == HV_BOX_RES ? &h->res : NULL;
    if (h->parent == HV_BOX_JP2H) {
        if (h->children == 0 && type != HV_BOX_IHDR) return "jp2h.ihdr-first";
        if (type == HV_BOX_COLR && h->colr_ended) return "jp2h.colr-contiguous";
        if (type != HV_BOX_COLR && h->colr > 0) h->colr_ended = 1;
    }
    h->children++;
    if (type == HV_BOX_COLR) h->colr++;
    if (type == HV_BOX_JP2I) h->ipr++;
    if (type == HV_BOX_CREG) h->creg++;
    if (type == HV_BOX_CGRP) h->cgrp++;
    if (count == NULL) return NULL;
    if (++*count > 1) {
        /* T.800 I.5.3: ihdr in other places is ignored; jpch has one. */
        if (type == HV_BOX_IHDR) return h->parent == HV_BOX_JPCH ? "header.one-ihdr" : NULL;
        return type == HV_BOX_BPCC ? "header.one-bpcc" : type == HV_BOX_PCLR ? "header.one-pclr"
             : type == HV_BOX_CMAP ? "header.one-cmap" : type == HV_BOX_CDEF ? "header.one-cdef"
             : "header.one-res";
    }
    if (type == HV_BOX_RES) h->resc = h->resd = 0;
    return NULL;
}

const char *hv_rule_extent(uint32_t type, uint64_t extra) {
    if (extra == 0) return NULL;
    switch (type) {
        case HV_BOX_IHDR: return "ihdr.extent";
        case HV_BOX_CDEF: return "cdef.extent";
        case HV_BOX_RESC: case HV_BOX_RESD: return "res.extent";
        case HV_BOX_RREQ: return "rreq.extent";
        default: return "box.extent";
    }
}

const char *hv_rule_ihdr(hv_header *h, const Ihdr *ihdr) {
    if (h->ihdr == 1) h->image = *ihdr;
    /* T.800 I.5.3.1: C "shall be 7"; T.801 Table M.19: 0 to 12. */
    if (h->jpx ? ihdr->c > 12 : ihdr->c != 7) return "ihdr.c";
    return hv_rule_extent(HV_BOX_IHDR, (uint64_t)ihdr->extra.nCount);
}

void hv_rule_bpcc(hv_header *h, const uint8_t *depths, size_t n) {
    if (h->bpcc != 1) return;
    h->bpcc_depths = depths;
    h->bpcc_count = n;
}

/* The bytes T.801 M.11.7.4 lets follow EnumCS in a JPX file (EP): RL, OL,
 * RA, OA, RB, OB and IL for CIELab (14), RJ, OJ, RA, OA, RB and OB for
 * CIEJab (19), 4 bytes each; or none, the defaults. */
static int jpx_ep_length(uint64_t enumcs, size_t rest) {
    return rest == 0 || (enumcs == 14 && rest == 28) || (enumcs == 19 && rest == 24);
}

/* ICC signatures (ICC.1:1998-09): the header's, the class, colourspaces
 * and PCS a restricted profile has, and the tags each class requires. */
#define ICC_SIG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))
enum { ICC_HEADER = 128, ICC_TAG = 12 };

/* A restricted ICC profile (T.800 I.5.3.3, Table I.9): the n bytes of
 * PROFILE. *colours is the colourspace's colours, 1 or 3. */
static const char *restricted_icc(const uint8_t *p, size_t n, unsigned *colours) {
    static const uint32_t gray[] = { ICC_SIG('d','e','s','c'), ICC_SIG('k','T','R','C'),
                                     ICC_SIG('w','t','p','t'), ICC_SIG('c','p','r','t') };
    static const uint32_t rgb[] = { ICC_SIG('d','e','s','c'), ICC_SIG('r','X','Y','Z'),
                                    ICC_SIG('g','X','Y','Z'), ICC_SIG('b','X','Y','Z'),
                                    ICC_SIG('r','T','R','C'), ICC_SIG('g','T','R','C'),
                                    ICC_SIG('b','T','R','C'), ICC_SIG('w','t','p','t'),
                                    ICC_SIG('c','p','r','t') };
    const uint32_t *required;
    size_t count, tags, i, k;
    uint32_t space;
    if (n < ICC_HEADER + 4 || be32(p) != n || be32(p + 36) != ICC_SIG('a','c','s','p'))
        return "colr.icc-header";
    space = be32(p + 16);
    if (be32(p + 12) != ICC_SIG('s','c','n','r') ||
        (space != ICC_SIG('G','R','A','Y') && space != ICC_SIG('R','G','B',' ')))
        return "colr.icc-class";
    if (be32(p + 20) != ICC_SIG('X','Y','Z',' ')) return "colr.icc-pcs";
    *colours = space == ICC_SIG('G','R','A','Y') ? 1 : 3;
    required = *colours == 1 ? gray : rgb;
    count = *colours == 1 ? sizeof gray / sizeof *gray : sizeof rgb / sizeof *rgb;
    tags = be32(p + ICC_HEADER);
    if (tags > (n - ICC_HEADER - 4) / ICC_TAG) return "colr.icc-tags";
    for (i = 0; i < tags; i++) {
        const uint8_t *e = p + ICC_HEADER + 4 + i * ICC_TAG;
        uint32_t offset = be32(e + 4), size = be32(e + 8);
        if (offset > n || size > n - offset) return "colr.icc-tags";
    }
    for (k = 0; k < count; k++) {
        for (i = 0; i < tags && be32(p + ICC_HEADER + 4 + i * ICC_TAG) != required[k]; i++)
            ;
        if (i == tags) return "colr.icc-tags";
    }
    return NULL;
}

/* The colours of an enumerated colourspace of T.800 (Table I.18). */
static unsigned enumerated_colours(uint64_t enumcs) {
    return enumcs == 16 || enumcs == 18 ? 3 : enumcs == 17 ? 1 : 0;
}

/* T.801 M.9.2.4: the methods a baseline file's first layer uses. */
static int baseline_method(const ColrHeader *c) {
    if (c->meth == 2 || c->meth == 3) return 1;
    return c->meth == 1 && (c->enumcs == 16 || c->enumcs == 17 || c->enumcs == 21 ||
                            c->enumcs == 18 || c->enumcs == 20 || c->enumcs == 24 ||
                            c->enumcs == 14 || c->enumcs == 19);
}

const char *hv_rule_colr(hv_header *h, const ColrHeader *colr, const uint8_t *rest, size_t n) {
    unsigned colours = 0;
    const char *r;
    if (colr->meth == 1 && (h->jpx ? !jpx_ep_length(colr->enumcs, n) : n != 0))
        return "colr.enumcs-length";
    /* T.800 I.5.3.3, Table I.11: in a JP2 file PREC and APPROX "shall be
     * set to zero" (a reader ignores them). */
    if (!h->jpx && (colr->prec != 0 || colr->approx != 0))
        return "colr.prec-approx";
    /* T.801 M.11.7.2, Table M.23: APPROX 1 to 4 ("a value of 0 in the
     * APPROX field is illegal in a JPX file"), for the methods T.801
     * defines (Table M.22: a reader ignores a colr of another METH). */
    if (h->jpx && colr->meth >= 1 && colr->meth <= 5 && (colr->approx < 1 || colr->approx > 4))
        return "colr.approx";
    if (h->jp2 && h->parent == HV_BOX_JP2H && h->colr == 1) {
        /* T.800 I.5.3.3: METH "shall be 1 or 2"; EnumCS 16 (sRGB), 17
         * (greyscale) or 18 (sYCC) in the first colr box. */
        if (colr->meth != 1 && colr->meth != 2) return "colr.method";
        if (colr->meth == 1 && colr->enumcs != 16 && colr->enumcs != 17 && colr->enumcs != 18)
            return "colr.enumcs";
    }
    if (colr->meth == 2 && (r = restricted_icc(rest, n, &colours)) != NULL) return r;
    if (colr->meth == 1) colours = enumerated_colours(colr->enumcs);
    if (h->colr == 1) {
        h->colours = colours;
        h->rgb = (colr->meth == 1 && colr->enumcs == 16) || (colr->meth == 2 && colours == 3);
    }
    h->baseline_colr |= baseline_method(colr);
    h->approx_colr |= colr->approx >= 1 && colr->approx <= 3;
    if (h->jpx && (h->parent == HV_BOX_JP2H || h->parent == HV_BOX_CGRP)) {
        if ((colr->meth == 1 && h->meth1++ > 0) || (colr->meth == 2 && h->meth2++ > 0))
            return "colr.one-method";
    }
    return NULL;
}

void hv_rule_pclr(hv_header *h, uint64_t ne, uint64_t npc) {
    h->pclr_ne = ne;
    h->pclr_columns = 0;
    if (h->pclr == 1) h->npc = (uint32_t)npc;
}

void hv_rule_pclr_column(hv_header *h, uint64_t depth) {
    if (h->pclr_columns < sizeof h->pclr_column_depths) {
        h->pclr_column_depths[h->pclr_columns] = (uint8_t)depth;
        if (h->pclr == 1) h->pclr_depths[h->pclr_columns] = (uint8_t)depth;
        h->pclr_columns++;
    }
}

/* The bytes of one palette value of a column of this BitDepth. */
static unsigned value_bytes(uint8_t depth) {
    return ((depth & 0x7Fu) + 1 + 7) / 8;
}

const char *hv_rule_pclr_end(hv_header *h, const uint8_t *entries, uint64_t n) {
    uint64_t entry = 0, j, i;
    for (i = 0; i < h->pclr_columns; i++) entry += value_bytes(h->pclr_column_depths[i]);
    if (n != entry * h->pclr_ne) return "pclr.entries-length";
    /* I.5.3.4: each value "is padded with zeros to a multiple of 8 bits
     * and the actual value shall be stored in the low-order bits". */
    for (j = 0; j < h->pclr_ne; j++)
        for (i = 0; i < h->pclr_columns; i++) {
            uint8_t depth = h->pclr_column_depths[i];
            unsigned bits = (depth & 0x7Fu) + 1, pad = 8 * value_bytes(depth) - bits;
            if (pad > 0 && (*entries >> (8 - pad)) != 0) return "pclr.padding";
            entries += value_bytes(depth);
        }
    return NULL;
}

const char *hv_rule_cmap_entry(hv_header *h, const CmapEntry *entry) {
    if (entry->mtyp == 0 && entry->pcol != 0) return "cmap.pcol-zero";
    if (h->cmap != 1) return NULL;
    if (h->cmap_count < UINT32_MAX) h->cmap_count++;
    if (entry->cmp > h->cmap_cmp) h->cmap_cmp = (uint32_t)entry->cmp;
    if (entry->mtyp == 1) {
        h->cmap_palette = 1;
        if (entry->pcol > h->cmap_pcol) h->cmap_pcol = (uint32_t)entry->pcol;
    }
    return NULL;
}

void hv_rule_cmap_end(hv_header *h, const uint8_t *entries, size_t n) {
    if (h->cmap == 1) h->cmap_entries = entries;
    (void)n;                    /* cmap_count, from hv_rule_cmap_entry */
}

/* Keys for sorting cdef entries: (Typ, Asoc, Cn) for cdef.pairs, and
 * (Asoc, Cn) of the opacity channels for cdef.opacity. */
static uint64_t pair_key(const CdefEntry *e) {
    return (uint64_t)e->typ << 32 | (uint64_t)e->asoc << 16 | (uint64_t)e->cn;
}

static int opacity(const CdefEntry *e) {
    return (e->typ == 1 || e->typ == 2) && e->asoc != 65535;
}

static int compare_keys(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Two entries that break cdef.pairs: two channels with the same Typ and
 * Asoc, neither 65,535. */
static int same_pair(const CdefEntry *a, const CdefEntry *b) {
    return a->typ == b->typ && a->asoc == b->asoc && a->typ != 65535 && a->asoc != 65535 &&
           a->cn != b->cn;
}

/* Two entries that break cdef.opacity: two opacity channels for one
 * colour, the whole image's (Asoc 0) standing for every colour. */
static int two_opacities(const CdefEntry *a, const CdefEntry *b) {
    return opacity(a) && opacity(b) && a->cn != b->cn &&
           (a->asoc == b->asoc || a->asoc == 0 || b->asoc == 0);
}

const char *hv_rule_cdef(hv_header *h, const CdefEntry *entries, size_t n, const uint8_t *raw,
                         uint64_t *scratch) {
    size_t i, k, m;
    uint64_t whole = UINT64_MAX;    /* the Cn of an opacity channel for the whole image */
    if (h->cdef == 1) {
        h->cdef_entries = raw;
        h->cdef_count = n;
        for (i = 0; i < n; i++) {
            if (entries[i].cn > h->cdef_cn) h->cdef_cn = (uint32_t)entries[i].cn;
            h->opacity |= entries[i].typ == 1;
            h->premultiplied |= entries[i].typ == 2;
        }
    }
    /* M.11.7.5 relaxes both JP2 channel restrictions for headers a JP2
     * reader does not use. In a JP2-compatible JPX file, only jp2h is
     * parsed by that reader; its jpch/jplh headers describe JPX use. */
    if (!h->jp2) return NULL;
    if (n <= HV_CDEF_PAIRWISE) {
        for (i = 0; i < n; i++)
            for (k = 0; k < i; k++)
                if (same_pair(&entries[i], &entries[k])) return "cdef.pairs";
        for (i = 0; i < n; i++)
            for (k = 0; k < i; k++)
                if (two_opacities(&entries[i], &entries[k])) return "cdef.opacity";
        return NULL;
    }
    for (i = m = 0; i < n; i++)
        if (entries[i].typ != 65535 && entries[i].asoc != 65535)
            scratch[m++] = pair_key(&entries[i]);
    qsort(scratch, m, sizeof *scratch, compare_keys);
    for (i = 1; i < m; i++)
        if (scratch[i] >> 16 == scratch[i - 1] >> 16 && scratch[i] != scratch[i - 1])
            return "cdef.pairs";
    for (i = m = 0; i < n; i++)
        if (opacity(&entries[i])) {
            if (entries[i].asoc == 0) {
                if (whole != UINT64_MAX && whole != entries[i].cn) return "cdef.opacity";
                whole = entries[i].cn;
            }
            scratch[m++] = (uint64_t)entries[i].asoc << 16 | entries[i].cn;
        }
    qsort(scratch, m, sizeof *scratch, compare_keys);
    for (i = 0; i < m; i++) {
        if (whole != UINT64_MAX && (scratch[i] & 0xFFFF) != whole) return "cdef.opacity";
        if (i > 0 && scratch[i] >> 16 == scratch[i - 1] >> 16 && scratch[i] != scratch[i - 1])
            return "cdef.opacity";
    }
    return NULL;
}

void hv_rule_res_child(hv_header *h, uint32_t type) {
    h->resc += type == HV_BOX_RESC;
    h->resd += type == HV_BOX_RESD;
}

const char *hv_rule_res_end(hv_header *h) {
    return h->resc + h->resd == 0 ? "res.resc-resd" : NULL;
}

void hv_rule_cgrp_child(hv_header *g, uint32_t type) {
    if (type == HV_BOX_COLR) g->colr++;
}

const char *hv_rule_cgrp_end(hv_header *h, const hv_header *g) {
    if (g->colr == 0) return "cgrp.empty";
    h->cgrp_colr += g->colr;
    h->cgrp_baseline |= g->baseline_colr;
    h->cgrp_approx |= g->approx_colr;
    return NULL;
}

/* The box of a codestream's header: its own, or else the default. */
static const hv_header *pick(const hv_header *h, const hv_header *d, int present_h, int present_d) {
    return present_h ? h : present_d ? d : NULL;
}

/* Whether a JP2 channel's samples are signed (I.5.3.5, I.5.3.6): those
 * of the component or palette column the cmap maps it from, else of
 * component `cn`, as bpcc or ihdr give them. -1 where the boxes do not
 * say (a rule on them fails then). */
static int channel_signed(const hv_header *h, uint64_t cn) {
    uint64_t component = cn;
    if (h->cmap_entries != NULL) {
        CmapEntry e;
        if (cn >= h->cmap_count ||
            HV_DECODE(CmapEntry, &e, h->cmap_entries, (size_t)cn * HV_FIXED(CmapEntry),
                      HV_FIXED(CmapEntry)) != 0)
            return -1;
        if (e.mtyp == 1) return e.pcol < h->npc ? h->pclr_depths[e.pcol] >> 7 : -1;
        component = e.cmp;
    }
    if (h->image.bpc != 255) return (int)(h->image.bpc >> 7);
    return h->bpcc_depths != NULL && component < h->bpcc_count
               ? h->bpcc_depths[component] >> 7 : -1;
}

/* The channel rules of a JP2 file's jp2h (T.800 I.5.3.6), on the entries
 * of its cdef, with `mct` the codestream's multiple component transform. */
static const char *jp2_channels(const hv_header *h, int mct, uint64_t channels) {
    uint8_t defined[8192];       /* Cn is 16 bits; duplicate entries are legal. */
    size_t i, count = 0;
    int colour[4] = { 0, 0, 0, 0 };     /* R, G, B as channels 0, 1, 2 */
    unsigned colours = 0;
    if (h->cdef) memset(defined, 0, sizeof defined);
    for (i = 0; i < h->cdef_count; i++) {
        CdefEntry e;
        if (HV_DECODE(CdefEntry, &e, h->cdef_entries, i * HV_FIXED(CdefEntry),
                      HV_FIXED(CdefEntry)) != 0)
            continue;               /* hv_rule_cdef's caller decoded it */
        /* "All opacity channels shall be mapped from unsigned components." */
        if ((e.typ == 1 || e.typ == 2) && channel_signed(h, e.cn) == 1)
            return "cdef.opacity-signed";
        if (e.asoc != 0 && e.asoc != 65535 && h->colours != 0 && e.asoc > h->colours)
            return "cdef.asoc";
        if (!(defined[e.cn / 8] & (1u << (e.cn % 8)))) {
            defined[e.cn / 8] |= (uint8_t)(1u << (e.cn % 8));
            count++;
        }
        if (e.typ == 0 && e.asoc >= 1 && e.asoc <= h->colours)
            colours |= 1u << (unsigned)(e.asoc - 1);
        if (e.typ == 0 && e.asoc >= 1 && e.asoc <= 3 && e.cn == e.asoc - 1)
            colour[e.asoc] = 1;
    }
    /* "If a multiple component transform is specified within the
     * codestream, the image must be in an RGB colourspace and the red,
     * green and blue colours as channels 0, 1 and 2 in the codestream,
     * respectively": the colourspace, and cdef, if any, mapping them so. */
    if (mct == 1 && (!h->rgb || (h->cdef && !(colour[1] && colour[2] && colour[3]))))
        return "jp2.mct-colourspace";
    if (h->cdef && (count != channels || colours != (1u << h->colours) - 1))
        return "cdef.complete";
    return NULL;
}

const char *hv_rule_codestream_header(const hv_header *h, const hv_header *d, const hv_siz *siz,
                                      int mct, int jpx) {
    static const hv_header none;
    const hv_header *ihdr, *bpcc, *pclr, *cmap;
    const SizFixed *s;
    const char *r;
    size_t i;
    int same = 1;
    uint64_t nc, ssiz0 = 0;

    if (h == NULL) h = &none;
    if (d == NULL) d = &none;
    ihdr = pick(h, d, h->ihdr, d->ihdr);
    pclr = pick(h, d, h->pclr, d->pclr);
    cmap = pick(h, d, h->cmap, d->cmap);
    if (ihdr == NULL) return jpx ? "jpch.ihdr" : "jp2h.ihdr-first";
    /* T.801 M.11.5.1: BPC 255 and a bpcc in the ihdr's own box. */
    bpcc = ihdr->bpcc ? ihdr : NULL;
    nc = ihdr->image.nc;
    /* T.801 M.11.6: with IPR 0 in the codestream's ihdr, its jpch holds no
     * IPR box. */
    if (jpx && h->parent == HV_BOX_JPCH && h->ipr > 0 && ihdr->image.ipr == 0)
        return "jpch.ipr";

    if (!jpx) {
        if (h->colr == 0) return "jp2h.colr";
        if ((ihdr->image.bpc == 255) != (bpcc != NULL)) return "ihdr.bpcc";
        if ((pclr != NULL) != (cmap != NULL)) return "header.pclr-cmap";
    } else {
        if (ihdr->image.bpc == 255 && bpcc == NULL) return "ihdr.bpcc";
        if ((pclr != NULL && cmap == NULL) || (cmap != NULL && cmap->cmap_palette && pclr == NULL))
            return "header.pclr-cmap";
    }
    if (bpcc != NULL && ihdr->image.bpc == 255 && bpcc->bpcc_count != nc) return "bpcc.count";
    if (cmap != NULL) {
        if (cmap->cmap_cmp >= nc) return "cmap.component";
        if (cmap->cmap_palette && pclr != NULL && cmap->cmap_pcol >= pclr->npc)
            return "cmap.palette-column";
    }
    if (!jpx) {
        if (h->cdef && h->cdef_cn >= (cmap != NULL ? cmap->cmap_count : nc)) return "cdef.channel";
        if ((r = jp2_channels(h, mct, cmap != NULL ? cmap->cmap_count : nc)) != NULL) return r;
    }

    if (siz == NULL) return NULL;
    s = siz->fixed;
    /* T.800 I.5.3.1 and Table A.10: a JP2 file holds a T.800 codestream. */
    if (!jpx && part2_rsiz(s->rsiz)) return "jp2.rsiz";
    /* A JPEG 2000 codestream at hand: C 7 (Table M.19). */
    if (ihdr->image.c != 7) return "ihdr.c";
    if (ihdr->image.height != s->ysiz - s->yosiz) return "ihdr.height";
    if (ihdr->image.width != s->xsiz - s->xosiz) return "ihdr.width";
    if (nc != s->csiz) return "ihdr.nc";
    /* T.801 M.11.5.1, NOTE: the depths after the Part 2 multiple component
     * (Table A.2: 1x00 xxx1 xxxx xxxx) or non-linear point (1x00 xx1x xxxx
     * xxxx) transformation. */
    if (jpx && part2_rsiz(s->rsiz) && (s->rsiz & 0x0300) != 0) return NULL;
    for (i = 0; i < siz->ncomponents; i++) {
        const Component *k = &siz->components[i];
        uint64_t ssiz = k->depthMinus1 | (uint64_t)k->isSigned << 7;
        if (i == 0) ssiz0 = ssiz;
        same &= ssiz == ssiz0;
        if (bpcc != NULL && ihdr->image.bpc == 255 && i < bpcc->bpcc_count &&
            bpcc->bpcc_depths[i] != ssiz)
            return "bpcc.depth";
    }
    if (ihdr->image.bpc != (same ? ssiz0 : 255)) return "ihdr.bpc";
    return NULL;
}

const char *hv_rule_ihdr_ipr(const hv_header *jp2h, int ipr_boxes) {
    if (jp2h->ihdr > 0 && (jp2h->image.ipr == 1) != (ipr_boxes > 0)) return "ihdr.ipr";
    return NULL;
}

const char *hv_rule_jpx_ipr(const hv_header *h, const hv_header *d, int ipr_boxes) {
    static const hv_header none;
    const hv_header *ihdr;
    if (h == NULL) h = &none;
    if (d == NULL) d = &none;
    ihdr = pick(h, d, h->ihdr, d->ihdr);
    if (ihdr != NULL && ihdr->image.ipr == 1 && ipr_boxes == 0) return "jpx.ipr";
    return NULL;
}

const char *hv_rule_jpx_creg(int jplh, int with_creg) {
    return with_creg > 0 && with_creg != jplh ? "jpx.creg" : NULL;
}

const char *hv_rule_jpxb_layer(const hv_header *jp2h, const hv_header *jpch,
                               const hv_header *jplh) {
    static const hv_header none;
    const hv_header *headers[2];
    int i, baseline, approx;
    if (jp2h == NULL) jp2h = &none;
    if (jplh != NULL && jplh->cgrp > 0) {
        baseline = jplh->cgrp_baseline;
        approx = jplh->cgrp_approx;
    } else {
        baseline = jp2h->baseline_colr;
        approx = jp2h->approx_colr;
    }
    if (!baseline || !approx) return "jpxb.colour";
    headers[0] = jpch;
    headers[1] = jplh;
    for (i = 0; i < 2; i++) {
        const hv_header *x = headers[i];
        if (x == NULL) continue;
        if ((jp2h->ihdr && x->ihdr) || (jp2h->bpcc && x->bpcc) ||
            (jp2h->colr && (x->colr || x->cgrp_colr)) || (jp2h->pclr && x->pclr) ||
            (jp2h->cmap && x->cmap) || (jp2h->cdef && x->cdef) || (jp2h->res && x->res))
            return "jpxb.header-override";
    }
    return NULL;
}

const char *hv_rule_jpxb_fragment(uint64_t *end, uint64_t dr, uint64_t off, uint64_t len) {
    if (dr != 0 || off < *end) return "jpxb.fragments";
    *end = off + len;
    return NULL;
}
