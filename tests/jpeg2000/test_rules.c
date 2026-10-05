/* Validation boundaries from T.800/T.801 and the served profile. Each
 * failing case has an accepted control and asserts the specific rule.
 * These public-rule tests complement the reader's whole-file corpus. */
#include "jpeg2000/hv_rules.h"
#include "jpeg2000/hv_writer.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL %s\n", what); failures++; }
}
static void rule(const char *got, const char *want, const char *what) {
    if ((got == NULL) != (want == NULL) || (got && strcmp(got, want))) {
        printf("FAIL %s: got %s, expected %s\n", what, got ? got : "valid", want ? want : "valid");
        failures++;
    }
}

/* T.800 A.5.1: origins are inside the image, and the first tile must
 * cover the image origin. Exercise the two axes independently. */
static void siz_boundaries(void) {
    SizFixed f = {0};
    Component components[3] = {{0}};
    hv_siz s = {0};
    Sgcod cod = {0};
    int axis, delta, i, field;
    f.xsiz = f.ysiz = f.xtsiz = f.ytsiz = 8;
    f.csiz = 3;
    s.fixed = &f; s.components = components; s.ncomponents = 3;
    for (i = 0; i < 3; i++) {
        components[i].depthMinus1 = 7;
        components[i].xrsiz = components[i].yrsiz = 1;
    }
    for (axis = 0; axis < 2; axis++) {
        for (delta = -1; delta <= 1; delta++) {
            f.xosiz = axis == 0 ? (uint64_t)(8 + delta) : 0;
            f.yosiz = axis == 1 ? (uint64_t)(8 + delta) : 0;
            rule(hv_rule_siz(&s, NULL, 0), delta < 0 ? NULL : "siz.origin-inside", "image origin");
            f.xosiz = f.yosiz = 4;
            f.xtsiz = axis == 0 ? (uint64_t)(4 + delta) : 8;
            f.ytsiz = axis == 1 ? (uint64_t)(4 + delta) : 8;
            rule(hv_rule_siz(&s, NULL, 0), delta > 0 ? NULL : "siz.tile-covers-origin", "tile end");
            f.xtsiz = f.ytsiz = 8;
            f.xtosiz = axis == 0 ? (uint64_t)(4 + delta) : 0;
            f.ytosiz = axis == 1 ? (uint64_t)(4 + delta) : 0;
            rule(hv_rule_siz(&s, NULL, 0), delta <= 0 ? NULL : "siz.tile-origin", "tile origin");
            f.xtosiz = f.ytosiz = 0;
        }
    }
    f.xosiz = f.yosiz = 0;
    cod.mct = 1;
    rule(hv_rule_siz(&s, &cod, 0), NULL, "matching MCT components");
    for (i = 1; i < 3; i++) for (field = 0; field < 3; field++) {
        components[i].depthMinus1 = field == 0 ? 8 : 7;
        components[i].xrsiz = field == 1 ? 2 : 1;
        components[i].yrsiz = field == 2 ? 2 : 1;
        rule(hv_rule_siz(&s, &cod, 0), "siz.mct-geometry", "one MCT component differs");
        components[i] = components[0];
    }
    for (field = 0; field < 4; field++) {
        f.xosiz = field == 0 || field == 2;
        f.yosiz = field == 1 || field == 3;
        f.xtosiz = field == 2; f.ytosiz = field == 3;
        rule(hv_rule_siz(&s, NULL, 1), "siz.zero-origin", "profile origins");
    }
}

/* T.800:2019 Table A.10: recognized declarations are distinguished from
 * reserved values without claiming to validate unimplemented profiles. */
static void rsiz_boundaries(void) {
    SizFixed f = {0};
    Component component = {0};
    hv_siz siz = {&f, &component, 1};
    static const unsigned supported[] = {0, 1, 2, 0x8000, 0x8100, 0xC000};
    static const unsigned unvalidated[] = {
        3, 4, 5, 6, 7, 0x0100, 0x010B, 0x0200, 0x020B,
        0x0306, 0x0307, 0x0400, 0x0411, 0x0413, 0x0424,
        0x049B, 0x0500, 0x0600, 0x0700, 0x0800, 0x0900, 0x099B, 0x0FFF
    };
    static const unsigned reserved[] = {
        8, 0x00FF, 0x010C, 0x0110, 0x020C, 0x0305, 0x0308,
        0x0421, 0x0434, 0x04AB, 0x040C, 0x0A00, 0x0FFE, 0x1000
    };
    size_t i;
    f.xsiz = f.ysiz = f.xtsiz = f.ytsiz = 8;
    f.csiz = 1;
    component.xrsiz = component.yrsiz = 1;
    for (i = 0; i < sizeof supported / sizeof *supported; i++) {
        f.rsiz = supported[i];
        rule(hv_rule_siz(&siz, NULL, 0), NULL, "implemented Rsiz declaration");
    }
    for (i = 0; i < sizeof unvalidated / sizeof *unvalidated; i++) {
        f.rsiz = unvalidated[i];
        rule(hv_rule_siz(&siz, NULL, 0), "siz.unsupported-profile", "unvalidated Part 1 profile");
        rule(hv_rule_siz(&siz, NULL, 1), NULL, "server preserves unvalidated profile");
    }
    for (i = 0; i < sizeof reserved / sizeof *reserved; i++) {
        f.rsiz = reserved[i];
        rule(hv_rule_siz(&siz, NULL, 0), "siz.rsiz", "reserved Rsiz declaration");
        rule(hv_rule_siz(&siz, NULL, 1), NULL, "server preserves other declarations");
    }
    f.rsiz = 0x4000;
    rule(hv_rule_siz(&siz, NULL, 0), "siz.unsupported-capabilities", "CAP capabilities unvalidated");
    rule(hv_rule_siz(&siz, NULL, 1), NULL, "server preserves CAP signalling");
}

static void plt_boundaries(void) {
    SizFixed f = {0};
    hv_siz s = {0};
    Sgcod cod = {0};
    Spcod sp = {0};
    hv_zplt z = {0};
    unsigned i;
    int delta;
    /* A complete permutation need not arrive in Z order at the standard
     * layer. A duplicate must fail before the final completeness check. */
    for (i = 256; i > 0;) {
        --i;
        rule(hv_rule_zplt(&z, i, 0), NULL, "all PLT indices descending");
    }
    rule(hv_rule_zplt(&z, 255, 0), "plt.zplt-index", "duplicate PLT index");
    for (delta = -1; delta <= 1; delta++) {
        rule(hv_rule_plt_coverage((uint64_t)(20 + delta), 20, 0),
             delta == 0 ? NULL : "plt.coverage", "PLT coverage without SOP");
        rule(hv_rule_plt_coverage((uint64_t)(8 + delta), 20, 2),
             delta == 0 ? NULL : "plt.coverage", "PLT coverage excluding two SOPs");
        rule(hv_rule_plt_coverage((uint64_t)(20 + delta), 20, 2),
             delta == 0 ? NULL : "plt.coverage", "PLT coverage including SOPs");
    }
    s.fixed = &f; f.csiz = 1; f.ysiz = 1; cod.layers = 1;
    sp.precincts.nCount = 1; /* level 0, precincts of one sample */
    for (delta = -1; delta <= 1; delta++) {
        f.xsiz = (uint64_t)(2147483647LL + delta);
        check(hv_rule_packets(&s, &cod, &sp) == (delta > 0 ? 0 : f.xsiz), "packet count limit");
    }
    f.xsiz = 3; f.ysiz = 5; f.csiz = 2; cod.layers = 3;
    sp.levels = 1; sp.precincts.nCount = 2;
    check(hv_rule_packets(&s, &cod, &sp) == 126, "packet count rounds each resolution");
}

/* M.11.3.1: local fragments must be contained in one mdat. The first
 * fragment starts at SOC, allowing the marker to span fragments. */
static void fragment_boundaries(void) {
    uint8_t buf[64] = {0};
    hv_extent mdat[] = {{12, 20}, {28, 36}, {44, 52}};
    size_t i;
    for (i = 0; i < 3; i++) {
        uint64_t start = mdat[i].start, end = mdat[i].end;
        buf[start] = 0xFF; buf[start + 1] = 0x4F;
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 8, 1), NULL, "whole mdat fragment");
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 9, 0), "flst.mdat", "fragment past end");
        rule(hv_rule_fragment_here(mdat, 3, buf, start - 1, 1, 0), "flst.mdat", "fragment before start");
        rule(hv_rule_fragment_here(mdat, 3, buf, end, 0, 0), NULL, "empty fragment at end");
        rule(hv_rule_fragment_here(mdat, 3, buf, end + 1, 0, 0), "flst.mdat", "empty fragment past end");
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 0, 1), "flst.codestream-start", "empty first fragment");
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 1, 1), NULL, "split SOC marker");
        buf[start + 1] = 0;
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 2, 1), "flst.codestream-start", "bad SOC second byte");
        buf[start] = 0;
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 1, 1), "flst.codestream-start", "bad SOC first byte");
        rule(hv_rule_fragment_here(mdat, 3, buf, start, 2, 0), NULL, "later fragment needs no SOC");
    }
    rule(hv_rule_fragment_here(NULL, 0, buf, 12, 1, 1), "flst.mdat", "no mdat");
    rule(hv_rule_fragment_here(mdat, 3, buf, UINT64_MAX, 1, 0), "flst.mdat", "offset beyond file");
    rule(hv_rule_fragment_here(mdat, 3, buf, 12, UINT64_MAX, 0), "flst.mdat", "length beyond file");
}

static void url_boundaries(void) {
    static const uint8_t url[] = "file://a.jp2";
    static const char *bad[] = {"%", "%0", "%00", "%/1", "%:1", "%@1", "%G1", "%`1", "%g1",
                                "%1/", "%1:", "%1@", "%1G", "%1`", "%1g"};
    unsigned i, upper;
    char encoded[4], out[2];
    rule(hv_rule_url(1, 0, url, sizeof url), "url.version-flags", "URL version alone");
    rule(hv_rule_url(0, 1, url, sizeof url), "url.version-flags", "URL flags alone");
    rule(hv_rule_url(0, 0, url, sizeof url), NULL, "URL control");
    for (upper = 0; upper < 2; upper++) for (i = 1; i <= 255; i++) {
        snprintf(encoded, sizeof encoded, upper ? "%%%02X" : "%%%02x", i);
        check(hv_url_path((const uint8_t *)encoded, 3, out, sizeof out) == 0 &&
              (unsigned char)out[0] == i && out[1] == 0, "percent byte and terminator");
        check(hv_url_path((const uint8_t *)encoded, 3, out, 1) == -2, "decoded buffer one short");
        check(hv_url_path((const uint8_t *)encoded, 3, NULL, 0) == 0, "validate without output");
    }
    for (i = 0; i < sizeof bad / sizeof *bad; i++)
        check(hv_url_path((const uint8_t *)bad[i], strlen(bad[i]), out, sizeof out) == -1,
              "invalid percent encoding");
    check(hv_url_path(url, 0, out, 1) == 0 && out[0] == 0, "empty decoded path");
    check(hv_url_path(url, 0, out, 0) == -2, "no room for terminator");
}

/* I.5.3.4: unused high bits in each stored palette value are zero,
 * including signed values. Cross byte boundaries and both entries. */
static void palette_boundaries(void) {
    static const unsigned bits[] = {1, 7, 8, 9, 15, 16, 17, 32, 38};
    static const unsigned widths[] = {1, 1, 1, 2, 2, 2, 3, 4, 5};
    unsigned i, sign, entry;
    for (i = 0; i < sizeof bits / sizeof *bits; i++) for (sign = 0; sign < 2; sign++) {
        hv_header h = {0};
        uint8_t values[10] = {0};
        unsigned width = widths[i], high = bits[i] % 8;
        h.pclr = 1; h.pclr_ne = 2;
        hv_rule_pclr_column(&h, bits[i] - 1 + 128 * sign);
        check(h.pclr_columns == 1 && h.pclr_column_depths[0] == bits[i] - 1 + 128 * sign,
              "palette column recorded");
        for (entry = 0; entry < 2; entry++) {
            memset(values, 0, sizeof values);
            values[entry * width] = high ? (uint8_t)((1u << high) - 1) : 255;
            rule(hv_rule_pclr_end(&h, values, width * 2), NULL, "palette high data bits");
            if (high) {
                values[entry * width] = (uint8_t)(1u << high);
                rule(hv_rule_pclr_end(&h, values, width * 2), "pclr.padding", "first padding bit");
            }
        }
        rule(hv_rule_pclr_end(&h, values, width * 2 - 1), "pclr.entries-length", "short palette");
        rule(hv_rule_pclr_end(&h, values, width * 2 + 1), "pclr.entries-length", "long palette");
    }
}

/* T.801 M.11.7.4: optional CIELab/CIEJab parameters have exact sizes.
 * M.11.7.2 gives APPROX 1..4 for the defined methods. */
static void colour_boundaries(void) {
    static const uint64_t spaces[] = {14, 19, 16, 17, 18, 20, 21, 24};
    uint8_t rest[29] = {0};
    unsigned i, n, approx;
    for (i = 0; i < sizeof spaces / sizeof *spaces; i++) for (n = 0; n <= 29; n++) {
        hv_header h = {0};
        ColrHeader c = {0};
        int valid = n == 0 || (spaces[i] == 14 && n == 28) || (spaces[i] == 19 && n == 24);
        h.jpx = 1; h.colr = 1;
        c.meth = 1; c.approx = 1; c.enumcs = spaces[i];
        rule(hv_rule_colr(&h, &c, rest, n), valid ? NULL : "colr.enumcs-length", "enumerated colour extent");
        if (valid) {
            check(h.baseline_colr && h.approx_colr, "baseline colour classification");
            check(h.colours == (spaces[i] == 16 || spaces[i] == 18 ? 3u : spaces[i] == 17 ? 1u : 0u),
                  "enumerated channel count");
            check(h.rgb == (spaces[i] == 16), "RGB classification");
        }
    }
    for (approx = 0; approx <= 5; approx++) {
        hv_header h = {0};
        ColrHeader c = {0};
        h.jpx = 1; h.colr = 1; c.meth = 3; c.approx = approx;
        rule(hv_rule_colr(&h, &c, rest, 0), approx >= 1 && approx <= 4 ? NULL : "colr.approx", "JPX approximation limits");
    }
}

/* Tables A.28-A.30: the permitted step-list lengths for 0..32
 * decomposition levels. These are syntax checks, separate from the
 * effective-level relationship still listed in the standards review. */
static void quantization_boundaries(void) {
    SizFixed f = {0};
    Component component = {0};
    hv_siz siz = {0};
    hv_component state = {0};
    hv_segments segments;
    Qcd q = {0};
    uint8_t body[196] = {0};
    unsigned style, n, level;
    f.xsiz = f.ysiz = f.xtsiz = f.ytsiz = 4; f.csiz = 1;
    component.xrsiz = component.yrsiz = 1;
    siz.fixed = &f; siz.components = &component; siz.ncomponents = 1;
    for (style = 0; style < 4; style++) {
        unsigned char valid[195] = {0};
        if (style == 1) valid[2] = 1;
        else if (style < 3)
            for (level = 0; level <= 32; level++) valid[(1 + 3 * level) * (style == 2 ? 2 : 1)] = 1;
        for (n = 1; n <= 194; n++) {
            const char *want = style == 3 ? "qcd.style" : valid[n] ? NULL : "qcd.length";
            hv_segments_init(&segments, &siz, &state, 0);
            q.sqcd = 0xE0 | style; q.spqcd.nCount = (int)n;
            rule(hv_segments_main(&segments, HV_QCD, NULL, &q, NULL, 0), want, "QCD step-list extent");
            memset(&state, 0, sizeof state);
            hv_segments_init(&segments, &siz, &state, 0);
            body[1] = (uint8_t)q.sqcd;
            want = style == 3 ? "qcc.style" : valid[n] ? NULL : "qcc.length";
            rule(hv_segments_main(&segments, HV_QCC, NULL, NULL, body, n + 2), want, "QCC step-list extent");
        }
    }
    q.sqcd = 0; q.spqcd.nCount = 97;
    for (n = 0; n < 97; n++) {
        hv_segments_init(&segments, &siz, &state, 0);
        q.spqcd.arr[n] = 1;
        rule(hv_segments_main(&segments, HV_QCD, NULL, &q, NULL, 0), "qcd.reserved-bits", "reserved bit in each step");
        q.spqcd.arr[n] = 0;
    }
}

static void be32(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static const char *icc(const uint8_t *data, size_t size) {
    hv_header h = {0};
    ColrHeader c = {0};
    h.jp2 = 1; h.parent = HV_BOX_JP2H; h.colr = 1; c.meth = 2;
    return hv_rule_colr(&h, &c, data, size);
}
/* Restricted ICC header/tag-directory checks. Tag payload semantics are
 * outside this rule; test extent containment independently of them. */
static void icc_boundaries(void) {
    static const char *tags[] = {"desc", "kTRC", "wtpt", "cprt"};
    uint8_t data[184] = {0};
    unsigned i;
    be32(data, sizeof data);
    memcpy(data + 12, "scnrGRAYXYZ ", 12);
    memcpy(data + 36, "acsp", 4);
    be32(data + 128, 4);
    for (i = 0; i < 4; i++) {
        memcpy(data + 132 + 12 * i, tags[i], 4);
        be32(data + 136 + 12 * i, 180);
        be32(data + 140 + 12 * i, 4);
    }
    rule(icc(data, sizeof data), NULL, "ICC directory exact extent");
    for (i = 0; i < 4; i++) {
        uint8_t *offset = data + 136 + 12 * i, *size = offset + 4;
        be32(offset, 184); be32(size, 0);
        rule(icc(data, sizeof data), NULL, "ICC empty tag at end");
        be32(size, 1);
        rule(icc(data, sizeof data), "colr.icc-tags", "ICC tag past end");
        be32(offset, 185); be32(size, 0);
        rule(icc(data, sizeof data), "colr.icc-tags", "ICC offset past end");
        be32(offset, 180); be32(size, 4);
        memcpy(offset - 4, "xxxx", 4);
        rule(icc(data, sizeof data), "colr.icc-tags", "ICC required tag missing");
        memcpy(offset - 4, tags[i], 4);
    }
    be32(data + 128, 5);
    rule(icc(data, sizeof data), "colr.icc-tags", "ICC directory too large");
    be32(data + 128, 0); be32(data, 132);
    rule(icc(data, 132), "colr.icc-tags", "ICC header minimum still lacks required tags");
    be32(data, 131);
    rule(icc(data, 131), "colr.icc-header", "ICC header one short");
}

static void quant_values(Qcd *q, unsigned style, unsigned coverage) {
    memset(q, 0, sizeof *q);
    q->sqcd = style;
    q->spqcd.nCount = style == 1 ? 2 : (int)((1 + 3 * coverage) * (style == 2 ? 2 : 1));
}

/* A.6.1/A.6.5: component overrides use component 0. Other components
 * exercise the unchanged aggregate, and one-component cases remove the
 * entire aggregate. Run both reversible and scalar-expounded coding. */
static void effective_quantization(void) {
    static const struct {
        int levels, coverage, coc, qcc, tile_cod, tile_qcd, tile_coc, tile_qcc;
        unsigned components;
        int valid;
    } cases[] = {
        {1,0,-1,-1,-1,-1,-1,-1,2,0},
        {1,1,-1,-1,-1,-1,-1,-1,2,1},
        {1,2,-1,-1,-1,-1,-1,-1,2,1},
        {1,1, 2,-1,-1,-1,-1,-1,2,0},
        {1,1,-1, 0,-1,-1,-1,-1,2,0},
        {1,1, 2, 2,-1,-1,-1,-1,2,1},
        {1,0, 2,-1, 0,-1,-1,-1,2,1},
        {1,1, 2, 0,-1, 2,-1,-1,2,1},
        {1,1,-1,-1, 2,-1,-1,-1,2,0},
        {1,1,-1,-1,-1, 0,-1,-1,2,0},
        {1,1,-1,-1, 2, 2,-1,-1,2,1},
        {1,1,-1,-1,-1,-1, 2,-1,2,0},
        {1,1,-1,-1,-1,-1, 2, 2,2,1},
        {1,1,-1,-1,-1,-1,-1, 0,2,0},
        {1,1,-1,-1, 2, 2, 1, 1,2,1},
        {1,1,-1,-1, 2, 2, 3, 2,2,0},
        {1,1,-1,-1, 2, 2, 0, 0,2,1},
        {1,0,-1,-1, 2, 0, 0, 0,1,1},
        {1,0,-1,-1,-1,-1, 0, 0,1,1},
        {1,0,-1,-1,-1,-1, 0, 0,2,0},
        {0,0,-1,-1,-1,-1,32,32,2,1},
    };
    unsigned style;
    size_t i;
    for (style = 0; style <= 2; style++) for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        SizFixed f = {0};
        Component components[2] = {{0}};
        hv_siz siz = {0};
        hv_component states[2] = {{0}};
        hv_segments segments;
        hv_tile_count tile = {0};
        Cod cod = {0};
        Qcd q;
        uint8_t body[196] = {0};
        int stage;
        char what[96];
        f.xsiz = 16; f.ysiz = f.xtsiz = f.ytsiz = 8; f.csiz = cases[i].components;
        components[0].xrsiz = components[0].yrsiz = 1; components[1] = components[0];
        siz.fixed = &f; siz.components = components; siz.ncomponents = cases[i].components;
        hv_segments_init(&segments, &siz, states, 0);
        cod.spcod.transform = style == 0; cod.spcod.levels = (unsigned)cases[i].levels;
        cod.spcod.cbWidthExp = cod.spcod.cbHeightExp = 2; cod.sgcod.layers = 1;
        rule(hv_segments_main(&segments, HV_COD, &cod, NULL, NULL, 0), NULL, "main COD");
        quant_values(&q, style, (unsigned)cases[i].coverage);
        rule(hv_segments_main(&segments, HV_QCD, NULL, &q, NULL, 0), NULL, "main QCD");
        for (stage = 0; stage < 2; stage++) {
            int coc = stage ? cases[i].tile_coc : cases[i].coc;
            int qcc = stage ? cases[i].tile_qcc : cases[i].qcc;
            if (stage) {
                rule(hv_segments_tile_part(&segments, &tile, 0, 0, 100), NULL, "begin tile before overrides");
                if (cases[i].tile_cod >= 0) {
                    cod.spcod.levels = (unsigned)cases[i].tile_cod;
                    rule(hv_segments_tile(&segments, HV_COD, &cod, NULL, NULL, 0), NULL, "tile COD");
                }
                if (cases[i].tile_qcd >= 0) {
                    quant_values(&q, style, (unsigned)cases[i].tile_qcd);
                    rule(hv_segments_tile(&segments, HV_QCD, NULL, &q, NULL, 0), NULL, "tile QCD");
                }
            }
            if (coc >= 0) {
                const uint8_t coc_body[] = {0, 0, (uint8_t)coc, 2, 2, 0, (uint8_t)(style == 0)};
                rule(stage ? hv_segments_tile(&segments, HV_COC, NULL, NULL, coc_body, sizeof coc_body)
                           : hv_segments_main(&segments, HV_COC, NULL, NULL, coc_body, sizeof coc_body), NULL, "COC override");
            }
            if (qcc >= 0) {
                quant_values(&q, style, (unsigned)qcc);
                body[1] = (uint8_t)style;
                memcpy(body + 2, q.spqcd.arr, (size_t)q.spqcd.nCount);
                rule(stage ? hv_segments_tile(&segments, HV_QCC, NULL, NULL, body, (size_t)q.spqcd.nCount + 2)
                           : hv_segments_main(&segments, HV_QCC, NULL, NULL, body, (size_t)q.spqcd.nCount + 2), NULL, "QCC override");
            }
        }
        snprintf(what, sizeof what, "effective quantization case %zu, style %u", i, style);
        rule(hv_segments_data(&segments, body, 0, 0, 0, 0),
             style == 1 || cases[i].valid ? NULL : "codestream.quantization-coverage", what);
        if (style == 1 || cases[i].valid) {
            hv_tile_count next = {0};
            int levels = cases[i].coc >= 0 ? cases[i].coc : cases[i].levels;
            int coverage = cases[i].qcc >= 0 ? cases[i].qcc : cases[i].coverage;
            int shortfall = levels > coverage ||
                (cases[i].components > 1 && cases[i].levels > cases[i].coverage);
            rule(hv_segments_tile_part(&segments, &next, 1, 0, 100), NULL, "begin next tile");
            rule(hv_segments_data(&segments, body, 0, 0, 0, 0),
                 style != 1 && shortfall ? "codestream.quantization-coverage" : NULL,
                 "previous tile overrides do not leak into the next tile");
        }
    }
}

static void profile1_overrides(void) {
    unsigned override, main_levels, tile_levels;
    for (override = 0; override < 3; override++)
        for (main_levels = 0; main_levels <= 1; main_levels++)
            for (tile_levels = 0; tile_levels <= 1; tile_levels++) {
                SizFixed f = {0};
                Component component = {0};
                hv_siz siz = {0};
                hv_component state = {0};
                hv_segments s;
                hv_tile_count tile = {0};
                Cod cod = {0};
                Qcd q;
                const uint8_t coc[] = {0, 0, (uint8_t)tile_levels, 2, 2, 0, 1};
                unsigned effective = override ? tile_levels : main_levels;
                f.xsiz = f.ysiz = f.xtsiz = f.ytsiz = 256; f.csiz = 1; f.rsiz = 2;
                component.xrsiz = component.yrsiz = 1;
                siz.fixed = &f; siz.components = &component; siz.ncomponents = 1;
                hv_segments_init(&s, &siz, &state, 0);
                cod.sgcod.layers = 1; cod.spcod.transform = 1; cod.spcod.levels = main_levels;
                cod.spcod.cbWidthExp = cod.spcod.cbHeightExp = 2;
                quant_values(&q, 0, 1);
                rule(hv_segments_main(&s, HV_COD, &cod, NULL, NULL, 0), NULL, "Profile 1 main COD");
                rule(hv_segments_main(&s, HV_QCD, NULL, &q, NULL, 0), NULL, "Profile 1 QCD");
                rule(hv_segments_tile_part(&s, &tile, 0, 0, 100), NULL, "Profile 1 allows later overrides");
                if (override == 1) {
                    cod.spcod.levels = tile_levels;
                    rule(hv_segments_tile(&s, HV_COD, &cod, NULL, NULL, 0), NULL, "Profile 1 tile COD");
                } else if (override == 2)
                    rule(hv_segments_tile(&s, HV_COC, NULL, NULL, coc, sizeof coc), NULL, "Profile 1 tile COC");
                rule(hv_segments_data(&s, coc, 0, 0, 0, 0), effective ? NULL : "codestream.profile-1",
                     "Profile 1 uses effective LL dimensions");
            }
}

static void channel_contexts(void) {
    static const uint32_t parents[] = {HV_BOX_JP2H, HV_BOX_JPCH, HV_BOX_JPLH};
    CdefEntry entries[HV_CDEF_PAIRWISE + 1];
    uint64_t scratch[HV_CDEF_PAIRWISE + 1];
    size_t count, i, parent;
    int jp2, kind;
    for (count = 2; count <= HV_CDEF_PAIRWISE + 1; count += HV_CDEF_PAIRWISE - 1)
        for (parent = 0; parent < sizeof parents / sizeof *parents; parent++)
            for (jp2 = 0; jp2 < 2; jp2++) for (kind = 0; kind < 2; kind++) {
                hv_header h;
                hv_header_init(&h, parents[parent], 1);
                /* The JP2 compatibility flag belongs only to jp2h. */
                h.jp2 = jp2 && parent == 0; h.cdef = 1;
                for (i = 0; i < count; i++) {
                    entries[i].cn = i; entries[i].typ = 65535; entries[i].asoc = 65535;
                }
                entries[0].typ = kind ? 1 : 0; entries[0].asoc = 1;
                entries[1].typ = kind ? 2 : 0; entries[1].asoc = 1;
                rule(hv_rule_cdef(&h, entries, count, NULL, scratch),
                     h.jp2 ? (kind ? "cdef.opacity" : "cdef.pairs") : NULL, "CDEF JP2/JPX context");
                check(h.cdef_cn == count - 1 && h.cdef_count == count, "JPX retains channel metadata");
            }
}

/* B.11 and Table A.5: the grid is bounded, independently of which
 * tile-parts a later traversal encounters. */
static void tile_grid_limits(void) {
    SizFixed f = {0};
    Component component = {0};
    hv_siz siz = {0};
    int axis;
    f.csiz = 1;
    component.xrsiz = component.yrsiz = 1;
    siz.fixed = &f; siz.components = &component; siz.ncomponents = 1;
    for (axis = 0; axis < 2; axis++) {
        f.xtsiz = f.ytsiz = 1;
        f.xsiz = axis ? 1 : 65535; f.ysiz = axis ? 65535 : 1;
        rule(hv_rule_siz(&siz, NULL, 0), NULL, "65,535 tiles fit Isot");
        f.xsiz = axis ? 1 : 65536; f.ysiz = axis ? 65536 : 1;
        rule(hv_rule_siz(&siz, NULL, 0), "siz.tile-count", "65,536 tiles do not fit Isot");
    }
    f.xsiz = 255; f.ysiz = 257;
    rule(hv_rule_siz(&siz, NULL, 0), NULL, "65,535 tiles across both axes");
    f.xsiz = f.ysiz = 256;
    rule(hv_rule_siz(&siz, NULL, 0), "siz.tile-count", "65,536 tiles across both axes");
    f.xsiz = f.ysiz = UINT32_MAX;
    rule(hv_rule_siz(&siz, NULL, 0), "siz.tile-count", "largest grid product");
    f.xtsiz = f.ytsiz = UINT32_MAX;
    rule(hv_rule_siz(&siz, NULL, 0), NULL, "largest coordinates in one tile");
}

/* Bound fixed container-information fields before reading their counts. */
static void container_information_lengths(void) {
    for (int kind = 0; kind < 2; kind++) {
        size_t required = kind ? 20 : 8;
        uint8_t fields[21] = {0};
        fields[3] = 1;                 /* M or Ncs. */
        if (kind) fields[11] = 1;       /* One compositing layer, no threads. */
        for (size_t length = required - 1; length <= required + 1; length++) {
            hv_out out;
            hv_box box = {0};
            size_t start, at = 0;
            int ipr = 0;
            hv_out_init(&out);
            int status = hv_begin_box(&out, kind ? HV_BOX_JCLX : HV_BOX_J2CX, 0, &start);
            if (status == 0) status = hv_write_box_header(&out, kind ? HV_BOX_JLXI : HV_BOX_J2CI, length);
            if (status == 0) status = hv_write_bytes(&out, fields, length);
            if (status == 0) status = hv_write_box_header(&out, kind ? HV_BOX_JPLH : HV_BOX_JP2C, 0);
            if (status == 0) status = hv_end_box(&out, start);
            check(status == 0, "container information fixture");
            if (status == 0) {
                box.type = kind ? HV_BOX_JCLX : HV_BOX_J2CX;
                box.payload = 8; box.end = out.size;
                const char *error = hv_rule_box_placed(out.data, &box, 0, &ipr, &at);
                rule(error, length == required ? NULL : kind ? "jlxi.length" : "j2ci.length",
                     "container information length");
                if (error) check(at == 8, "container information error offset");
            }
            hv_out_free(&out);
        }
    }
}

int main(void) {
    container_information_lengths();
    siz_boundaries();
    rsiz_boundaries();
    tile_grid_limits();
    plt_boundaries();
    fragment_boundaries();
    url_boundaries();
    palette_boundaries();
    colour_boundaries();
    quantization_boundaries();
    icc_boundaries();
    effective_quantization();
    profile1_overrides();
    channel_contexts();
    if (!failures) puts("all rule boundaries passed");
    return failures != 0;
}
