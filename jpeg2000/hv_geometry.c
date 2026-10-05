/* hv_geometry.c: see hv_geometry.h. Equation numbers are T.800's. */
#include "hv_geometry.h"
#include "hv_grid.h"

#include "hv_error.h"

#include <stdlib.h>
#include <string.h>

/* The most packets hv_geometry_packets lays out (hv_geometry.h). */
#define MAX_PACKETS 2147483647u

/* b > 0. C division truncates, which is the ceiling for a <= 0. */
static int64_t ceildiv(int64_t a, int64_t b) {
    return a > 0 ? (a + b - 1) / b : a / b;
}

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

static uint64_t sat_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}


/* Decoded SIZ/COD field ranges, then cross-field layout requirements.
 * Rsiz declaration checks belong to explicit validation or output policy. */
static int check_input(const hv_siz *hsiz, const Cod *cod, char *error, size_t error_size) {
    const char *rule;
    size_t i;
    int err;
    if (!SizFixed_IsConstraintValid(hsiz->fixed, &err))
        return hv_fail(error, error_size, "invalid SIZ");
    for (i = 0; i < hsiz->ncomponents; i++)
        if (!Component_IsConstraintValid(&hsiz->components[i], &err))
            return hv_fail(error, error_size, "invalid SIZ");
    if (!Cod_IsConstraintValid(cod, &err))
        return hv_fail(error, error_size, "invalid COD");
    if ((rule = hv_rule_siz_geometry(hsiz, &cod->sgcod)) != NULL ||
        (rule = hv_rule_cod(&cod->scod, &cod->spcod, 0)) != NULL)
        return hv_fail(error, error_size, "%s", rule);
    return 0;
}

int hv_geometry_init(hv_geometry *g, const hv_siz *hsiz, const Cod *cod, uint32_t tile,
                     const hv_geometry_limits *limits, char *error, size_t error_size) {
    const SizFixed *siz = hsiz->fixed;
    uint64_t ntx, nty;
    hv_interval x, y;
    int NL = (int)cod->spcod.levels, c, r, b;
    int cbw = (int)cod->spcod.cbWidthExp + 2, cbh = (int)cod->spcod.cbHeightExp + 2;
    char number[32];

    memset(g, 0, sizeof *g);
    /* The components are read up to Csiz: there must be that many. */
    if (hsiz->ncomponents != siz->csiz)
        return hv_fail(error, error_size, "siz.csiz-count");
    if (check_input(hsiz, cod, error, error_size) != 0)
        return -1;
    if ((uint64_t)siz->csiz * (uint64_t)(NL + 1) > limits->resolutions)
        return hv_fail(error, error_size,
                       "component and resolution count exceeds supported limit (%s)",
                       hv_grouped(limits->resolutions, number));
    /* B-5 to B-10: the tile's position in the grid and its extent. */
    ntx = hv_tile_axis_count(siz->xsiz, siz->xtosiz, siz->xtsiz);
    nty = hv_tile_axis_count(siz->ysiz, siz->ytosiz, siz->ytsiz);
    /* ntx and nty are at least 1 (hv_rule_siz); ntx * nty can overflow. */
    if (tile / ntx >= nty)
        return hv_fail(error, error_size, "tile %u outside the tile grid", (unsigned)tile);
    x = hv_tile_axis_extent(siz->xosiz, siz->xsiz, siz->xtosiz, siz->xtsiz, tile % ntx);
    y = hv_tile_axis_extent(siz->yosiz, siz->ysiz, siz->ytosiz, siz->ytsiz, tile / ntx);
    g->tx0 = (int64_t)x.first;
    g->tx1 = (int64_t)x.end;
    g->ty0 = (int64_t)y.first;
    g->ty1 = (int64_t)y.end;
    g->ncomps = (int)siz->csiz;
    g->levels = NL;
    g->layers = (int)cod->sgcod.layers;
    g->progression = (int)cod->sgcod.progression;
    g->res = calloc((size_t)g->ncomps * (NL + 1), sizeof *g->res);
    if (g->res == NULL)
        return hv_fail(error, error_size, "out of memory");

    for (c = 0; c < g->ncomps; c++) {
        int64_t xr = (int64_t)hsiz->components[c].xrsiz;
        int64_t yr = (int64_t)hsiz->components[c].yrsiz;
        /* B-12: the tile-component. */
        int64_t cx0 = ceildiv(g->tx0, xr), cy0 = ceildiv(g->ty0, yr);
        int64_t cx1 = ceildiv(g->tx1, xr), cy1 = ceildiv(g->ty1, yr);
        for (r = 0; r <= NL; r++) {
            hv_resolution *res = &g->res[(size_t)c * (NL + 1) + r];
            int64_t s = (int64_t)1 << (NL - r);
            /* Precincts smaller than the code-blocks clip them (B-17). */
            int xcb, ycb;

            res->c = c;
            res->r = r;
            res->ppx = cod->scod.customPrecincts ? (int)cod->spcod.precincts.arr[r].ppx : 15;
            res->ppy = cod->scod.customPrecincts ? (int)cod->spcod.precincts.arr[r].ppy : 15;
            res->bppx = r ? res->ppx - 1 : res->ppx;
            res->bppy = r ? res->ppy - 1 : res->ppy;
            xcb = cbw < res->bppx ? cbw : res->bppx;
            ycb = cbh < res->bppy ? cbh : res->bppy;
            /* B-14 */
            res->x0 = (int64_t)hv_resolution_coord((uint64_t)cx0, (unsigned)(NL - r));
            res->y0 = (int64_t)hv_resolution_coord((uint64_t)cy0, (unsigned)(NL - r));
            res->x1 = (int64_t)hv_resolution_coord((uint64_t)cx1, (unsigned)(NL - r));
            res->y1 = (int64_t)hv_resolution_coord((uint64_t)cy1, (unsigned)(NL - r));
            res->xstep = (uint64_t)xr * (uint64_t)s;
            res->ystep = (uint64_t)yr * (uint64_t)s;
            res->nbands = r ? 3 : 1;
            for (b = 0; b < res->nbands; b++) {
                hv_band *band = &res->band[b];
                /* Band type: 0 LL, 1 HL, 2 LH, 3 HH; bit 0 x, bit 1 y. */
                int type = r ? b + 1 : 0, xo = type & 1, yo = type >> 1;
                int nb = r ? NL - r + 1 : NL;
                int64_t o = r ? (int64_t)1 << (nb - 1) : 0, step = (int64_t)1 << nb;
                /* B-15 */
                band->x0 = ceildiv(cx0 - o * xo, step);
                band->y0 = ceildiv(cy0 - o * yo, step);
                band->x1 = ceildiv(cx1 - o * xo, step);
                band->y1 = ceildiv(cy1 - o * yo, step);
                band->xcb = xcb;
                band->ycb = ycb;
                band->gx0 = band->x0 >> xcb;
                band->gy0 = band->y0 >> ycb;
                if (band->x1 > band->x0 && band->y1 > band->y0) {
                    band->nx = ceildiv(band->x1, (int64_t)1 << xcb) - band->gx0;
                    band->ny = ceildiv(band->y1, (int64_t)1 << ycb) - band->gy0;
                }
                band->first_block = g->nblocks;
                /* nx, ny < 2^32: the product fits; the sum saturates. */
                g->nblocks = sat_add(g->nblocks, (uint64_t)band->nx * (uint64_t)band->ny);
            }
            /* B-16 */
            if (res->x1 > res->x0 && res->y1 > res->y0) {
                res->px0 = res->x0 >> res->ppx;
                res->py0 = res->y0 >> res->ppy;
                res->pw = (int64_t)hv_precinct_count((uint64_t)res->x0, (uint64_t)res->x1, (uint64_t)1 << res->ppx);
                res->ph = (int64_t)hv_precinct_count((uint64_t)res->y0, (uint64_t)res->y1, (uint64_t)1 << res->ppy);
            }
            res->first_precinct = g->nprecincts;
            g->nprecincts = sat_add(g->nprecincts, (uint64_t)res->pw * (uint64_t)res->ph);
        }
    }
    /* Compared by division, which cannot overflow (nprecincts saturates);
     * layers is at least 1 (Sgcod). */
    if (g->nprecincts > limits->packets / (uint64_t)g->layers) {
        if (limits->packets_bound != NULL)
            return hv_fail(error, error_size, "packet count exceeds %s", limits->packets_bound);
        return hv_fail(error, error_size, "packet count exceeds supported limit (%s)",
                       hv_grouped(limits->packets, number));
    }
    return 0;
}

void hv_geometry_free(hv_geometry *g) {
    free(g->res);
    memset(g, 0, sizeof *g);
}

hv_block_rect hv_precinct_blocks(const hv_resolution *res, int b, int64_t px, int64_t py) {
    const hv_band *band = &res->band[b];
    hv_block_rect rect = {0, 0, 0, 0};
    /* The precinct's region in band coordinates. */
    int64_t qx0 = max64(band->x0, px << res->bppx);
    int64_t qy0 = max64(band->y0, py << res->bppy);
    int64_t qx1 = min64(band->x1, (px + 1) << res->bppx);
    int64_t qy1 = min64(band->y1, (py + 1) << res->bppy);
    if (qx1 > qx0 && qy1 > qy0) {
        rect.cx0 = qx0 >> band->xcb;
        rect.cy0 = qy0 >> band->ycb;
        rect.w = ceildiv(qx1, (int64_t)1 << band->xcb) - rect.cx0;
        rect.h = ceildiv(qy1, (int64_t)1 << band->ycb) - rect.cy0;
    }
    return rect;
}

/* ------------------------------------------------------------------------
 * Progression order (B.12)
 * ------------------------------------------------------------------------ */

typedef struct {
    uint64_t key[4];
    uint32_t resolution, precinct;
} sort_entry;

static int compare_entries(const void *a, const void *b) {
    const sort_entry *p = a, *q = b;
    int i;
    for (i = 0; i < 4; i++)
        if (p->key[i] != q->key[i])
            return p->key[i] < q->key[i] ? -1 : 1;
    if (p->resolution != q->resolution)
        return p->resolution < q->resolution ? -1 : 1;
    return p->precinct < q->precinct ? -1 : p->precinct > q->precinct;
}

int hv_geometry_packets(const hv_geometry *g, hv_packet **packets, size_t *count,
                        char *error, size_t error_size) {
    /* Within limits->packets (hv_geometry_init): the product fits. */
    uint64_t total = g->nprecincts * (uint64_t)g->layers;
    size_t nres = (size_t)g->ncomps * (g->levels + 1), k = 0, i, n;
    hv_packet *out;
    int l, r, c;

    *packets = NULL;
    *count = 0;
    if (total > MAX_PACKETS)
        return hv_fail(error, error_size, "more than 2,147,483,647 packets in the tile");
    if ((out = malloc((total ? total : 1) * sizeof *out)) == NULL)
        return hv_fail(error, error_size, "out of memory");

    if (g->progression == HV_LRCP || g->progression == HV_RLCP) {
        /* Every component has levels + 1 resolutions (no COC). */
        int lrcp = g->progression == HV_LRCP;
        int nouter = lrcp ? g->layers : g->levels + 1;
        int ninner = lrcp ? g->levels + 1 : g->layers;
        int outer, inner;
        for (outer = 0; outer < nouter; outer++)
            for (inner = 0; inner < ninner; inner++) {
                l = lrcp ? outer : inner;
                r = lrcp ? inner : outer;
                for (c = 0; c < g->ncomps; c++) {
                    size_t ri = (size_t)c * (g->levels + 1) + r;
                    n = (size_t)((uint64_t)g->res[ri].pw * (uint64_t)g->res[ri].ph);
                    for (i = 0; i < n; i++) {
                        out[k].resolution = (uint32_t)ri;
                        out[k].precinct = (uint32_t)i;
                        out[k++].layer = (uint16_t)l;
                    }
                }
            }
    } else {
        /* Layers come last. Each precinct is visited at its anchor, the first
         * reference grid point of the precinct inside the tile; the other
         * loops are (component, resolution, anchor) in the order's nesting. */
        sort_entry *sorted = malloc((g->nprecincts ? g->nprecincts : 1) * sizeof *sorted);
        size_t ri, m = 0;
        if (sorted == NULL) {
            free(out);
            return hv_fail(error, error_size, "out of memory");
        }
        for (ri = 0; ri < nres; ri++) {
            const hv_resolution *res = &g->res[ri];
            n = (size_t)((uint64_t)res->pw * (uint64_t)res->ph);
            for (i = 0; i < n; i++) {
                int64_t px, py;
                uint64_t ax, ay;
                hv_precinct_cell(res, i, &px, &py);
                ax = ((uint64_t)px << res->ppx) * res->xstep;
                ay = ((uint64_t)py << res->ppy) * res->ystep;
                if (ax < (uint64_t)g->tx0)
                    ax = (uint64_t)g->tx0;
                if (ay < (uint64_t)g->ty0)
                    ay = (uint64_t)g->ty0;
                if (g->progression == HV_RPCL) {
                    sorted[m].key[0] = (uint64_t)res->r;
                    sorted[m].key[1] = ay;
                    sorted[m].key[2] = ax;
                    sorted[m].key[3] = (uint64_t)res->c;
                } else if (g->progression == HV_PCRL) {
                    sorted[m].key[0] = ay;
                    sorted[m].key[1] = ax;
                    sorted[m].key[2] = (uint64_t)res->c;
                    sorted[m].key[3] = (uint64_t)res->r;
                } else {
                    sorted[m].key[0] = (uint64_t)res->c;
                    sorted[m].key[1] = ay;
                    sorted[m].key[2] = ax;
                    sorted[m].key[3] = (uint64_t)res->r;
                }
                sorted[m].resolution = (uint32_t)ri;
                sorted[m++].precinct = (uint32_t)i;
            }
        }
        qsort(sorted, m, sizeof *sorted, compare_entries);
        for (i = 0; i < m; i++)
            for (l = 0; l < g->layers; l++) {
                out[k].resolution = sorted[i].resolution;
                out[k].precinct = sorted[i].precinct;
                out[k++].layer = (uint16_t)l;
            }
        free(sorted);
    }
    *packets = out;
    *count = k;
    return 0;
}
