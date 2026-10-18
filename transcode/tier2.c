/* tier2.c: see tier2.h. Clause numbers are T.800's. */
#include "tier2.h"

#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_codes.h"
#include "jpeg2000/hv_error.h"

/* Tag-tree value of a node not yet known: above any layer or bit-plane. */
#define INF ((int32_t)1 << 30)
/* Code-block lengths saturate here: such a length cannot fit in the tile,
 * and the caller then reports the overrun. */
#define HUGE_LENGTH ((uint64_t)1 << 62)

/* An SOP marker segment (A.8.1): the marker, then Lsop = 4 and Nsop. */
enum { LSOP = 4, SOP_SIZE = 2 + LSOP };
/* A marker without a segment: EPH. */
enum { MARKER_SIZE = 2 };

/* hvJP2K's messages for a packet header and a packet that run past the
 * tile's data. */
#define INCOMPLETE_HEADER "truncated input is not supported: incomplete packet header"
#define OVERRUNS_THE_TILE "truncated input is not supported: packet data overruns the tile"

int hv_codeblocks_init(hv_codeblocks *cb, size_t nblocks, size_t max_contributions) {
    size_t i;
    memset(cb, 0, sizeof *cb);
    cb->max_contributions = max_contributions;
    if (nblocks > SIZE_MAX / sizeof *cb->blocks ||
        (cb->blocks = malloc((nblocks ? nblocks : 1) * sizeof *cb->blocks)) == NULL)
        return -1;
    cb->nblocks = nblocks;
    for (i = 0; i < nblocks; i++) {
        cb->blocks[i].first_layer = -1;
        cb->blocks[i].zero_planes = -1;
        cb->blocks[i].first = cb->blocks[i].last = HV_NONE;
    }
    return 0;
}

void hv_codeblocks_free(hv_codeblocks *cb) {
    free(cb->blocks);
    free(cb->contrib);
    memset(cb, 0, sizeof *cb);
}

/* Appends a contribution of block blk. 0; -1 out of memory; -2 past
 * cb->max_contributions. */
static int add_contribution(hv_codeblocks *cb, size_t blk, int layer, int passes,
                            uint64_t length) {
    hv_contribution *c;
    size_t k = cb->ncontrib;
    if (k == cb->max_contributions)
        return -2;
    if (k == cb->capacity) {
        size_t n = cb->capacity ? 2 * cb->capacity : 1024;
        hv_contribution *grown = n > SIZE_MAX / sizeof *grown ? NULL
                                 : realloc(cb->contrib, n * sizeof *grown);
        if (grown == NULL)
            return -1;
        cb->contrib = grown;
        cb->capacity = n;
    }
    c = &cb->contrib[k];
    c->offset = 0;
    c->length = length;
    c->next = HV_NONE;
    c->layer = (uint16_t)layer;
    c->passes = (uint8_t)passes;
    if (cb->blocks[blk].first == HV_NONE)
        cb->blocks[blk].first = k;
    else
        cb->contrib[cb->blocks[blk].last].next = k;
    cb->blocks[blk].last = k;
    cb->ncontrib = k + 1;
    return 0;
}

/* ------------------------------------------------------------------------
 * Packet header bits (B.10.1): after a 0xFF byte the next byte carries
 * only 7 bits, its most significant bit is a stuffed 0.
 * ------------------------------------------------------------------------ */

/* How decoding a packet header ended. */
typedef enum {
    HEADER_OK,
    HEADER_OVERRUN,             /* it runs past its tile-part */
    HEADER_STUFFING,            /* a byte after 0xFF has its top bit set */
    HEADER_NOMEM,
    HEADER_LIMIT                /* more contributions than cb->max_contributions */
} header_status;

typedef struct {
    const uint8_t *p, *end;     /* next byte; end of the tile-part */
    unsigned buf;
    int ct;
    header_status error;        /* set, later reads give 0 */
} bit_reader;

static int read_bit(bit_reader *rd) {
    if (rd->ct == 0) {
        if (rd->error != HEADER_OK)
            return 0;
        if (rd->p >= rd->end) {
            rd->error = HEADER_OVERRUN;
            return 0;
        }
        if (rd->buf == 0xFF && (*rd->p & 0x80)) {
            rd->error = HEADER_STUFFING;
            return 0;
        }
        rd->ct = rd->buf == 0xFF ? 7 : 8;
        rd->buf = *rd->p++;
    }
    rd->ct--;
    return (rd->buf >> rd->ct) & 1;
}

static uint64_t read_bits(bit_reader *rd, int64_t n) {
    uint64_t value = 0;
    for (; n > 0 && rd->error == HEADER_OK; n--) {
        int b = read_bit(rd);
        if (value < HUGE_LENGTH) {
            value = value * 2 + (unsigned)b;
            if (value > HUGE_LENGTH)
                value = HUGE_LENGTH;
        }
    }
    return value;
}

/* The header ends after the stuffed byte required after a final 0xFF (B.10.1). */
static const uint8_t *read_end(bit_reader *rd) {
    if (rd->buf != 0xFF)
        return rd->p;
    if (rd->p == rd->end) {
        rd->error = HEADER_OVERRUN;
        return rd->p;
    }
    if (*rd->p & 0x80) rd->error = HEADER_STUFFING;
    return rd->p + 1;
}

typedef struct {
    uint8_t *out;
    size_t size, capacity;
    unsigned cur;
    int n, cap;
    int failed;                 /* out of memory; later writes do nothing */
} bit_writer;

static void put_byte(bit_writer *wr, unsigned byte) {
    if (wr->failed)
        return;
    if (wr->size == wr->capacity) {
        if (wr->capacity > SIZE_MAX / 2) {
            wr->failed = 1;
            return;
        }
        size_t n = wr->capacity ? 2 * wr->capacity : 256;
        uint8_t *grown = realloc(wr->out, n);
        if (grown == NULL) {
            wr->failed = 1;
            return;
        }
        wr->out = grown;
        wr->capacity = n;
    }
    wr->out[wr->size++] = (uint8_t)byte;
}

static void put_bit(bit_writer *wr, int b) {
    wr->cur = (wr->cur << 1) | (unsigned)b;
    if (++wr->n == wr->cap) {
        put_byte(wr, wr->cur);
        wr->cap = wr->cur == 0xFF ? 7 : 8;
        wr->cur = 0;
        wr->n = 0;
    }
}

/* v in n bits, the most significant first: bits above the 64th are 0,
 * written without shifting v by 64 or more. */
static void put_bits(bit_writer *wr, uint64_t v, int n) {
    while (n > 0) {
        n--;
        put_bit(wr, n < 64 && ((v >> n) & 1));
    }
}

static void header_begin(bit_writer *wr) {
    wr->size = 0;
    wr->cur = 0;
    wr->n = 0;
    wr->cap = 8;
}

static void header_end(bit_writer *wr) {
    if (wr->n)
        put_byte(wr, wr->cur << (wr->cap - wr->n));
    /* A packet header must not end with 0xFF. */
    if (wr->size && wr->out[wr->size - 1] == 0xFF)
        put_byte(wr, 0);
}

/* ------------------------------------------------------------------------
 * Tag trees (B.10.2): one per precinct and band with code-blocks, all in
 * shared arrays. A tree's levels follow each other, leaves first, so a
 * node's parent has a larger index.
 * ------------------------------------------------------------------------ */

typedef struct {
    int32_t *parent, *value, *low;
    uint8_t *known;
} tag_trees;

static size_t tree_size(int w, int h) {
    size_t total = 0;
    for (;;) {
        total += (size_t)w * h;
        if ((size_t)w * h == 1)
            return total;
        w = (w + 1) >> 1;
        h = (h + 1) >> 1;
    }
}

static void tree_init(tag_trees *t, size_t base, int w, int h) {
    size_t level = base, next, k;
    int x, y, pw;
    for (;;) {
        next = level + (size_t)w * h;
        if ((size_t)w * h == 1) {
            t->parent[level] = -1;
            break;
        }
        pw = (w + 1) >> 1;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                t->parent[level + (size_t)y * w + x] =
                        (int32_t)(next + (size_t)(y >> 1) * pw + (x >> 1));
        level = next;
        w = pw;
        h = (h + 1) >> 1;
    }
    for (k = base; k < next; k++) {
        t->value[k] = INF;
        t->low[k] = 0;
        t->known[k] = 0;
    }
}

/* Sets every node to the minimum of its children, for encoding. */
static void tree_propagate(tag_trees *t, size_t base, size_t size) {
    size_t k;
    for (k = base; k < base + size; k++) {
        int32_t p = t->parent[k];
        if (p >= 0 && t->value[k] < t->value[p])
            t->value[p] = t->value[k];
    }
}

static int tree_path(const tag_trees *t, size_t leaf, int32_t *stack) {
    int depth = 0;
    int64_t n = (int64_t)leaf;
    while (n >= 0) {
        stack[depth++] = (int32_t)n;
        n = t->parent[n];
    }
    return depth;
}

/* Whether the leaf's value is below threshold, reading what is needed. */
static int tree_decode(tag_trees *t, bit_reader *rd, size_t leaf, int32_t threshold) {
    int32_t stack[64], low = 0;
    int depth = tree_path(t, leaf, stack), i;
    for (i = depth - 1; i >= 0; i--) {
        int32_t n = stack[i];
        if (low < t->low[n])
            low = t->low[n];
        while (low < threshold && low < t->value[n]) {
            if (read_bit(rd))
                t->value[n] = low;
            else if (rd->error != HEADER_OK)
                return 0;
            else
                low++;
        }
        t->low[n] = low;
    }
    return t->value[leaf] < threshold;
}

static void tree_encode(tag_trees *t, bit_writer *wr, size_t leaf, int32_t threshold) {
    int32_t stack[64], low = 0;
    int depth = tree_path(t, leaf, stack), i;
    for (i = depth - 1; i >= 0; i--) {
        int32_t n = stack[i];
        if (low < t->low[n])
            low = t->low[n];
        while (low < threshold) {
            if (low >= t->value[n]) {
                if (!t->known[n]) {
                    put_bit(wr, 1);
                    t->known[n] = 1;
                }
                break;
            }
            put_bit(wr, 0);
            low++;
        }
        t->low[n] = low;
    }
}

/* ------------------------------------------------------------------------
 * Coding passes (B.10.6, Table B.4) and lengths (B.10.7)
 * ------------------------------------------------------------------------ */

static int read_passes(bit_reader *rd) {
    uint64_t n;
    if (!read_bit(rd))
        return 1;
    if (!read_bit(rd))
        return 2;
    if ((n = read_bits(rd, 2)) != 3)
        return (int)(3 + n);
    if ((n = read_bits(rd, 5)) != 31)
        return (int)(6 + n);
    return (int)(37 + read_bits(rd, 7));
}

static void write_passes(bit_writer *wr, int n) {
    if (n == 1)
        put_bit(wr, 0);
    else if (n == 2)
        put_bits(wr, 2, 2);
    else if (n <= 5)
        put_bits(wr, 12 | (unsigned)(n - 3), 4);
    else if (n <= 36)
        put_bits(wr, 0x1E0 | (unsigned)(n - 6), 9);
    else
        put_bits(wr, ((uint64_t)0x1FF << 7) | (unsigned)(n - 37), 16);
}

static int floorlog2(uint64_t n) {
    int r = -1;
    while (n) {
        n >>= 1;
        r++;
    }
    return r;
}

/* ------------------------------------------------------------------------
 * The tile's precincts and their tag trees
 * ------------------------------------------------------------------------ */

#define NO_TREE SIZE_MAX

/* Per-pass state: the tag trees of every precinct and band, and Lblock
 * (B.10.7.1) of every code-block. */
typedef struct {
    size_t *tree;               /* tree base per precinct * 3 + band, or NO_TREE */
    tag_trees incl, zbp;        /* inclusion (B.10.4), zero bit-planes (B.10.5) */
    int64_t *lblock;
} pass_state;

static void state_free(pass_state *s) {
    free(s->tree);
    free(s->incl.parent);
    free(s->incl.value);
    free(s->incl.low);
    free(s->incl.known);
    free(s->zbp.parent);
    free(s->zbp.value);
    free(s->zbp.low);
    free(s->zbp.known);
    free(s->lblock);
}

static int trees_alloc(tag_trees *t, size_t n) {
    n = n ? n : 1;
    t->parent = malloc(n * sizeof *t->parent);
    t->value = malloc(n * sizeof *t->value);
    t->low = malloc(n * sizeof *t->low);
    t->known = malloc(n);
    return t->parent && t->value && t->low && t->known ? 0 : -1;
}

/* A packet's precinct: its resolution, number and grid cell. */
typedef struct {
    const hv_resolution *res;
    uint64_t number;
    int64_t px, py;
} precinct_ref;

static precinct_ref precinct_of(const hv_geometry *g, const hv_packet *pk) {
    precinct_ref p;
    p.res = &g->res[pk->resolution];
    p.number = p.res->first_precinct + pk->precinct;
    hv_precinct_cell(p.res, pk->precinct, &p.px, &p.py);
    return p;
}

/* Sets up the tag trees of every precinct and band with code-blocks. For
 * encoding (cb given) the leaves get their values and the other nodes the
 * minimum of theirs; for decoding every value starts unknown. */
static int state_init(pass_state *s, const hv_geometry *g, const hv_codeblocks *cb,
                      size_t nblocks, int nlayers) {
    size_t nres = (size_t)g->ncomps * (g->levels + 1), ri, nodes = 0, k;
    uint64_t i, n;
    int pass, b;

    memset(s, 0, sizeof *s);
    s->tree = malloc(((size_t)g->nprecincts * 3 + 1) * sizeof *s->tree);
    s->lblock = malloc((nblocks ? nblocks : 1) * sizeof *s->lblock);
    if (s->tree == NULL || s->lblock == NULL)
        return -1;
    for (k = 0; k < nblocks; k++)
        s->lblock[k] = 3;
    /* Pass 0 lays out the trees, pass 1 fills them in. */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1 && (trees_alloc(&s->incl, nodes) != 0 || trees_alloc(&s->zbp, nodes) != 0))
            return -1;
        for (ri = 0; ri < nres; ri++) {
            const hv_resolution *res = &g->res[ri];
            n = (uint64_t)(res->pw * res->ph);
            for (i = 0; i < n; i++) {
                size_t *slot = &s->tree[(res->first_precinct + i) * 3];
                int64_t px, py, cx, cy;
                hv_precinct_cell(res, i, &px, &py);
                for (b = 0; b < 3; b++) {
                    hv_block_rect rect = {0, 0, 0, 0};
                    size_t leaf;
                    if (b < res->nbands)
                        rect = hv_precinct_blocks(res, b, px, py);
                    if (pass == 0) {
                        slot[b] = NO_TREE;
                        if (rect.w * rect.h > 0) {
                            slot[b] = nodes;
                            nodes += tree_size((int)rect.w, (int)rect.h);
                        }
                        continue;
                    }
                    if ((leaf = slot[b]) == NO_TREE)
                        continue;
                    tree_init(&s->incl, leaf, (int)rect.w, (int)rect.h);
                    tree_init(&s->zbp, leaf, (int)rect.w, (int)rect.h);
                    if (cb == NULL)
                        continue;
                    for (cy = rect.cy0; cy < rect.cy0 + rect.h; cy++)
                        for (cx = rect.cx0; cx < rect.cx0 + rect.w; cx++, leaf++) {
                            const hv_block *blk =
                                    &cb->blocks[hv_block_number(&res->band[b], cx, cy)];
                            s->incl.value[leaf] = blk->first_layer >= 0 ? blk->first_layer
                                                                        : nlayers;
                            s->zbp.value[leaf] = blk->zero_planes >= 0 ? blk->zero_planes : 0;
                        }
                    tree_propagate(&s->incl, slot[b], tree_size((int)rect.w, (int)rect.h));
                    tree_propagate(&s->zbp, slot[b], tree_size((int)rect.w, (int)rect.h));
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Decoding (B.10)
 * ------------------------------------------------------------------------ */

/* The marker `code` (HV_SOT, HV_SOP, HV_EPH) at buf[pos] inside the
 * tile-part. */
static int marker_at(const uint8_t *buf, size_t pos, size_t end, uint16_t code) {
    return pos + 1 < end && buf[pos] == code >> 8 && buf[pos + 1] == (code & 0xFF);
}

/* Decodes one packet header at buf[*pos, end), records its contributions
 * (offsets not yet set) and moves *pos past it. */
static header_status decode_header(const precinct_ref *p, int l, const uint8_t *buf,
                                   size_t *pos, size_t end, pass_state *s,
                                   hv_codeblocks *cb) {
    bit_reader rd = {buf + *pos, buf + end, 0, 0, HEADER_OK};
    int b;

    if (read_bit(&rd)) {        /* zero-length packet otherwise (B.10.3) */
        for (b = 0; b < p->res->nbands; b++) {
            hv_block_rect rect = hv_precinct_blocks(p->res, b, p->px, p->py);
            size_t leaf = s->tree[p->number * 3 + b];       /* NO_TREE: no code-blocks */
            int64_t cx, cy;
            for (cy = rect.cy0; cy < rect.cy0 + rect.h; cy++)
                for (cx = rect.cx0; cx < rect.cx0 + rect.w; cx++, leaf++) {
                    size_t k = (size_t)hv_block_number(&p->res->band[b], cx, cy);
                    hv_block *blk = &cb->blocks[k];
                    uint64_t length;
                    int passes;
                    if (blk->first_layer < 0) {
                        if (!tree_decode(&s->incl, &rd, leaf, l + 1))
                            continue;
                        tree_decode(&s->zbp, &rd, leaf, INF);
                        blk->zero_planes = s->zbp.value[leaf];
                        blk->first_layer = l;
                    } else if (!read_bit(&rd)) {
                        continue;
                    }
                    passes = read_passes(&rd);
                    while (read_bit(&rd))
                        s->lblock[k]++;
                    length = read_bits(&rd, s->lblock[k] + floorlog2((uint64_t)passes));
                    if (rd.error != HEADER_OK)
                        return rd.error;
                    switch (add_contribution(cb, k, l, passes, length)) {
                    case 0:
                        break;
                    case -1:
                        return HEADER_NOMEM;
                    default:
                        return HEADER_LIMIT;
                    }
                }
        }
    }
    /* After an error every bit reads 0, which records nothing more, and
     * read_end keeps the error. */
    *pos = (size_t)(read_end(&rd) - buf);
    return rd.error;
}

/* Whether the n bytes at p hold 0xFF followed by a byte above 0x8F. */
static int holds_marker(const uint8_t *p, size_t n) {
    const uint8_t *end = p + n, *ff;
    while (p < end && (ff = memchr(p, 0xFF, (size_t)(end - p))) != NULL) {
        if (ff + 1 < end && ff[1] > 0x8F)
            return 1;
        p = ff + 1;
    }
    return 0;
}

/* Tile-parts end between packets (A.4.2); skip ended and empty ones. */
static void advance_tile_part(const hv_tile_data *tile, size_t *part, size_t *pos) {
    while (*part < tile->nparts && *pos == tile->parts[*part].end)
        if (++*part < tile->nparts)
            *pos = tile->parts[*part].start;
}

/* Consume the packet's contributions in header order. */
static int read_packet_body(const uint8_t *buf, size_t *pos, size_t end,
                            hv_codeblocks *cb, size_t first, size_t pkt, int last_part,
                            char *error, size_t error_size) {
    size_t i;
    for (i = first; i < cb->ncontrib; i++) {
        hv_contribution *c = &cb->contrib[i];
        if (c->length > end - *pos) {
            return hv_fail(error, error_size, "%s",
                           last_part ? OVERRUNS_THE_TILE
                                     : "packet crosses a tile-part boundary");
        }
        /* B.10.7: "The sequence of bytes actually included for any
         * given code-block must not end in a 0xFF"; and within it, a
         * 0xFF is followed by a byte of at most 0x8F, as the coders'
         * bit stuffing leaves it: a decoder reads 0xFF and a byte above
         * 0x8F as a marker, which ends the coded data (C.3.4, Figure
         * C.19; D.4, NOTE). The output places the bytes next to others,
         * where such a pair would read as a marker. */
        if (c->length > 0 && buf[*pos + (size_t)c->length - 1] == 0xFF) {
            return hv_fail(error, error_size,
                           "a code-block's contribution to packet %zu ends in 0xFF "
                           "(T.800 B.10.7)", pkt);
        }
        if (holds_marker(buf + *pos, (size_t)c->length)) {
            return hv_fail(error, error_size,
                           "a code-block's contribution to packet %zu holds 0xFF and a "
                           "byte above 0x8F, a marker (T.800 C.3.4)", pkt);
        }
        c->offset = *pos;
        *pos += (size_t)c->length;
    }
    return 0;
}

int hv_read_packets(const hv_geometry *g, const hv_packet *packets, size_t npackets,
                    const hv_tile_data *tile, int sop, int eph, hv_codeblocks *cb,
                    char *error, size_t error_size) {
    const uint8_t *buf = tile->buf;
    const hv_span *parts = tile->parts;
    size_t nparts = tile->nparts;
    int zero_psot = tile->zero_psot;
    pass_state s;
    size_t pkt, part = 0, pos = nparts ? parts[0].start : 0, rest, i;
    int status = 0;

    if (state_init(&s, g, NULL, cb->nblocks, g->layers) != 0) {
        state_free(&s);
        return hv_fail(error, error_size, "out of memory");
    }
    for (pkt = 0; pkt < npackets && status == 0; pkt++) {
        precinct_ref p = precinct_of(g, &packets[pkt]);
        size_t end, first = cb->ncontrib;
        int last_part;
        advance_tile_part(tile, &part, &pos);
        if (part == nparts) {
            status = hv_fail(error, error_size,
                             "truncated input is not supported: missing packets");
            break;
        }
        end = parts[part].end;
        last_part = part + 1 == nparts;
        /* With Psot = 0 the last tile-part runs to EOC, so a tile-part the
         * input places after it lies inside its data; its SOT starts where
         * the next packet would. No packet can start with FF 90: after
         * 0xFF, a packet header's next byte is below 0x90 (bit stuffing,
         * B.10.1), and SOP is FF 91. This holds for the input tier2 reads,
         * whose headers it checks for stuffing; data that is not stuffed
         * would need another way to find the next tile-part. */
        if (zero_psot && marker_at(buf, pos, end, HV_SOT)) {
            status = hv_fail(error, error_size, "Psot=0 is only valid for the last tile-part");
            break;
        }
        /* SOP marker segment (A.8.1), allowed by Scod bit 1: Lsop = 4, and
         * Nsop numbers the tile's packets from 0, rolling over after 65,535. */
        if (sop && marker_at(buf, pos, end, HV_SOP)) {
            if (end - pos < SOP_SIZE || buf[pos + 2] != 0 || buf[pos + 3] != LSOP ||
                ((size_t)buf[pos + 4] << 8 | buf[pos + 5]) != (pkt & 0xFFFF)) {
                status = hv_fail(error, error_size, "invalid SOP marker segment before packet %zu",
                                 pkt);
                break;
            }
            pos += SOP_SIZE;
        }
        switch (decode_header(&p, packets[pkt].layer, buf, &pos, end, &s, cb)) {
        case HEADER_OK:
            break;
        case HEADER_OVERRUN:
            status = hv_fail(error, error_size, "%s",
                             last_part ? INCOMPLETE_HEADER : "packet crosses a tile-part boundary");
            break;
        case HEADER_STUFFING:
            status = hv_fail(error, error_size, "invalid bit stuffing in packet header %zu", pkt);
            break;
        case HEADER_NOMEM:
            status = hv_fail(error, error_size, "out of memory");
            break;
        case HEADER_LIMIT: {
            char number[32];
            status = hv_fail(error, error_size,
                             "code-block contribution count exceeds supported limit (%s)",
                             hv_grouped(cb->max_contributions, number));
            break;
        }
        }
        if (status != 0)
            break;
        if (pos > end) {        /* the stuffed byte after a final 0xFF */
            status = hv_fail(error, error_size, "%s",
                             last_part ? OVERRUNS_THE_TILE : "packet crosses a tile-part boundary");
            break;
        }
        /* EPH marker (A.8.2): required after every header by Scod bit 2. */
        if (eph) {
            if (!marker_at(buf, pos, end, HV_EPH)) {
                status = hv_fail(error, error_size, "missing EPH marker after packet header %zu",
                                 pkt);
                break;
            }
            pos += MARKER_SIZE;
        }
        status = read_packet_body(buf, &pos, end, cb, first, pkt, last_part,
                                  error, error_size);
    }
    if (status == 0) {
        advance_tile_part(tile, &part, &pos);
        if (part < nparts) {
            if (zero_psot && marker_at(buf, pos, parts[part].end, HV_SOT)) {
                status = hv_fail(error, error_size, "Psot=0 is only valid for the last tile-part");
            } else {
                rest = parts[part].end - pos;
                for (i = part + 1; i < nparts; i++)
                    rest += parts[i].end - parts[i].start;
                status = hv_fail(error, error_size, "%zu unparsed tile bytes", rest);
            }
        }
    }
    state_free(&s);
    return status;
}

/* ------------------------------------------------------------------------
 * Encoding (B.10)
 * ------------------------------------------------------------------------ */

int hv_write_packets(const hv_geometry *g, const hv_packet *packets, size_t npackets,
                     const uint8_t *buf, const hv_codeblocks *cb, hv_out *out,
                     uint64_t *lengths, char *error, size_t error_size) {
    pass_state s;
    bit_writer wr;
    size_t *cursor = NULL, *body = NULL, nbody, body_cap = 0, pkt, k;
    int nlayers = g->layers, b, status = 0;

    memset(&wr, 0, sizeof wr);
    if (state_init(&s, g, cb, cb->nblocks, nlayers) != 0 ||
        (cursor = malloc((cb->nblocks ? cb->nblocks : 1) * sizeof *cursor)) == NULL) {
        state_free(&s);
        return hv_fail(error, error_size, "out of memory");
    }
    /* Each code-block's next contribution; layers only increase per precinct. */
    for (k = 0; k < cb->nblocks; k++)
        cursor[k] = cb->blocks[k].first;

    for (pkt = 0; pkt < npackets && status == 0; pkt++) {
        precinct_ref p = precinct_of(g, &packets[pkt]);
        int l = packets[pkt].layer;
        uint64_t length;
        nbody = 0;
        header_begin(&wr);
        /* Never the zero-length packet: the inclusion information is always
         * written, even when no code-block contributes. */
        put_bit(&wr, 1);
        for (b = 0; b < p.res->nbands; b++) {
            hv_block_rect rect = hv_precinct_blocks(p.res, b, p.px, p.py);
            size_t leaf = s.tree[p.number * 3 + b];         /* NO_TREE: no code-blocks */
            int64_t cx, cy;
            for (cy = rect.cy0; cy < rect.cy0 + rect.h; cy++)
                for (cx = rect.cx0; cx < rect.cx0 + rect.w; cx++, leaf++) {
                    size_t n = (size_t)hv_block_number(&p.res->band[b], cx, cy);
                    const hv_block *blk = &cb->blocks[n];
                    const hv_contribution *c;
                    int log_passes, inc, m;
                    if (blk->first_layer < 0 || blk->first_layer > l) {
                        tree_encode(&s.incl, &wr, leaf, l + 1);
                        continue;
                    }
                    c = cursor[n] != HV_NONE && cb->contrib[cursor[n]].layer == l
                        ? &cb->contrib[cursor[n]] : NULL;
                    if (blk->first_layer == l) {
                        tree_encode(&s.incl, &wr, leaf, l + 1);
                        tree_encode(&s.zbp, &wr, leaf, INF);
                    } else {
                        put_bit(&wr, c != NULL);
                    }
                    if (c == NULL)
                        continue;
                    write_passes(&wr, c->passes);
                    log_passes = floorlog2(c->passes);
                    /* Lblock grows until the length fits (B.10.7.1). */
                    inc = floorlog2(c->length) + 1 - log_passes - (int)s.lblock[n];
                    if (inc < 0)
                        inc = 0;
                    for (m = 0; m < inc; m++)
                        put_bit(&wr, 1);
                    put_bit(&wr, 0);
                    s.lblock[n] += inc;
                    put_bits(&wr, c->length, (int)s.lblock[n] + log_passes);
                    if (nbody == body_cap) {
                        if (body_cap > SIZE_MAX / sizeof *body / 2) {
                            wr.failed = 1;
                            continue;
                        }
                        size_t cap = body_cap ? 2 * body_cap : 64;
                        size_t *grown = realloc(body, cap * sizeof *grown);
                        if (grown == NULL) {
                            wr.failed = 1;
                            continue;
                        }
                        body = grown;
                        body_cap = cap;
                    }
                    body[nbody++] = cursor[n];
                    cursor[n] = c->next;
                }
        }
        header_end(&wr);
        if (wr.failed) {
            status = hv_fail(error, error_size, "out of memory");
            break;
        }
        length = wr.size;
        for (k = 0; k < nbody; k++)
            length += cb->contrib[body[k]].length;
        if (out == NULL) {
            lengths[pkt] = length;
            continue;
        }
        /* The PLT was written from the first pass: the packets must match. */
        if (lengths[pkt] != length) {
            status = hv_fail(error, error_size, "packet %zu: %llu bytes, the PLT says %llu", pkt,
                             (unsigned long long)length, (unsigned long long)lengths[pkt]);
            break;
        }
        if (hv_write_bytes(out, wr.out, wr.size) != 0)
            status = hv_fail(error, error_size, "%s", out->error);
        for (k = 0; k < nbody && status == 0; k++) {
            const hv_contribution *c = &cb->contrib[body[k]];
            if (hv_write_bytes(out, buf + c->offset, (size_t)c->length) != 0)
                status = hv_fail(error, error_size, "%s", out->error);
        }
    }
    state_free(&s);
    free(cursor);
    free(body);
    free(wr.out);
    return status;
}
