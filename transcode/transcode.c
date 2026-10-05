/* transcode.c: see transcode.h. A port of hvJP2K's
 * jp2_precincts.transcode_codestream: the codestream is read with
 * hv_reader, laid out twice with hv_geometry (the input's precincts and
 * order, then the new ones), and written with hv_writer. */
#include "transcode.h"

#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_reader.h"
#include "tier2.h"

/* Memory bounds of one transcode, for what a header can declare with
 * little or no data behind it. Each component-resolution takes 360
 * bytes of layout (hv_geometry_limits), each code-block under 100 bytes of
 * state and tag-tree nodes, each output packet 20 bytes (its place in the
 * order and its PLT length) and at least a byte of output, and each output
 * precinct 64 bytes more (40 to put the packets in order, 24 to index its
 * tag trees). The input's packets are bounded by the tile's data, each
 * taking at least a byte of it (B.10.3), and take 12 bytes each: at most
 * 16,000,000 (192 MB; 1,024 code-blocks in 15,625 layers, or 64 x 64
 * precincts over 16,384 x 16,384 samples in 32 layers, take that many),
 * so that a file of empty packets cannot ask for 12 times its size. Each
 * contribution of a code-block to a layer that they signal takes 32 bytes,
 * and a packet header signals one in a few bits: at most 8,000,000
 * (250,000 code-blocks in 32 layers), 256 MB. */
#define MAX_RESOLUTIONS 65536
#define MAX_CODE_BLOCKS 250000
#define MAX_CONTRIBUTIONS 8000000
#define MAX_INPUT_PACKETS 16000000
#define MAX_OUTPUT_PACKETS 2000000
/* The same, as the messages state it. */
#define MAX_CODE_BLOCKS_TEXT "250,000"

/* Code-block styles (T.800 Table A.19) whose coding passes end where the
 * packet headers do not say: not supported. */
enum { CB_BYPASS = 0x01, CB_TERMALL = 0x04 };

/* ------------------------------------------------------------------------
 * Limits and the code-block partition
 * ------------------------------------------------------------------------ */

/* Nonzero if every non-empty band has the same code-blocks in both. */
static int same_partition(const hv_geometry *a, const hv_geometry *b) {
    size_t nres = (size_t)a->ncomps * (a->levels + 1), i;
    int k;
    for (i = 0; i < nres; i++)
        for (k = 0; k < a->res[i].nbands; k++) {
            const hv_band *x = &a->res[i].band[k], *y = &b->res[i].band[k];
            if (y->nx > 0 && y->ny > 0 && (x->xcb != y->xcb || x->ycb != y->ycb))
                return 0;
        }
    return 1;
}

/* ------------------------------------------------------------------------
 * Codestream (transcode_codestream)
 * ------------------------------------------------------------------------ */

/* The new COD: the input's, with the given precincts at every resolution,
 * RPCL order, and no SOP or EPH markers. */
static CodSegment new_cod(const CodSegment *in, int ppx, int ppy) {
    CodSegment cod = *in;
    int r;
    cod.body.scod.customPrecincts = TRUE;
    cod.body.scod.sopMarkers = FALSE;
    cod.body.scod.ephMarkers = FALSE;
    cod.body.sgcod.progression = HV_RPCL;
    cod.body.spcod.precincts.nCount = (int)in->body.spcod.levels + 1;
    for (r = 0; r <= (int)in->body.spcod.levels; r++) {
        cod.body.spcod.precincts.arr[r].ppx = ppx;
        cod.body.spcod.precincts.arr[r].ppy = ppy;
    }
    return cod;
}

/* One transcode: the input as read, both layouts of its tile, and the
 * code-blocks' contributions. */
typedef struct {
    char *error;
    size_t error_size;
    const uint8_t *buf;
    hv_codestream cs;
    hv_span parts[255];         /* the tile-parts' data (HV_TILE_DATA) */
    size_t nparts;
    int zero_psot;
    CodSegment cod;         /* the new COD */
    hv_geometry in, out;        /* the input's and the new layout */
    hv_packet *in_packets, *out_packets;
    size_t in_count, out_count;
    hv_codeblocks cb;
    uint64_t *lengths;          /* of the new packets */
} transcode;

static void transcode_free(transcode *t) {
    hv_codestream_close(&t->cs);
    hv_geometry_free(&t->in);
    hv_geometry_free(&t->out);
    hv_codeblocks_free(&t->cb);
    free(t->in_packets);
    free(t->out_packets);
    free(t->lengths);
}

/* A write to out failed: its error, as the transcode's. */
static int write_failed(transcode *t, const hv_out *out) {
    return hv_fail(t->error, t->error_size, "%s", out->error);
}

/* What the transcoder does with each marker of the input's headers, with
 * any hv_transcode_codestream profile: the one place that decides it.
 *
 * Main header: SIZ, QCD, QCC, RGN, CRG and COM are copied, as they
 * describe the image, the code-blocks or nothing the transcoder changes;
 * COD is replaced by the new one (read_codestream). TLM and PLM are
 * dropped: transcoding makes them stale. Everything else is rejected: COC
 * and POC would change the packet layout, PPM moves the packet headers, a
 * code T.800 does not define (Table A.2: 0xFF70, or an extension of
 * T.801) may describe the packets, and 0xFF30 to 0xFF3F, which have no
 * segment (A.1.3), are outside the served profile's main header, which
 * the JPIP output target requires (HV_READ_PACKETS_JPIP: its reader
 * rejects these codes itself, as main.marker-code, before they get here).
 *
 * Tile-part headers: PLT and COM are dropped with the headers, which the
 * transcoder writes anew; everything else is rejected. */
typedef enum { MARKER_REJECTED = -1, MARKER_DROPPED = 0, MARKER_COPIED = 1 } marker_action;

static marker_action main_marker(uint16_t code) {
    switch (code) {
    case HV_SIZ: case HV_QCD: case HV_QCC: case HV_RGN: case HV_CRG: case HV_COM:
        return MARKER_COPIED;
    case HV_TLM: case HV_PLM:
        return MARKER_DROPPED;
    default:
        return MARKER_REJECTED;
    }
}

static marker_action tile_marker(uint16_t code) {
    return code == HV_PLT || code == HV_COM ? MARKER_DROPPED : MARKER_REJECTED;
}

/* Reads the codestream in buf[start, end) with hv_reader. The main header
 * is written as it is read, with the new COD in place of the old one, and
 * its other markers copied, dropped or rejected (main_marker); those of
 * the tile-part headers are dropped or rejected (tile_marker). The
 * tile-parts' data is recorded where it is. The input's PLT is not used,
 * so its body stays opaque. */
static int read_codestream(transcode *t, size_t start, size_t end, int ppx, int ppy,
                           hv_output_profile profile, hv_out *out) {
    hv_item item;
    int status = hv_codestream_open(&t->cs, t->buf, start, end,
                                    profile == HV_OUTPUT_JPIP ? HV_READ_PACKETS_JPIP : HV_READ_PACKETS);

    /* General output retains the declaration; only the serving target permits
     * declarations whose profile requirements are not implemented here. */
    if (status == 0 && profile == HV_OUTPUT_JPEG2000) {
        const char *rule = hv_rule_rsiz(hv_codestream_siz(&t->cs)->fixed->rsiz);
        if (rule != NULL)
            return hv_fail(t->error, t->error_size, "%s at %zu", rule, start + 2);
    }
    if (status == 0 && hv_write_marker(out, HV_SOC) != 0)
        return write_failed(t, out);
    while (status == 0 && (status = hv_codestream_next(&t->cs, &item)) == 1) {
        status = 0;
        switch (item.kind) {
        case HV_SEGMENT:
            if (item.code == HV_COD) {
                t->cod = new_cod(item.cod, ppx, ppy);
                if (hv_write_cod(out, &t->cod) != 0)
                    return write_failed(t, out);
                break;
            }
            switch (main_marker(item.code)) {
            case MARKER_COPIED:
                if (hv_write_bytes(out, t->buf + item.start, item.end - item.start) != 0)
                    return write_failed(t, out);
                break;
            case MARKER_REJECTED:
                return hv_fail(t->error, t->error_size, "unsupported main header marker 0x%04X",
                               item.code);
            case MARKER_DROPPED:
                break;
            }
            break;
        case HV_TILE_SEGMENT:
            if (tile_marker(item.code) == MARKER_REJECTED)
                return hv_fail(t->error, t->error_size, "unsupported tile-part marker 0x%04X",
                               item.code);
            break;
        case HV_TILE_PART:
            /* One tile, so at most 255 tile-parts, and parts holds them:
             * the reader decodes TPsot as the model's SotSegment, whose
             * range is 0 to 254 (jpeg2000-io.asn1), and requires TPsot 0,
             * 1, 2, ... for the tile (sot.tpsot-sequence). parts must grow
             * if that range does. */
            if (t->cs.tiles != 1)
                return hv_fail(t->error, t->error_size,
                               "only single-tile codestreams are supported");
            t->zero_psot |= item.sot.psot == 0;
            break;
        case HV_TILE_DATA:
            t->parts[t->nparts].start = item.start;
            t->parts[t->nparts++].end = item.end;
            break;
        case HV_END:
            return 0;
        }
    }
    /* HV_END returns above: the reader failed. */
    return hv_fail(t->error, t->error_size, "%s at %zu", t->cs.error, t->cs.error_at);
}

/* Lays out the tile with the input's precincts and order and decodes its
 * packet headers. */
static int read_packets(transcode *t) {
    const Cod *cod = hv_codestream_cod(&t->cs);
    const hv_siz *siz = hv_codestream_siz(&t->cs);
    hv_tile_data tile = {t->buf, t->parts, t->nparts, t->zero_psot};
    hv_geometry_limits limits = {MAX_RESOLUTIONS, 0, "tile data"};
    size_t i;

    if (cod->spcod.cbStyle & (CB_BYPASS | CB_TERMALL))
        return hv_fail(t->error, t->error_size,
                       "code-block styles with bypass or termall are not supported");
    /* Empty components and resolutions have no packets, so the data bounds
     * the packets but not them. */
    for (i = 0; i < t->nparts; i++)
        limits.packets += t->parts[i].end - t->parts[i].start;
    if (limits.packets > MAX_INPUT_PACKETS) {
        limits.packets = MAX_INPUT_PACKETS;
        limits.packets_bound = NULL;
    }
    if (hv_geometry_init(&t->in, siz, cod, 0, &limits, t->error, t->error_size) != 0)
        return -1;
    if (t->in.nblocks > MAX_CODE_BLOCKS)
        return hv_fail(t->error, t->error_size,
                       "code-block count exceeds supported limit (" MAX_CODE_BLOCKS_TEXT ")");
    if (hv_geometry_packets(&t->in, &t->in_packets, &t->in_count, t->error,
                            t->error_size) != 0)
        return -1;
    if (hv_codeblocks_init(&t->cb, (size_t)t->in.nblocks, MAX_CONTRIBUTIONS) != 0)
        return hv_fail(t->error, t->error_size, "out of memory");
    return hv_read_packets(&t->in, t->in_packets, t->in_count, &tile, cod->scod.sopMarkers,
                           cod->scod.ephMarkers, &t->cb, t->error, t->error_size);
}

/* Lays out the tile with the new COD and writes it as one tile-part. Code-
 * block numbers are the same in both layouts while the code-block
 * partition is. The PLT needs the packet lengths, so the packets are
 * encoded twice, first for their lengths, then into the output. */
static int write_tile(transcode *t, int ppx, int ppy, hv_out *out) {
    hv_geometry_limits limits = {MAX_RESOLUTIONS, MAX_OUTPUT_PACKETS, NULL};
    size_t tp_start;

    /* The same code-block partition means the same code-blocks, so their
     * count is already bounded. */
    if (hv_geometry_init(&t->out, hv_codestream_siz(&t->cs), &t->cod.body, 0, &limits, t->error,
                         t->error_size) != 0)
        return -1;
    if (!same_partition(&t->in, &t->out))
        return hv_fail(t->error, t->error_size, "%dx%d precincts change the code-block partition",
                       1 << ppx, 1 << ppy);
    if (hv_geometry_packets(&t->out, &t->out_packets, &t->out_count, t->error,
                            t->error_size) != 0)
        return -1;
    if ((t->lengths = malloc((t->out_count + 1) * sizeof *t->lengths)) == NULL)
        return hv_fail(t->error, t->error_size, "out of memory");
    if (hv_write_packets(&t->out, t->out_packets, t->out_count, t->buf, &t->cb, NULL,
                         t->lengths, t->error, t->error_size) != 0)
        return -1;
    if (hv_begin_tile_part(out, 0, 0, 1, &tp_start) != 0 ||
        hv_write_plt(out, t->lengths, t->out_count) != 0 ||
        hv_write_marker(out, HV_SOD) != 0)
        return write_failed(t, out);
    if (hv_write_packets(&t->out, t->out_packets, t->out_count, t->buf, &t->cb, out,
                         t->lengths, t->error, t->error_size) != 0)
        return -1;
    if (hv_end_tile_part(out, tp_start) != 0 || hv_write_marker(out, HV_EOC) != 0)
        return write_failed(t, out);
    return 0;
}

int hv_transcode_codestream(const uint8_t *buf, size_t start, size_t end, int ppx, int ppy,
                            hv_output_profile profile, hv_out *out, char *error, size_t error_size) {
    size_t out_start = out->size;
    transcode t;
    int status;

    memset(&t, 0, sizeof t);
    t.error = error;
    t.error_size = error_size;
    t.buf = buf;
    error[0] = 0;
    if (out->error != NULL)                     /* an earlier write failed */
        return write_failed(&t, out);
    if (profile != HV_OUTPUT_JPEG2000 && profile != HV_OUTPUT_JPIP)
        status = hv_fail(t.error, t.error_size,
                         "invalid output profile");
    else if (ppx < 1 || ppx > 15 || ppy < 1 || ppy > 15)
        status = hv_fail(t.error, t.error_size,
                         "precinct dimensions must be powers of 2 from 2 to 32,768");
    else if ((status = read_codestream(&t, start, end, ppx, ppy, profile, out)) == 0 &&
             (status = read_packets(&t)) == 0)
        status = write_tile(&t, ppx, ppy, out);
    if (status != 0)
        hv_out_rewind(out, out_start);  /* nothing of a failed transcode */
    transcode_free(&t);
    return status;
}
