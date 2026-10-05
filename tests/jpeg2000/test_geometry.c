/* test_geometry: hv_geometry against T.800 computed here a second way.
 *
 * For random SIZ and COD segments, most of them small, some on a grid near
 * 2^32, every value hv_geometry_init lays out and every packet of
 * hv_geometry_packets is compared with a reference that works from the
 * standard's definitions:
 *   - the tile (B-7 to B-10), tile-component (B-12), resolution (B-14) and
 *     band (B-15) extents, from the equations;
 *   - the precinct grid of each resolution, and the code-block grid of
 *     each band, by visiting every sample of the resolution or band and
 *     taking the cell that holds it (B.6, B.7), code-blocks clipped by the
 *     precincts (B-17);
 *   - the code-blocks of each precinct in each band (hv_precinct_blocks),
 *     by the same visit;
 *   - the code-block numbering: per band in raster order, bands in
 *     (component, resolution, band) order (hv_geometry.h);
 *   - the packet order (B.12.1), with the standard's loops: for RPCL, PCRL
 *     and CPRL over every point (x, y) of the tile on the reference grid,
 *     with the conditions of B.12.1.3 to B.12.1.5 that pick the point at
 *     which each precinct's packets come.
 * Neither side shares code with the other.
 *
 * Also what the random cases do not reach (check_edges): the checks of
 * hv_geometry_init and their messages, a tile index outside the grid,
 * the packet limit at its boundary, counts that saturate at 2^64 - 1, and
 * running out of memory at each allocation. hv_geometry.c is included,
 * with calloc and malloc that fail on demand.
 *
 *   test_geometry */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The allocation hv_geometry.c makes when fail_at counts down to 0 fails;
 * -1: none does. A request for 0 bytes fails too, as C allows (a tile
 * whose sub-sampled components hold no sample has no packets). */
static int fail_at = -1;

static void *test_calloc(size_t n, size_t size) {
    return (fail_at >= 0 && fail_at-- == 0) || n * size == 0 ? NULL : calloc(n, size);
}

static void *test_malloc(size_t size) {
    return (fail_at >= 0 && fail_at-- == 0) || size == 0 ? NULL : malloc(size);
}

#define calloc test_calloc
#define malloc test_malloc
#include "jpeg2000/hv_geometry.c"
#undef calloc
#undef malloc

static int failures;
static unsigned long cases_run, packets_compared;

static void fail(unsigned long seed, const char *what, long long a, long long b) {
    if (failures < 20)
        printf("FAIL case %lu: %s: %lld, expected %lld\n", seed, what, a, b);
    failures++;
}

/* Unsigned arithmetic that wraps on purpose, which Clang's
 * -fsanitize=integer would report. */
#if defined(__clang__)
#define WRAPS __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base")))
#else
#define WRAPS
#endif

/* xorshift64*: the cases are the same on every run. */
static uint64_t state = 0x9E3779B97F4A7C15u;
WRAPS static uint64_t next(void) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717u;
}
static int64_t pick(int64_t lo, int64_t hi) {         /* lo..hi */
    return lo + (int64_t)(next() % (uint64_t)(hi - lo + 1));
}

/* Floor and ceiling of a / b for b > 0, a of any sign. */
static int64_t fdiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}
static int64_t cdiv(int64_t a, int64_t b) { return -fdiv(-a, b); }

/* ------------------------------------------------------------------------
 * The reference
 * ------------------------------------------------------------------------ */

#define MAXC 3
#define MAXR 7              /* levels 0 to 6 */

typedef struct {
    int64_t x0, y0, x1, y1;         /* band extent (B-15) */
    int xcb, ycb;                   /* clipped code-block exponents (B-17) */
    int64_t gx0, gy0, nx, ny;       /* code-block cells holding samples */
} ref_band;

typedef struct {
    int64_t x0, y0, x1, y1;         /* resolution extent (B-14) */
    int ppx, ppy;
    int64_t px0, py0, pw, ph;       /* precinct cells holding samples */
    int nbands;
    ref_band band[3];
} ref_res;

typedef struct {
    int64_t tx0, ty0, tx1, ty1;
    int64_t tcx0[MAXC], tcy0[MAXC], tcx1[MAXC], tcy1[MAXC];
    ref_res res[MAXC][MAXR];
} ref_tile;

/* The cells of size 2^e that hold the samples [a0, a1): the first and
 * how many, by visiting every sample. */
static void cells(int64_t a0, int64_t a1, int e, int64_t *first, int64_t *n) {
    int64_t a, lo = 0, hi = -1;
    *first = 0;
    *n = 0;
    for (a = a0; a < a1; a++) {
        int64_t cell = fdiv(a, (int64_t)1 << e);
        if (a == a0)
            lo = cell;
        hi = cell;
    }
    if (a1 > a0) {
        *first = lo;
        *n = hi - lo + 1;
    }
}

static void reference(const SizFixed *s, const Component *comp, const Cod *cod, uint32_t tile,
                      ref_tile *t) {
    int64_t ntx = cdiv((int64_t)s->xsiz - (int64_t)s->xtosiz, (int64_t)s->xtsiz);
    int64_t p = (int64_t)tile % ntx, q = (int64_t)tile / ntx;
    int NL = (int)cod->spcod.levels, c, r, b;

    /* B-7 to B-10 */
    t->tx0 = (int64_t)s->xtosiz + p * (int64_t)s->xtsiz;
    if (t->tx0 < (int64_t)s->xosiz)
        t->tx0 = (int64_t)s->xosiz;
    t->ty0 = (int64_t)s->ytosiz + q * (int64_t)s->ytsiz;
    if (t->ty0 < (int64_t)s->yosiz)
        t->ty0 = (int64_t)s->yosiz;
    t->tx1 = (int64_t)s->xtosiz + (p + 1) * (int64_t)s->xtsiz;
    if (t->tx1 > (int64_t)s->xsiz)
        t->tx1 = (int64_t)s->xsiz;
    t->ty1 = (int64_t)s->ytosiz + (q + 1) * (int64_t)s->ytsiz;
    if (t->ty1 > (int64_t)s->ysiz)
        t->ty1 = (int64_t)s->ysiz;

    for (c = 0; c < (int)s->csiz; c++) {
        int64_t xr = (int64_t)comp[c].xrsiz, yr = (int64_t)comp[c].yrsiz;
        /* B-12 */
        t->tcx0[c] = cdiv(t->tx0, xr);
        t->tcy0[c] = cdiv(t->ty0, yr);
        t->tcx1[c] = cdiv(t->tx1, xr);
        t->tcy1[c] = cdiv(t->ty1, yr);
        for (r = 0; r <= NL; r++) {
            ref_res *res = &t->res[c][r];
            int64_t d = (int64_t)1 << (NL - r);
            int xcb = (int)cod->spcod.cbWidthExp + 2, ycb = (int)cod->spcod.cbHeightExp + 2;
            /* B-14 */
            res->x0 = cdiv(t->tcx0[c], d);
            res->y0 = cdiv(t->tcy0[c], d);
            res->x1 = cdiv(t->tcx1[c], d);
            res->y1 = cdiv(t->tcy1[c], d);
            /* Table A.21: 15 without custom precincts. */
            res->ppx = cod->scod.customPrecincts ? (int)cod->spcod.precincts.arr[r].ppx : 15;
            res->ppy = cod->scod.customPrecincts ? (int)cod->spcod.precincts.arr[r].ppy : 15;
            cells(res->x0, res->x1, res->ppx, &res->px0, &res->pw);
            cells(res->y0, res->y1, res->ppy, &res->py0, &res->ph);
            if (res->pw == 0 || res->ph == 0)
                res->pw = res->ph = 0;
            /* B-17: in a band, the precinct is 2^(PP - 1) above r = 0. */
            if (xcb > res->ppx - (r > 0))
                xcb = res->ppx - (r > 0);
            if (ycb > res->ppy - (r > 0))
                ycb = res->ppy - (r > 0);
            res->nbands = r == 0 ? 1 : 3;
            for (b = 0; b < res->nbands; b++) {
                ref_band *band = &res->band[b];
                /* B-15: nb, and (xob, yob): LL (0, 0) at r = 0, above it
                 * HL (1, 0), LH (0, 1) and HH (1, 1). */
                int nb = r == 0 ? NL : NL - r + 1;
                int64_t xob = r > 0 && b != 1, yob = r > 0 && b != 0;
                int64_t half = nb > 0 ? (int64_t)1 << (nb - 1) : 0;
                int64_t full = (int64_t)1 << nb;
                band->x0 = cdiv(t->tcx0[c] - half * xob, full);
                band->y0 = cdiv(t->tcy0[c] - half * yob, full);
                band->x1 = cdiv(t->tcx1[c] - half * xob, full);
                band->y1 = cdiv(t->tcy1[c] - half * yob, full);
                band->xcb = xcb;
                band->ycb = ycb;
                cells(band->x0, band->x1, xcb, &band->gx0, &band->nx);
                cells(band->y0, band->y1, ycb, &band->gy0, &band->ny);
                if (band->nx == 0 || band->ny == 0)
                    band->nx = band->ny = 0;
            }
        }
    }
}

/* The packet order of B.12.1, as (resolution index, precinct, layer). */
typedef struct {
    uint32_t resolution, precinct;
    int layer;
} ref_packet;

typedef struct {
    ref_packet *p;
    size_t n, cap;
} ref_packets;

static void emit(ref_packets *out, uint32_t ri, uint32_t k, int layer) {
    if (out->n == out->cap) {
        out->cap = out->cap ? 2 * out->cap : 256;
        out->p = realloc(out->p, out->cap * sizeof *out->p);
        if (out->p == NULL) {
            printf("FAIL out of memory\n");
            exit(1);
        }
    }
    out->p[out->n].resolution = ri;
    out->p[out->n].precinct = k;
    out->p[out->n++].layer = layer;
}

/* The precinct of resolution r of component c whose packets come at the
 * reference grid point (x, y), if any (B.12.1.3): x is divisible by
 * XRsiz 2^(PPx + NL - r), or is the tile's first column when the
 * resolution's first column does not start a precinct; the same for y. */
static int precinct_at(const ref_tile *t, const Component *comp, int NL, int c, int r,
                       int64_t x, int64_t y, uint32_t *k) {
    const ref_res *res = &t->res[c][r];
    int64_t xr = (int64_t)comp[c].xrsiz, yr = (int64_t)comp[c].yrsiz, d = (int64_t)1 << (NL - r);
    int64_t px, py;
    int xok, yok;
    if (res->pw == 0)
        return 0;
    xok = x % (xr * (d << res->ppx)) == 0 ||
          (x == t->tx0 && (res->x0 * d) % (d << res->ppx) != 0);
    yok = y % (yr * (d << res->ppy)) == 0 ||
          (y == t->ty0 && (res->y0 * d) % (d << res->ppy) != 0);
    if (!xok || !yok)
        return 0;
    px = fdiv(cdiv(x, xr * d), (int64_t)1 << res->ppx) - fdiv(res->x0, (int64_t)1 << res->ppx);
    py = fdiv(cdiv(y, yr * d), (int64_t)1 << res->ppy) - fdiv(res->y0, (int64_t)1 << res->ppy);
    *k = (uint32_t)(px + res->pw * py);
    return 1;
}

static void reference_packets(const ref_tile *t, const Component *comp, int ncomps,
                              const Cod *cod, ref_packets *out) {
    int NL = (int)cod->spcod.levels, layers = (int)cod->sgcod.layers;
    int order = (int)cod->sgcod.progression, l, r, c;
    int64_t x, y, k;
    uint32_t pk;
#define RI(c, r) ((uint32_t)((c) * (NL + 1) + (r)))
#define NPREC(c, r) (t->res[c][r].pw * t->res[c][r].ph)
    out->n = 0;
    switch (order) {
    case HV_LRCP:
        for (l = 0; l < layers; l++)
            for (r = 0; r <= NL; r++)
                for (c = 0; c < ncomps; c++)
                    for (k = 0; k < NPREC(c, r); k++)
                        emit(out, RI(c, r), (uint32_t)k, l);
        break;
    case HV_RLCP:
        for (r = 0; r <= NL; r++)
            for (l = 0; l < layers; l++)
                for (c = 0; c < ncomps; c++)
                    for (k = 0; k < NPREC(c, r); k++)
                        emit(out, RI(c, r), (uint32_t)k, l);
        break;
    case HV_RPCL:
        for (r = 0; r <= NL; r++)
            for (y = t->ty0; y < t->ty1; y++)
                for (x = t->tx0; x < t->tx1; x++)
                    for (c = 0; c < ncomps; c++)
                        if (precinct_at(t, comp, NL, c, r, x, y, &pk))
                            for (l = 0; l < layers; l++)
                                emit(out, RI(c, r), pk, l);
        break;
    case HV_PCRL:
        for (y = t->ty0; y < t->ty1; y++)
            for (x = t->tx0; x < t->tx1; x++)
                for (c = 0; c < ncomps; c++)
                    for (r = 0; r <= NL; r++)
                        if (precinct_at(t, comp, NL, c, r, x, y, &pk))
                            for (l = 0; l < layers; l++)
                                emit(out, RI(c, r), pk, l);
        break;
    default:                /* CPRL */
        for (c = 0; c < ncomps; c++)
            for (y = t->ty0; y < t->ty1; y++)
                for (x = t->tx0; x < t->tx1; x++)
                    for (r = 0; r <= NL; r++)
                        if (precinct_at(t, comp, NL, c, r, x, y, &pk))
                            for (l = 0; l < layers; l++)
                                emit(out, RI(c, r), pk, l);
        break;
    }
#undef RI
#undef NPREC
}

/* ------------------------------------------------------------------------
 * The comparison
 * ------------------------------------------------------------------------ */

#define SAME(what, a, b)                                                        \
    do {                                                                        \
        if ((long long)(a) != (long long)(b))                                   \
            fail(n, what, (long long)(a), (long long)(b));                      \
    } while (0)

/* The code-blocks of every precinct of res in band b, by visiting the
 * band's samples, against hv_precinct_blocks. */
static void compare_precinct_blocks(unsigned long n, const hv_resolution *res,
                                    const ref_res *rr, int b) {
    const ref_band *band = &rr->band[b];
    int bx = rr->ppx - (res->r > 0), by = rr->ppy - (res->r > 0);
    int64_t i, x, y;
    for (i = 0; i < rr->pw * rr->ph; i++) {
        int64_t px = rr->px0 + i % rr->pw, py = rr->py0 + i / rr->pw;
        int64_t lox = 0, hix = -1, loy = 0, hiy = -1;
        int any = 0;
        hv_block_rect got = hv_precinct_blocks(res, b, px, py);
        for (y = band->y0; y < band->y1; y++) {
            if (fdiv(y, (int64_t)1 << by) != py)
                continue;
            for (x = band->x0; x < band->x1; x++) {
                int64_t cx, cy;
                if (fdiv(x, (int64_t)1 << bx) != px)
                    continue;
                cx = fdiv(x, (int64_t)1 << band->xcb);
                cy = fdiv(y, (int64_t)1 << band->ycb);
                if (!any || cx < lox) lox = cx;
                if (!any || cx > hix) hix = cx;
                if (!any || cy < loy) loy = cy;
                if (!any || cy > hiy) hiy = cy;
                any = 1;
            }
        }
        SAME("precinct code-blocks, w", got.w, any ? hix - lox + 1 : 0);
        SAME("precinct code-blocks, h", got.h, any ? hiy - loy + 1 : 0);
        SAME("precinct code-blocks, cx0", got.cx0, any ? lox : 0);
        SAME("precinct code-blocks, cy0", got.cy0, any ? loy : 0);
    }
}

static void compare(unsigned long n, const SizFixed *s, const Component *comp, const Cod *cod,
                    uint32_t tile) {
    static ref_tile t;
    static ref_packets want;
    hv_siz siz = {s, comp, (size_t)s->csiz};
    hv_geometry_limits limits = {1u << 20, 1u << 24, NULL};
    hv_geometry g;
    hv_packet *packets = NULL;
    size_t count = 0, i;
    uint64_t precincts = 0, blocks = 0;
    char error[256];
    int c, r, b;

    memset(&g, 0x5A, sizeof g);             /* hv_geometry_init sets every field */
    if (hv_geometry_init(&g, &siz, cod, tile, &limits, error, sizeof error) != 0) {
        printf("FAIL case %lu: hv_geometry_init: %s\n", n, error);
        failures++;
        hv_geometry_free(&g);
        return;
    }
    reference(s, comp, cod, tile, &t);
    SAME("tx0", g.tx0, t.tx0);
    SAME("ty0", g.ty0, t.ty0);
    SAME("tx1", g.tx1, t.tx1);
    SAME("ty1", g.ty1, t.ty1);
    SAME("components", g.ncomps, s->csiz);
    SAME("levels", g.levels, cod->spcod.levels);
    SAME("layers", g.layers, cod->sgcod.layers);
    SAME("progression", g.progression, cod->sgcod.progression);
    for (c = 0; c < (int)s->csiz; c++)
        for (r = 0; r <= (int)cod->spcod.levels; r++) {
            const hv_resolution *res = hv_geometry_res(&g, c, r);
            const ref_res *rr = &t.res[c][r];
            int d = (int)cod->spcod.levels - r;
            SAME("res c", res->c, c);
            SAME("res r", res->r, r);
            SAME("res x0", res->x0, rr->x0);
            SAME("res y0", res->y0, rr->y0);
            SAME("res x1", res->x1, rr->x1);
            SAME("res y1", res->y1, rr->y1);
            SAME("res ppx", res->ppx, rr->ppx);
            SAME("res ppy", res->ppy, rr->ppy);
            SAME("res bppx", res->bppx, rr->ppx - (r > 0));
            SAME("res bppy", res->bppy, rr->ppy - (r > 0));
            SAME("res xstep", res->xstep, (int64_t)comp[c].xrsiz << d);
            SAME("res ystep", res->ystep, (int64_t)comp[c].yrsiz << d);
            SAME("res pw", res->pw, rr->pw);
            SAME("res ph", res->ph, rr->ph);
            if (rr->pw > 0) {
                SAME("res px0", res->px0, rr->px0);
                SAME("res py0", res->py0, rr->py0);
            }
            SAME("res first precinct", res->first_precinct, precincts);
            precincts += (uint64_t)(rr->pw * rr->ph);
            SAME("res bands", res->nbands, rr->nbands);
            for (b = 0; b < rr->nbands; b++) {
                const hv_band *band = &res->band[b];
                const ref_band *rb = &rr->band[b];
                SAME("band x0", band->x0, rb->x0);
                SAME("band y0", band->y0, rb->y0);
                SAME("band x1", band->x1, rb->x1);
                SAME("band y1", band->y1, rb->y1);
                SAME("band xcb", band->xcb, rb->xcb);
                SAME("band ycb", band->ycb, rb->ycb);
                SAME("band nx", band->nx, rb->nx);
                SAME("band ny", band->ny, rb->ny);
                if (rb->nx > 0) {
                    SAME("band gx0", band->gx0, rb->gx0);
                    SAME("band gy0", band->gy0, rb->gy0);
                    SAME("band number of a code-block",
                         hv_block_number(band, rb->gx0 + rb->nx - 1, rb->gy0),
                         blocks + (uint64_t)(rb->nx - 1));
                }
                SAME("band first block", band->first_block, blocks);
                blocks += (uint64_t)(rb->nx * rb->ny);
                compare_precinct_blocks(n, res, rr, b);
            }
        }
    SAME("precincts", g.nprecincts, precincts);
    SAME("code-blocks", g.nblocks, blocks);

    reference_packets(&t, comp, (int)s->csiz, cod, &want);
    if (hv_geometry_packets(&g, &packets, &count, error, sizeof error) != 0) {
        printf("FAIL case %lu: hv_geometry_packets: %s\n", n, error);
        failures++;
    } else {
        SAME("packets", count, want.n);
        for (i = 0; i < count && i < want.n; i++)
            if (packets[i].resolution != want.p[i].resolution ||
                packets[i].precinct != want.p[i].precinct ||
                packets[i].layer != want.p[i].layer) {
                char what[96];
                snprintf(what, sizeof what, "packet %zu (resolution, precinct, layer %d)", i,
                         want.p[i].layer);
                fail(n, what, (long long)packets[i].resolution * 1000000 + packets[i].precinct,
                     (long long)want.p[i].resolution * 1000000 + want.p[i].precinct);
                break;
            }
        packets_compared += count;
    }
    free(packets);
    hv_geometry_free(&g);
    cases_run++;
}

/* ------------------------------------------------------------------------
 * The cases
 * ------------------------------------------------------------------------ */

/* A random valid SIZ, COD and tile index. `far`: the image near the end of
 * the 32-bit grid, its tiles small. The tile itself stays small, as the
 * reference visits each of its points. */
static void random_case(unsigned long n, int far) {
    SizFixed s;
    Component comp[MAXC];
    Cod cod;
    int64_t ntx, nty, xo, yo;
    uint32_t tile;
    int c, r;

    memset(&s, 0, sizeof s);
    memset(comp, 0, sizeof comp);
    memset(&cod, 0, sizeof cod);
    s.csiz = (asn1SccUint)pick(1, MAXC);
    for (c = 0; c < (int)s.csiz; c++) {
        comp[c].depthMinus1 = 7;
        comp[c].xrsiz = (asn1SccUint)(pick(0, 2) ? 1 : pick(2, 5));
        comp[c].yrsiz = (asn1SccUint)(pick(0, 2) ? 1 : pick(2, 5));
    }
    if (far) {
        s.xsiz = (asn1SccUint)(4294967295u - (uint64_t)pick(0, 3));
        s.ysiz = (asn1SccUint)(4294967295u - (uint64_t)pick(0, 3));
        s.xosiz = s.xsiz - (asn1SccUint)pick(1, 200);
        s.yosiz = s.ysiz - (asn1SccUint)pick(1, 200);
    } else {
        s.xosiz = (asn1SccUint)(pick(0, 1) ? 0 : pick(0, 40));
        s.yosiz = (asn1SccUint)(pick(0, 1) ? 0 : pick(0, 40));
        s.xsiz = s.xosiz + (asn1SccUint)pick(1, 90);
        s.ysiz = s.yosiz + (asn1SccUint)pick(1, 90);
    }
    s.xtosiz = s.xosiz - (asn1SccUint)pick(0, s.xosiz < 30 ? (int64_t)s.xosiz : 30);
    s.ytosiz = s.yosiz - (asn1SccUint)pick(0, s.yosiz < 30 ? (int64_t)s.yosiz : 30);
    /* XTOsiz + XTsiz > XOsiz */
    xo = (int64_t)(s.xosiz - s.xtosiz);
    yo = (int64_t)(s.yosiz - s.ytosiz);
    s.xtsiz = (asn1SccUint)(xo + pick(1, 70));
    s.ytsiz = (asn1SccUint)(yo + pick(1, 70));
    ntx = cdiv((int64_t)s.xsiz - (int64_t)s.xtosiz, (int64_t)s.xtsiz);
    nty = cdiv((int64_t)s.ysiz - (int64_t)s.ytosiz, (int64_t)s.ytsiz);
    tile = (uint32_t)(pick(0, nty - 1) * ntx + pick(0, ntx - 1));

    cod.sgcod.progression = (asn1SccUint)pick(0, 4);
    cod.sgcod.layers = (asn1SccUint)pick(1, 3);
    cod.spcod.levels = (asn1SccUint)pick(0, 6);
    cod.spcod.cbWidthExp = (asn1SccUint)pick(0, 4);
    cod.spcod.cbHeightExp = (asn1SccUint)pick(0, 4);
    cod.spcod.transform = 1;
    if (pick(0, 3)) {
        cod.scod.customPrecincts = TRUE;
        cod.spcod.precincts.nCount = (int)cod.spcod.levels + 1;
        for (r = 0; r <= (int)cod.spcod.levels; r++) {
            cod.spcod.precincts.arr[r].ppx = (asn1SccUint)pick(r > 0, 6);
            cod.spcod.precincts.arr[r].ppy = (asn1SccUint)pick(r > 0, 6);
        }
    }
    compare(n, &s, comp, &cod, tile);
}

/* ------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------ */

static void expect(const char *what, int ok, const char *error) {
    if (!ok) {
        printf("FAIL %s%s%s\n", what, error ? ": " : "", error ? error : "");
        failures++;
    }
}

/* hv_geometry_init with `limits`: 0, or its error in error. g is left
 * freed. */
static int init(const SizFixed *s, const Component *comp, size_t ncomps, const Cod *cod,
                uint32_t tile, const hv_geometry_limits *limits, hv_geometry *g, char *error,
                size_t size) {
    hv_siz siz = {s, comp, ncomps};
    int status;
    memset(g, 0x5A, sizeof *g);
    error[0] = 0;
    status = hv_geometry_init(g, &siz, cod, tile, limits, error, size);
    return status;
}

static void check_edges(unsigned rsiz) {
    static const hv_geometry_limits roomy = {1u << 20, UINT64_MAX, NULL};
    SizFixed s;
    Component comp[3];
    Cod cod;
    hv_geometry g, zero;
    hv_packet *packets;
    size_t count;
    char error[256], want[64];
    int c;

    /* 3 components on 20 x 17, tiles of 8 x 8 from (2, 1): 3 x 2 tiles. */
    memset(&s, 0, sizeof s);
    memset(comp, 0, sizeof comp);
    memset(&cod, 0, sizeof cod);
    memset(&zero, 0, sizeof zero);
    s.rsiz = rsiz;
    s.xsiz = 20;
    s.ysiz = 17;
    s.xosiz = 3;
    s.yosiz = 2;
    s.xtosiz = 2;
    s.ytosiz = 1;
    s.xtsiz = s.ytsiz = 8;
    s.csiz = 3;
    for (c = 0; c < 3; c++) {
        comp[c].depthMinus1 = 7;
        comp[c].xrsiz = comp[c].yrsiz = 1;
    }
    cod.sgcod.layers = 2;
    cod.sgcod.progression = HV_RPCL;
    cod.spcod.levels = 1;

    expect("the last tile", init(&s, comp, 3, &cod, 5, &roomy, &g, error, sizeof error) == 0 &&
           g.tx0 == 18 && g.ty0 == 9 && g.tx1 == 20 && g.ty1 == 17, error);
    hv_geometry_free(&g);
    expect("hv_geometry_free clears", memcmp(&g, &zero, sizeof g) == 0, NULL);
    expect("a tile past the grid", init(&s, comp, 3, &cod, 6, &roomy, &g, error,
                                        sizeof error) != 0 &&
           strcmp(error, "tile 6 outside the tile grid") == 0, error);
    hv_geometry_free(&g);
    expect("a tile far past the grid", init(&s, comp, 3, &cod, 65535, &roomy, &g, error,
                                            sizeof error) != 0 &&
           strcmp(error, "tile 65535 outside the tile grid") == 0, error);
    hv_geometry_free(&g);

    /* The checks, each with its message. */
    expect("two components for Csiz 3", init(&s, comp, 2, &cod, 0, &roomy, &g, error,
                                              sizeof error) != 0 &&
           strcmp(error, "siz.csiz-count") == 0, error);
    hv_geometry_free(&g);
    comp[0].xrsiz = 0;
    expect("XRsiz 0 in component 0", init(&s, comp, 3, &cod, 0, &roomy, &g, error,
                                          sizeof error) != 0 &&
           strcmp(error, "invalid SIZ") == 0, error);
    hv_geometry_free(&g);
    comp[0].xrsiz = 1;
    comp[2].yrsiz = 0;
    expect("YRsiz 0 in component 2", init(&s, comp, 3, &cod, 0, &roomy, &g, error,
                                          sizeof error) != 0 &&
           strcmp(error, "invalid SIZ") == 0, error);
    hv_geometry_free(&g);
    comp[2].yrsiz = 1;
    s.xtosiz = 4;
    expect("XTOsiz past XOsiz", init(&s, comp, 3, &cod, 0, &roomy, &g, error,
                                     sizeof error) != 0 &&
           strcmp(error, "siz.tile-origin") == 0, error);
    hv_geometry_free(&g);
    s.xtosiz = 2;
    cod.spcod.cbWidthExp = cod.spcod.cbHeightExp = 5;
    expect("code-blocks of 2^14 samples", init(&s, comp, 3, &cod, 0, &roomy, &g, error,
                                               sizeof error) != 0 &&
           strcmp(error, "cod.codeblock-area") == 0, error);
    hv_geometry_free(&g);
    cod.spcod.cbWidthExp = cod.spcod.cbHeightExp = 0;
    cod.spcod.transform = 2;
    expect("transform 2", init(&s, comp, 3, &cod, 0, &roomy, &g, error, sizeof error) != 0 &&
           strcmp(error, "invalid COD") == 0, error);
    hv_geometry_free(&g);
    cod.spcod.transform = 0;

    /* The packet limit, at the count and below it: tile 0 is 7 x 7, one
     * precinct per resolution, 2 resolutions, 3 components, 2 layers. */
    {
        hv_geometry_limits exact = {6, 12, NULL}, below = {6, 11, NULL}, bound = {6, 11, "tile data"};
        expect("12 packets within 12", init(&s, comp, 3, &cod, 0, &exact, &g, error,
                                             sizeof error) == 0 && g.nprecincts == 6, error);
        hv_geometry_free(&g);
        expect("12 packets above 11", init(&s, comp, 3, &cod, 0, &below, &g, error,
                                           sizeof error) != 0 &&
               strcmp(error, "packet count exceeds supported limit (11)") == 0, error);
        hv_geometry_free(&g);
        expect("12 packets above the tile data", init(&s, comp, 3, &cod, 0, &bound, &g, error,
                                                      sizeof error) != 0 &&
               strcmp(error, "packet count exceeds tile data") == 0, error);
        hv_geometry_free(&g);
        exact.resolutions = 5;
        expect("6 resolutions above 5", init(&s, comp, 3, &cod, 0, &exact, &g, error,
                                             sizeof error) != 0 &&
               strcmp(error, "component and resolution count exceeds supported limit (5)") ==
               0, error);
        hv_geometry_free(&g);
    }

    /* Out of memory: the resolutions, the packets, and for a position
     * order the precincts sorted. */
    for (c = 0; c < 3; c++) {
        int status;
        fail_at = c;
        status = init(&s, comp, 3, &cod, 0, &roomy, &g, error, sizeof error);
        if (status == 0)
            status = hv_geometry_packets(&g, &packets, &count, error, sizeof error);
        else
            packets = NULL;
        expect(c == 0 ? "no memory for the resolutions" : c == 1 ? "no memory for the packets"
                                                                 : "no memory to sort",
               status != 0 && strcmp(error, "out of memory") == 0 && packets == NULL, error);
        hv_geometry_free(&g);
        fail_at = -1;
    }

    /* The sort of hv_geometry_packets, where its keys tie (which no tile
     * gives: they tell the precincts apart), and where they do not. */
    {
        sort_entry a = {{1, 2, 3, 4}, 5, 6}, b = a;
        expect("equal entries", compare_entries(&a, &b) == 0, NULL);
        b.precinct = 7;
        expect("precinct 6 before 7", compare_entries(&a, &b) == -1 &&
               compare_entries(&b, &a) == 1, NULL);
        b.resolution = 4;
        expect("resolution 4 before 5", compare_entries(&b, &a) == -1 &&
               compare_entries(&a, &b) == 1, NULL);
        b.key[3] = 5;
        expect("the last key", compare_entries(&a, &b) == -1 && compare_entries(&b, &a) == 1,
               NULL);
        b.key[0] = 0;
        expect("the first key", compare_entries(&b, &a) == -1 && compare_entries(&a, &b) == 1,
               NULL);
    }

    /* 2^31 - 1 packets, the most hv_geometry_packets lays out (the
     * allocation made to fail), and 2^31: one component of W x 1 with
     * precincts of one sample. */
    s.xsiz = s.xtsiz = 2147483647u;
    s.ysiz = s.ytsiz = 1;
    s.xosiz = s.yosiz = s.xtosiz = s.ytosiz = 0;
    s.csiz = 1;
    cod.sgcod.layers = 1;
    cod.spcod.levels = 0;
    cod.scod.customPrecincts = TRUE;
    cod.spcod.precincts.nCount = 1;
    cod.spcod.precincts.arr[0].ppx = cod.spcod.precincts.arr[0].ppy = 0;
    expect("2^31 - 1 precincts", init(&s, comp, 1, &cod, 0, &roomy, &g, error,
                                      sizeof error) == 0 && g.nprecincts == 2147483647u, error);
    fail_at = 0;
    expect("2^31 - 1 packets are laid out",
           hv_geometry_packets(&g, &packets, &count, error, sizeof error) != 0 &&
           strcmp(error, "out of memory") == 0, error);
    fail_at = -1;
    hv_geometry_free(&g);
    s.xsiz = s.xtsiz = 2147483648u;
    expect("2^31 precincts", init(&s, comp, 1, &cod, 0, &roomy, &g, error, sizeof error) == 0,
           error);
    expect("2^31 packets are not",
           hv_geometry_packets(&g, &packets, &count, error, sizeof error) != 0 &&
           strcmp(error, "more than 2,147,483,647 packets in the tile") == 0, error);
    hv_geometry_free(&g);

    /* Counts that saturate at 2^64 - 1: 2 components of 2^32 - 1 squared,
     * with precincts and code-blocks of one sample. */
    s.xsiz = s.ysiz = s.xtsiz = s.ytsiz = 4294967295u;
    s.xosiz = s.yosiz = s.xtosiz = s.ytosiz = 0;
    s.csiz = 2;
    cod.sgcod.layers = 1;
    cod.spcod.levels = 0;
    cod.scod.customPrecincts = TRUE;
    cod.spcod.precincts.nCount = 1;
    cod.spcod.precincts.arr[0].ppx = cod.spcod.precincts.arr[0].ppy = 0;
    expect("saturated counts", init(&s, comp, 2, &cod, 0, &roomy, &g, error, sizeof error) == 0 &&
           g.nprecincts == UINT64_MAX && g.nblocks == UINT64_MAX &&
           g.res[1].first_precinct == 18446744065119617025u &&
           g.res[1].band[0].first_block == 18446744065119617025u, error);
    snprintf(want, sizeof want, "more than 2,147,483,647 packets in the tile");
    expect("packets of saturated counts",
           hv_geometry_packets(&g, &packets, &count, error, sizeof error) != 0 &&
           strcmp(error, want) == 0 && packets == NULL && count == 0, error);
    hv_geometry_free(&g);
}

int main(void) {
    unsigned long n;
    /* Declaration policy does not change layout, bounds or failure cleanup. */
    check_edges(0);
    check_edges(3);
    check_edges(0x4000);
    check_edges(0xffff);
    for (n = 0; n < 10000; n++)
        random_case(n, n % 10 == 9);
    printf("%lu cases, %lu packets compared, %d failures\n", cases_run, packets_compared,
           failures);
    return failures != 0;
}
