/* hv_reader.h: steps through a JPEG 2000 file held in memory, from header
 * to header, using the decoders generated from ../spec/ (jpeg2000-io.asn1
 * and the whole-file model's types it shares).
 *
 * The reader never copies a payload. It reports where each box, marker
 * segment, tile-part and block of tile-part data starts and ends, and it
 * checks the framing rules of T.800 Annex A and I.4 on the way. Marker
 * segment bodies (SIZ, COD, QCD, PLT, COM) are decoded and checked by the
 * generated code, COD and QCD whole, SIZ, PLT and COM one element at a
 * time up to the end Lxxx gives; the decoded SIZ, COD and QCD stay
 * available. A list is never held in a struct at the standard's bound:
 * SIZ's components are allocated as many as the segment holds, and PLT
 * entries and COM text stay in the caller's buffer.
 *
 * Offsets are byte offsets into the caller's buffer.
 *
 * Errors: a function that fails returns the rule that fails, by the name
 * the corpus manifest uses, or lowercase prose naming what failed, and sets
 * *at (or cs->error_at) to where it failed: the box, marker segment or
 * entry at fault; for a rule on the whole file (how many boxes of a type
 * there are, or where the first of them is: file.two-boxes,
 * jp2.one-codestream, the rules of hv_rule_jpx and hv_rule_jp2h_place),
 * the end of the file. A function that succeeds leaves *at as it was. */
#ifndef HV_READER_H
#define HV_READER_H

#include <stddef.h>
#include <stdint.h>

#include "hv_rules.h"
#include "jpeg2000-io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens bounded JP2/JPX container access: signature, second-box File Type
 * and compatibility for its kind (hv_ftyp_is_jpx). Brand and MinV follow
 * reader rules; no serving size limit or box-body checks are imposed.
 * NULL on success with *ftyp and *boxes positioned after the File Type box.
 * Iterate with hv_boxes_next: later framing errors are reported there.
 * Boxes remain in file order, including mixed jp2c/ftbl, dtbl, metadata and
 * extension boxes. Fragment tables and data references stay opaque; opening
 * does not establish fragment validity, usable images or JPX presentation.
 * The iterator borrows buf. On failure both outputs are unchanged. */
const char *hv_container_open(const uint8_t *buf, size_t size, hv_boxes *boxes,
                              hv_ftyp *ftyp, size_t *at);

/* Locates the single top-level codestream of a JP2-compatible container:
 * signature, ftyp second with 'jp2 ' compatibility, and bounded top-level
 * boxes. The brand and MinV are not restricted. Box bodies, including the
 * codestream and image header, stay opaque. This is access, not conformance
 * validation or JPX interpretation. No INT_MAX size limit is imposed.
 * NULL on success with *jp2c borrowing buf; on failure *jp2c is unchanged
 * and *at identifies the error. */
const char *hv_read_jp2(const uint8_t *buf, size_t size, hv_box *jp2c, size_t *at);

/* Read only SOC and SIZ from a bounded codestream prefix. Returns the fixed
 * image/tile grid and component precision, sign and sampling fields. No Rsiz
 * capability, serving, COD or packet checks. Pass components=NULL to query the
 * fixed fields; otherwise capacity is a component count. Malformed framing or
 * fields fail with outputs unchanged. No allocation or borrowed output. */
const char *hv_read_siz(const uint8_t *buf, size_t size, SizFixed *fixed,
                        Component *components, size_t capacity, size_t *at);

/* A borrowed Fragment List: count fixed-size entries starting at start.
 * Create with hv_fragments_open; read fields with hv_fragment_read. */
typedef struct {
    const uint8_t *buf;
    size_t start, count;
} hv_fragments;

/* Reads a framed FTBL's children, requiring exactly one FLST, then NF and
 * the exact entry extent. Other permitted children stay opaque. No serving
 * one-fragment restriction. Fragment fields are decoded on access below. */
const char *hv_fragments_open(const uint8_t *buf, const hv_box *ftbl,
                              hv_fragments *fragments, size_t *at);
/* Zero-based index. Decodes OFF/LEN/DR using the generated Fragment type.
 * Does not resolve DR, add OFF+LEN, access fragment data or validate a
 * codestream. Outputs stay unchanged on failure in both functions. */
const char *hv_fragment_read(const hv_fragments *fragments, size_t index,
                             Fragment *fragment, size_t *at);

/* Reads NDR and verifies that a framed DTBL contains exactly that many
 * bounded URL boxes. Returns their iterator and count. URL bodies are read
 * separately below. No paths are resolved. Outputs stay unchanged on failure. */
const char *hv_references_open(const uint8_t *buf, const hv_box *dtbl,
                               hv_boxes *references, size_t *count, size_t *at);
/* Reads version-0 URL framing and its single terminating NUL. Returns LOC
 * bytes without the NUL, borrowing buf. Scheme, extension, encoding and path
 * resolution are left to the operation using it. Outputs stay unchanged on
 * failure. These readers require boxes obtained from a bounded iterator. */
const char *hv_url_read(const uint8_t *buf, const hv_box *box,
                        const uint8_t **loc, size_t *loc_size, size_t *at);

/* The JP2 header boxes (T.800 I.5.3), and in a JPX file the Reader
 * Requirements box (T.801 M.11.1): the standard layer of
 * ../spec/jp2-boxes.asn1, whose rules are hv_rules.c's. The server reads
 * none of them, but a client decodes the image by them, so a tool that
 * writes a file checks them.
 *
 * Header/tree validation currently retains the INT_MAX implementation limit.
 * Both first check the file's start as hv_served_jp2 and hv_served_jpx do
 * (file.signature, file.ftyp-second, file.ftyp-compatibility,
 * file.two-boxes; file.ftyp-brand in a JPX file), then the box tree
 * (hv_rule_box_tree, for the file's kind), then MinV (file.ftyp-minor).
 * hv_check_jp2h, for a JP2 file (any brand but 'jpx '): one or more
 * codestreams; exactly one jp2h, before the first; its children decoded
 * by the model's types and checked by hv_rules.c; ihdr and bpcc against
 * the first codestream's SIZ and main COD MCT; and the ihdr's IPR against
 * the file's IPR boxes (ihdr.ipr).
 * hv_check_jpx_headers, for a JPX file (the brand 'jpx '): the count
 * rules of hv_rule_jpx at the standard layer (one Reader Requirements box,
 * the third box; at most one dtbl; a codestream per jpch), then the box
 * tree, the fragments in the file (hv_rule_fragments), MinV 1, the Reader
 * Requirements box's contents decoded one part at a time (RreqHeader, the
 * features, NVF), no deprecated feature, and nothing after them
 * (rreq.extent); at most one jp2h, in a baseline file before the first
 * jp2c, ftbl, mdat, jpch and jplh; the children of jp2h, jplh and jpch, in box
 * order; each codestream's header (its jpch over jp2h's defaults, T.801
 * M.11.6) against its SIZ where the codestream is embedded; creg in every
 * jplh or none (jpx.creg); in a baseline file (jpxb) its first layer's
 * rules (hv_rule_jpxb_layer); and in a file that lists 'jp2 ' the JP2
 * rules on jp2h and the first codestream.
 * Both expect framing hv_boxes_next accepts at the top level. NULL, or the
 * rule that fails (the manifest's name where it has one) and *at its
 * offset. A header rule that compares boxes (ihdr.bpcc, header.pclr-cmap,
 * ihdr.width, ...) points at the codestream's header box: its jpch, else
 * jp2h, else (in a JPX file with neither) the codestream. Main-header framing,
 * placement, required COD/QCD counts, SIZ field framing/ranges, component
 * count and nonempty image extent are checked. SIZ tile-grid and capability
 * admission are left to packet/validation consumers. JP2 also
 * reads COD MCT for its channel checks; JPX does not interpret COD unless
 * JP2 compatibility is declared. Other COD fields and marker bodies stay
 * opaque. Full coding, tile-part and packet checks are separate through
 * hv_codestream_check. */
/* Reads a JP2 input header, ignoring MinV, PREC and APPROX as T.800
 * I.5.2 and I.5.3.3 require of readers. Other checks are those of
 * hv_check_jp2h. The returned entry pointers borrow buf. The tools must
 * normalize these ignored fields when writing their output. */
const char *hv_read_jp2h(const uint8_t *buf, size_t size, hv_header *header, size_t *at);
const char *hv_check_jp2h(const uint8_t *buf, size_t size, size_t *at);
const char *hv_check_jpx_headers(const uint8_t *buf, size_t size, size_t *at);

/* Read a selected JP2H/JP2C pair obtained from bounded box iterators.
 * Header child framing/fields and image/channel consistency are checked,
 * with the input PREC/APPROX conventions above. Other file boxes are
 * not visited. No file kind, size, header count/order, whole box-tree or IPR
 * availability check; those belong to the caller's input/output policy.
 * Entry pointers borrow buf. On failure header is unchanged and *at gives
 * the error offset; success leaves *at unchanged. */
const char *hv_read_jp2h_boxes(const uint8_t *buf, const hv_box *jp2h,
                               const hv_box *jp2c, hv_header *header, size_t *at);

/* hv_check_jpx_headers for a file whose ftyp brand is 'jpx ', else
 * hv_check_jp2h: the file's kind at the standard layer (hv_ftyp_is_jpx),
 * whatever its name. */
const char *hv_check_headers(const uint8_t *buf, size_t size, size_t *at);

/* ------------------------------------------------------------------------
 * Codestream, T.800 Annex A
 * ------------------------------------------------------------------------ */

/* One marker and its bounded payload. For marker-only codes, payload and
 * end are just past the code. SOT and SOP include their length and body. */
typedef struct {
    uint16_t code;
    size_t start, payload, end;
} hv_marker;

/* Reads framing at start in buf[0, limit), leaving bodies opaque. Checks
 * the marker code, segment length and bounds, not placement, body fields,
 * or codestream conformance. Unknown codes have a length, except the
 * reserved marker-only range 0xFF30..0xFF3F. Call only at a marker boundary,
 * not inside tile data. NULL on success; otherwise the framing error at
 * start. The result is unchanged on error. No allocation or ownership. */
const char *hv_marker_read(const uint8_t *buf, size_t start, size_t limit, hv_marker *out);

typedef enum {
    HV_SEGMENT,             /* main-header marker (segment), SIZ first */
    HV_TILE_PART,           /* SOT: start = SOT code, end = end of the tile-part */
    HV_TILE_SEGMENT,        /* tile-part header marker segment */
    HV_TILE_DATA,           /* from the byte after SOD to the end of the tile-part */
    HV_END                  /* EOC: start = its code, end = end of the codestream */
} hv_item_kind;

/* A PLT segment: Zplt, and its Iplt entries, which stay in the caller's
 * buffer at [start, end) and are read with hv_plt_next. The reader has
 * checked them all by the time it reports the segment, unless opened with
 * HV_READ_JPIP_INDEX. */
typedef struct {
    Zplt zplt;
    size_t start, end;      /* Iplt entries in buf[start, end) */
} hv_plt;

/* The next Iplt entry of buf[*pos, end), as the standard layer reads one
 * (any number of leading zero groups, Table A.36): 1 with its value in
 * *value and *pos past it; 0 at end (*pos == end); -1 for an entry that
 * does not end within end or whose value does not fit 64 bits
 * (plt.value-overflow), *pos unchanged. */
int hv_plt_next(const uint8_t *buf, size_t *pos, size_t end, uint64_t *value);

/* A COM segment: Rcom, and its text, in place in the caller's buffer. */
typedef struct {
    Rcom rcom;
    const uint8_t *text;
    size_t size;
} hv_com;

/* hv_codestream_next clears every field it does not set. */
typedef struct {
    hv_item_kind kind;
    uint16_t code;          /* marker code; 0 for HV_TILE_DATA */
    size_t start, end;
    SotSegment sot;         /* HV_TILE_PART, HV_TILE_SEGMENT, HV_TILE_DATA: the
                             * current tile-part's SOT; zero otherwise */
    uint32_t plt_padding;   /* HV_TILE_DATA: trailing zero PLT entries accepted;
                              * unknown (zero) with HV_READ_JPIP_INDEX */
    /* A decoded SIZ, COD, QCD, PLT or COM segment, NULL otherwise, in the
     * form hv_writer's hv_write_* take (for PLT, the entries through
     * hv_plt_next; the accessors below give the COD and QCD bodies, the
     * form hv_rules and hv_geometry take). Valid until the next
     * hv_codestream_next call; siz as long as hv_codestream_siz.
     * With HV_READ_JPIP, QCD and COM bodies stay opaque and their pointers
     * are NULL. With packet modes, only SIZ and main COD are decoded. */
    const hv_siz *siz;
    const CodSegment *cod;
    const QcdSegment *qcd;
    const hv_plt *plt;
    const hv_com *com;
} hv_item;

/* Select one operation, never a bitmask. All modes check bounded framing.
 * Validation interprets supported marker bodies. Packet operations interpret
 * SIZ/COD and layout, leaving copied/dropped bodies opaque. JPIP operations
 * enforce serving requirements, leaving QCD/COM opaque for the decoder.
 * INDEX leaves PLT entries for hv_plt_reader; reaching HV_END does not finish
 * that validation. PACKETS leaves packet-header validation to its consumer. */
typedef enum {
    HV_READ_VALIDATE,
    HV_READ_PADDED,             /* validation, allowing trailing zero PLT entries */
    HV_READ_JPIP,               /* serving checks, including PLT coverage */
    HV_READ_JPIP_INDEX,         /* serving structure; PLT checked when indexed */
    HV_READ_PACKETS,            /* packet processing, general image geometry */
    HV_READ_PACKETS_JPIP        /* packet processing for JPIP output geometry */
} hv_read_mode;

/* Incremental PLT validation. No allocation or ownership of the input.
 * Initialize once per codestream. Begin each segment in traversal order,
 * then read it to completion before beginning another. A value of zero is
 * padding, not a packet. Copy hv_plt descriptors before advancing the
 * structural reader. The cursor retains offsets, not a buffer pointer: each
 * read takes the current buffer base. The buffer may move between calls, but
 * its contents and the offsets must still describe the same unchanged file.
 *
 * With HV_PLT_PROFILE, end_tile checks coverage and resets the tile-part state;
 * end checks the total packet count against hv_rule_packets, computed from
 * SIZ and COD before releasing them. Neither completion may be omitted
 * when claiming full validation. Pausing before completion is allowed, but
 * leaves the unread suffix unvalidated. Errors are terminal until init.
 *
 * Eager traversal uses this same decoder with its own standard-layer
 * tile-part checks. end_tile/end are for HV_PLT_PROFILE consumers only. */
typedef enum { HV_PLT_STANDARD, HV_PLT_PADDING, HV_PLT_PROFILE } hv_plt_mode;

typedef struct {
    size_t pos, end;
    hv_plt_mode mode;
    hv_zplt zplt;
    hv_plt_count count;
    uint64_t sum;
    uint32_t zeros;
    uint16_t cached_byte; /* 256 until a one-byte entry has been decoded */
    uint8_t cached_value; /* a decoded one-byte Iplt is in 0..127 */
    const char *rule_error; /* deferred until structural decoding completes */
    const char *error;
} hv_plt_reader;

/* PLT policy is independent of structural traversal. JPIP indexers use
 * HV_PLT_PROFILE and must complete coverage checks before publishing data. */
void hv_plt_init(hv_plt_reader *reader, hv_plt_mode mode);
/* NULL or reader->error; ranges must come from the structural reader. */
const char *hv_plt_begin(hv_plt_reader *reader, const hv_plt *plt);
/* 1 with a length, 0 at segment end, -1 with reader->error. On a rule
 * violation, drains the rest of that segment before failing so malformed
 * encoding still takes precedence over Zplt and entry rules. buf must hold
 * the file through the saved segment end for the duration of this call. */
int hv_plt_read(hv_plt_reader *reader, const uint8_t *buf, uint64_t *value);
/* Check a tile-part's saved segment sequence without decoding any entries.
 * at must be non-NULL. A bad sequence sets it to the offending segment's
 * first entry; an empty list leaves it unchanged. */
const char *hv_plt_check_segments(const hv_plt *segments, size_t count, size_t *at);
/* Profile packet indexing: skips padding and checks coverage before returning
 * a nonzero packet. last_segment describes the current tile-part's segment
 * list. offset is relative to its SOD data. Returns 1, 0 or -1 like read.
 * packets is the codestream's expected nonzero packet count. Drain the
 * remaining segments and complete the tile/codestream before publishing its
 * final logical packet, so malformed suffixes cannot escape validation. */
int hv_plt_packet(hv_plt_reader *reader, const uint8_t *buf, uint64_t data_size,
                  int last_segment, uint64_t packets, uint64_t *offset, uint64_t *length);
const char *hv_plt_end_tile(hv_plt_reader *reader, size_t data_size);
const char *hv_plt_end(hv_plt_reader *reader, uint64_t packets);

typedef struct {
    const uint8_t *buf;
    size_t pos, end;
    hv_read_mode mode;
    int state;
    size_t siz_end;         /* just past the SIZ segment */
    SizFixed siz_fixed;     /* decoded SIZ: its fixed part, */
    Component *components;  /* its components (allocated), */
    hv_siz siz;             /* and the two as hv_rules takes them */
    CodSegment cod;     /* decoded main-header COD */
    CodSegment tile_cod;/* decoded tile-part COD */
    QcdSegment qcd;     /* decoded main-header QCD */
    QcdSegment tile_qcd;/* decoded tile-part QCD */
    hv_com com;             /* last decoded COM */
    hv_plt plt;             /* last decoded PLT */
    uint32_t tiles;         /* tiles Isot can address: min(grid, 65,535) */
    int cods, qcds, tile_parts;
    /* The tile-parts seen, per tile: tile i's counts are
     * tile_pages[i / 256][i % 256]. The ceil(tiles / 256) page pointers are
     * allocated with the first tile-part, each page (of 256 tiles, the
     * last of the rest) when one of its tiles first appears, so that the
     * work of a codestream is bounded by its tile-parts, not by the tiles
     * its SIZ declares. */
    hv_tile_count **tile_pages;
    uint32_t unfinished;    /* tiles short of their TNsot (hv_rule_tile_part) */
    SotSegment sot;         /* current tile-part */
    size_t tp_start, tp_end;
    int tp_cod, tp_qcd, plts;
    hv_plt_reader plt_reader;
    int part_without_plt;   /* a tile-part header held no PLT */
    int layout_override;    /* COC, POC or PPM in the main header, or COD,
                             * COC, POC or PPT in a tile-part header */
    hv_segments segments;   /* the rules across segments (hv_rules.h) */
    hv_component *segment_components;   /* theirs, one per component
                                         * (allocated), NULL with HV_READ_JPIP or packet modes */
    const char *error;
    size_t error_at;
} hv_codestream;

/* Opens the selected operation and reads SOC/SIZ. Returns 0, or -1 with
 * cs->error and cs->error_at. Always close after an open attempt, including
 * failure. Closing releases allocations; no payload is copied. The caller
 * owns this stack-allocatable cursor and must not change its fields. */
int hv_codestream_open(hv_codestream *cs, const uint8_t *buf, size_t start, size_t end,
                       hv_read_mode mode);

/* 1: *item is the next item. 0: after HV_END. -1: invalid (cs->error). */
int hv_codestream_next(hv_codestream *cs, hv_item *item);

/* Frees what the reader allocated. cs->error and cs->error_at stay; the
 * accessors below return NULL. */
void hv_codestream_close(hv_codestream *cs);

/* Reads the codestream in buf[start, end) through with the selected operation, rejecting
 * HV_READ_JPIP_INDEX. NULL, or the reader's error and *at its offset. */
const char *hv_codestream_check(const uint8_t *buf, size_t start, size_t end, hv_read_mode mode,
                                size_t *at);

/* The main header's SIZ once hv_codestream_open has succeeded (NULL until
 * SIZ is accepted), and the bodies of its COD and QCD once the reader has
 * reported them: NULL before, for a segment the reader rejected, and after
 * hv_codestream_close. hv_rules and hv_geometry take these; the items
 * give the whole COD and QCD segments, for hv_writer. The QCD accessor
 * returns NULL under HV_READ_JPIP or packet modes, which leave its body opaque. */
const hv_siz *hv_codestream_siz(const hv_codestream *cs);
const Cod *hv_codestream_cod(const hv_codestream *cs);
const Qcd *hv_codestream_qcd(const hv_codestream *cs);

#ifdef __cplusplus
}
#endif

#endif
