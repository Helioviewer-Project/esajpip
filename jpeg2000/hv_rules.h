/* hv_rules.h: the cross-field rules of the model (listed at the end of
 * ../spec/j2k-headers.asn1, ../spec/j2k-codestream.asn1 and
 * ../spec/jp2-boxes.asn1): marker segment bodies, tile-part indices, PLT
 * entries and packet counts, the segments of a codestream across its
 * headers, the box tree, JPX boxes and JP2 header boxes. They are written
 * once, here, and used both by the reader (hv_reader.c) and by the model's
 * harness (../spec/harness/crossfield_impl.h, crossfield.c), which labels
 * the corpus in ../tests/vectors/j2k; some take values the caller decoded,
 * some (the box tree, the fragments) the file's bytes. The rules on the
 * placement and count of segments and on the file's first boxes the reader
 * and the harness each check in their own code, the reader on the bytes and
 * the harness on the decoded model, under the same names. With HV_READ_JPIP
 * the reader applies
 * every JP2 rule the profile labels come from; its T.800 mode leaves some
 * out (see README.md, "What the reader checks").
 *
 * `profile` selects the layer of a function that checks rules of both: 0
 * for the standard (T.800, T.801), nonzero for the served profile
 * (JPIP_PROFILE.md), which adds the server's restrictions. A function
 * whose rules all belong to the profile (hv_rule_tile_part_count,
 * hv_rule_url, hv_rule_flst) or all to the standard (the header box rules)
 * takes no `profile`: the caller calls it only at that layer.
 *
 * Recording functions return void. Each check returns NULL when the rules
 * hold, otherwise the name of the rule that fails (hv_rule_box_tree: at the
 * top level, a box that is not framed, in hv_boxes_next's words); the checks do not allocate. The
 * corpus manifest uses the same names, for the rules a vector exercises;
 * some (file.size-limit, ...) no vector does. The box iterator, the File
 * Type box's reader, hv_rule_tiles and hv_rule_packets are not checks but
 * what the checks and the reader work from. */
#ifndef HV_RULES_H
#define HV_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "hv_codes.h"
#include "j2k-headers.h"
#include "jp2-boxes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SIZ as the rules and the geometry see it, however it was decoded: the
 * reader decodes SizFixed and then the components one at a time; the
 * harness decodes the whole-file model's Siz and points into it. */
typedef struct {
    const SizFixed *fixed;
    const Component *components;
    size_t ncomponents;        /* components decoded (Csiz when the segment is valid) */
} hv_siz;

/* Declaration support of the standard validator, also used by writers
 * that retain Rsiz without implementing additional profile requirements.
 * Takes a decoded 16-bit value; does not validate profile-specific fields. */
const char *hv_rule_rsiz(uint64_t rsiz);

/* SIZ (A.5.1, B.3): Csiz against the components decoded (siz.csiz-count),
 * the image area and the tile grid. Given the Sgcod of a COD (NULL before
 * there is one), also what the multiple component transform requires of
 * the first three components (A.6.1, G.2). The standard layer checks Rsiz
 * too: it validates 0, 1 and 2 and the existing Part 2 declarations
 * (hv_rule_codestream_header keeps the latter out of a JP2 file).
 * Other recognized Part 1 profiles return siz.unsupported-profile;
 * unhandled CAP signalling returns siz.unsupported-capabilities. These
 * mean validation is incomplete, not that the declaration is reserved.
 * Remaining values fail siz.rsiz. The server preserves any Rsiz. The
 * standard also requires at most 65,535 tiles (B.11, Table A.5;
 * siz.tile-count). The profile adds zero origins, unit sampling and a
 * single tile. */
const char *hv_rule_siz(const hv_siz *siz, const Sgcod *sgcod, int profile);

/* Layout requirements: component count, image/tile extents, at most 65,535
 * tiles and MCT component geometry. Does not classify or validate Rsiz
 * declarations. Decoded field ranges are the caller's responsibility. */
const char *hv_rule_siz_geometry(const hv_siz *siz, const Sgcod *sgcod);

/* Header consistency needs the component count, nonempty image area and
 * JP2 MCT component geometry. No tile-grid or capability validation. Field
 * ranges and exact segment framing belong to the bounded SIZ reader. */
const char *hv_rule_siz_header(const hv_siz *siz, int mct);

/* COD (A.6.1): the precinct sizes and the code-block area. The profile
 * adds: no SOP markers. */
const char *hv_rule_cod(const Scod *scod, const Spcod *spcod, int profile);

/* Not a check: the tiles of the grid SIZ describes that Isot (0 to 65,534)
 * can address, at most 65,535. 0 if SIZ has no tile grid (siz.tile-origin
 * and the other SIZ rules fail then). */
uint32_t hv_rule_tiles(const hv_siz *siz);

/* The tile-parts of one tile so far. The caller keeps one per tile that
 * appears, zeroed before its first tile-part, and one count for the
 * codestream, zeroed before its first: the tiles whose nonzero TNsot their
 * tile-parts have not reached yet. */
typedef struct {
    uint16_t parts;         /* tile-parts seen */
    uint8_t tnsot;          /* the nonzero TNsot seen, or 0 */
    uint8_t flags;          /* HV_TILE_*, for hv_segments */
} hv_tile_count;

/* Isot within the grid: below `tiles` (hv_rule_tiles). */
const char *hv_rule_isot(uint32_t tiles, uint64_t isot);

/* Counts one tile-part of the tile whose counts are t, in codestream order
 * (A.4.2): TPsot 0, 1, 2, ...; a nonzero TNsot above TPsot and the same in
 * every tile-part that gives one. */
const char *hv_rule_tile_part(hv_tile_count *t, uint32_t *unfinished, uint64_t tpsot,
                              uint64_t tnsot);

/* After the last tile-part: every tile with a nonzero TNsot has that many
 * tile-parts, none unfinished. */
const char *hv_rule_tile_parts_end(uint32_t unfinished);

/* The profile's limit on the tile-parts of a codestream: the server
 * indexes at most 64 (PacketIndex::MAX_SEGMENTS; TPsot 0 to 63 in
 * ../spec/j2k-codestream.asn1). `parts` counts them so far. Profile only:
 * the standard sets no such limit. */
enum { HV_PROFILE_TILE_PARTS = 64 };
const char *hv_rule_tile_part_count(uint64_t parts);

/* The value of one PLT entry (A.7.3): 7-bit groups, most significant
 * first. "plt.value-overflow" when it does not fit 64 bits. */
const char *hv_rule_iplt(const Iplt *entry, uint64_t *value);

/* The PLT entries of a codestream, counted in codestream order. */
typedef struct {
    uint64_t packets;       /* nonzero entries */
    int padding;            /* a zero entry has been seen */
} hv_plt_count;

/* Counts one entry. The profile accepts zero lengths as padding after the
 * last packet of the codestream (plt.padding-position for a nonzero entry
 * after a zero one). The standard ignores them here: the caller counts them
 * for hv_segments_data, which
 * accepts them only where the packet headers are packed (plt.zero-length:
 * with the headers in the bit stream a packet has at least one byte). The
 * reader's HV_READ_PADDED applies plt.padding-position, under the
 * same name, to each tile-part instead of the codestream (hv_reader.h). */
const char *hv_rule_plt_entry(hv_plt_count *count, uint64_t value, int profile);

/* The Zplt of the PLT segments of one tile-part header. The profile takes
 * them in segment order, 0, 1, 2, ... (plt.zplt-sequence, as the server's
 * ReadPLTMarker); the standard in any order, each once, with gaps allowed
 * (A.7.3: the entries are "concatenated, in order of increasing Zplt";
 * A.1.3: marker segments "can appear in any order in a given header").
 * A duplicate fails plt.zplt-index. Zero it before the header. */
typedef struct {
    uint8_t seen[32];       /* a bit per Zplt */
    unsigned count;         /* segments */
} hv_zplt;
const char *hv_rule_zplt(hv_zplt *z, uint64_t zplt, int profile);

/* Not a check: the number of packets (B.6, B.9) of a codestream with one
 * tile, zero origins and unit sampling, from its SIZ and main COD; 0 when
 * it exceeds INT32_MAX, which the server's signed JPIP state cannot hold. */
uint64_t hv_rule_packets(const hv_siz *siz, const Sgcod *sgcod, const Spcod *spcod);

/* PLT sum against packet data, allowing SOP bytes outside the lengths. */
const char *hv_rule_plt_coverage(uint64_t sum, uint64_t data_size, uint64_t sops);

/* The counted PLT entries against the result of hv_rule_packets (including
 * its zero overflow result): one entry per packet, and in the profile a
 * packet count the server can hold. */
const char *hv_rule_plt_packets(const hv_plt_count *count, uint64_t packets, int profile);

/* ------------------------------------------------------------------------
 * The codestream's segments across headers (listed at the end of
 * ../spec/j2k-headers.asn1): the bodies the framing keeps opaque (COC,
 * QCC, RGN, POC, TLM, PLM, PPM, PPT, CRG), which these functions decode,
 * QCD's Sqcd and SPqcd, where each segment may be, and what the coding
 * and quantization of each tile-component come to: the transform against
 * the quantization style, enough quantization entries for its decomposition,
 * and the multiple component transform, Profile 0
 * and Profile 1 (Rsiz 1 and 2, Table A.45), the tile-part lengths of TLM,
 * the packet lengths of PLM, the packet headers of PPM and PPT against
 * the tile-parts, and the SOP and EPH markers in the tile-part data. Layer
 * 1 only: the profile keeps QCC, RGN, TLM, PLM and CRG opaque, as the
 * server skips them, and excludes the others (j2k-codestream.asn1);
 * hv_segments_data applies the PLT rules of both layers at the end of a
 * tile-part header.
 *
 * The caller passes the events of one codestream in order: hv_segments_init
 * after SIZ; hv_segments_main for each main-header segment but SIZ; for
 * each tile-part, hv_segments_tile_part at SOT, hv_segments_tile for each
 * of its header segments, and hv_segments_data at SOD with the tile-part's
 * data; hv_segments_end at EOC. COD and QCD come decoded (`cod`, `qcd`),
 * every other segment as its body, the `n` bytes after Lxxx, which stay in
 * place until hv_segments_end. The checks do not allocate: the caller
 * provides one hv_component per component and one hv_tile_count per tile
 * (the counts hv_rule_tile_part keeps), both zeroed.
 * ------------------------------------------------------------------------ */

/* The coding of a component as a COD or COC sets it. */
typedef struct {
    uint8_t levels, transform, custom;
    uint8_t precincts[33];          /* PPy << 4 | PPx per resolution, when custom */
} hv_coding;

/* What the main header and the current tile's header set for one
 * component. The header numbers say where a COC, QCC or RGN for it was
 * last seen (hv_segments.header). */
typedef struct {
    hv_coding coc;                  /* the main header's COC, when main_coc */
    uint8_t main_coc, main_qcc;     /* the main header has a COC, a QCC */
    uint8_t qcc_style, qcc_coverage; /* quantization style and supported levels */
    uint8_t tile_transform, tile_levels, tile_qcc_style, tile_qcc_coverage;
    uint32_t coc_header, qcc_header, rgn_header;
    uint16_t next;                  /* hv_segments.touched: the next, + 1 */
} hv_component;

/* hv_tile_count.flags: what a tile's tile-parts have set. */
enum {
    HV_TILE_COD = 1, HV_TILE_SOP = 2, HV_TILE_EPH = 4,  /* its COD, and what it signals */
    HV_TILE_PPT = 8,                                     /* a PPT segment */
    HV_TILE_POC = 16                                     /* a POC in the first tile-part */
};

/* A PLM or PPM series, or TLM's entries, in the order of increasing Z. */
typedef struct {
    const uint8_t *body[256];       /* the segment of each Z, after Z */
    uint16_t size[256];
    unsigned end;                   /* one past the largest Z seen, 0 if none */
    unsigned z;                     /* the reading position: segment Z, */
    size_t pos;                     /* and the byte in it */
    uint64_t runs, used;            /* tile-parts it describes, and read */
} hv_series;

/* Main-header component counts, excluding tile overrides when copied by
 * tile_coding. Coverage is the largest decomposition supported by QCD/QCC;
 * scalar-derived quantization covers all 32 levels. */
typedef struct {
    uint32_t levels[33], coverage[33];
    uint32_t shortfall;
} hv_quant_counts;

typedef struct {
    const hv_siz *siz;
    hv_component *components;
    uint64_t grid;                  /* the tiles of the grid, however many */
    int wide;                       /* Csiz >= 257: 16-bit component indices */
    int profile;
    unsigned rsiz_profile;          /* 1 or 2 for Profile 0 or 1, else 0 */
    uint32_t header;                /* 1 for the main header, then one per tile-part */
    /* The main header. */
    hv_coding cod;                  /* its COD */
    uint8_t cod_sop, cod_eph, mct, qcd_style, qcd_coverage;
    int ppm, crg;
    hv_quant_counts quant;
    uint64_t pairs[2][2];           /* components by main transform and style class */
    /* The current tile-part. */
    hv_tile_count *tile;
    uint32_t isot;
    uint64_t tpsot, psot;
    int tile_cod, tile_qcd, tile_poc;
    hv_coding tile_coding;          /* its COD, in the first tile-part */
    uint8_t tile_qcd_style, tile_qcd_coverage, tile_mct;
    uint8_t ppt_seen[32];
    uint32_t touched;               /* components with a tile COC or QCC, + 1 */
    uint64_t parts;                 /* tile-parts before the current one */
    uint64_t first_parts;           /* of those, with TPsot 0 */
    int later_parts;                /* a tile-part with TPsot > 0 */
    int main_poc, tlm_ordered;      /* a main POC; a TLM with ST 0 */
    hv_series tlm, plm, ppm_series;
    uint8_t tlm_st[256], tlm_sp[256];
} hv_segments;

/* After SIZ, which has passed hv_rule_siz: `components` holds
 * siz->ncomponents (NULL with `profile`, where they are not used). */
void hv_segments_init(hv_segments *s, const hv_siz *siz, hv_component *components,
                      int profile);

/* A main-header segment: `code`, and COD and QCD decoded, else the body. */
const char *hv_segments_main(hv_segments *s, uint16_t code, const Cod *cod, const Qcd *qcd,
                             const uint8_t *body, size_t n);

/* A tile-part, at its SOT, after hv_rule_tile_part has counted it in
 * `tile`, the counts of its tile, which hold hv_tile_count.flags for these
 * rules: Isot, TPsot, and the length from SOT to the end of its data. */
const char *hv_segments_tile_part(hv_segments *s, hv_tile_count *tile, uint64_t isot,
                                  uint64_t tpsot, uint64_t length);

/* A segment of its header, as hv_segments_main. */
const char *hv_segments_tile(hv_segments *s, uint16_t code, const Cod *cod, const Qcd *qcd,
                             const uint8_t *body, size_t n);

/* The tile-part's data at SOD, and its PLT segments: how many, the sum of
 * their entries and how many of those are zero. */
const char *hv_segments_data(hv_segments *s, const uint8_t *data, size_t n, unsigned plts,
                             uint64_t plt_sum, uint64_t plt_zeros);

/* At EOC. */
const char *hv_segments_end(hv_segments *s);

/* The value of the PLT or PLM entry at body + *pos, before end, and *pos
 * past it: NULL; "decode" when it is not an entry, or plt.value-overflow.
 * The standard layer takes any number of leading zero groups (0x80, Table
 * A.36 sets no limit), the profile at most ten bytes, the model's Iplt. */
const char *hv_rule_packet_length(const uint8_t *body, size_t *pos, size_t end, int profile,
                                  uint64_t *value);

/* ------------------------------------------------------------------------
 * Boxes, T.800 I.4
 * ------------------------------------------------------------------------ */

typedef struct {
    uint32_t type;          /* TBox */
    size_t start;           /* offset of LBox */
    size_t payload;         /* first payload byte */
    size_t end;             /* just past the box */
    int to_end;             /* LBox = 0: the box runs to the end of the file */
} hv_box;

/* The boxes of one container: the whole file, or a superbox's payload. */
typedef struct {
    const uint8_t *buf;
    size_t pos, end;
    int to_end;             /* the container runs to the end of the file */
} hv_boxes;

void hv_boxes_file(hv_boxes *it, const uint8_t *buf, size_t size);
void hv_boxes_children(hv_boxes *it, const uint8_t *buf, const hv_box *parent);

/* 1: *box is the next box. 0: no more boxes. -1: invalid; *error says why
 * and *at is the box's offset. *error and *at are written only then. */
int hv_boxes_next(hv_boxes *it, hv_box *box, const char **error, size_t *at);

/* Nonzero for the box types T.800 and T.801 define as superboxes. */
int hv_is_superbox(uint32_t type);

/* The deepest superbox nesting a tool descends into (the top level is 1),
 * so that walking the boxes recursively cannot exhaust the stack. The
 * standard sets no limit; files for the server nest at most 2 deep. */
enum { HV_BOX_DEPTH_MAX = 32 };

/* ------------------------------------------------------------------------
 * The file: its File Type box, and the box tree (listed at the end of
 * ../spec/jp2-boxes.asn1)
 * ------------------------------------------------------------------------ */

/* A File Type box (T.800 I.5.2, T.801 M.8), decoded as FtypHeader and
 * Brands, and which of the brands esajpip knows its compatibility list
 * holds. */
typedef struct {
    uint64_t brand, minor;
    int jp2, jpx, jpxb;         /* 'jp2 ', 'jpx ', 'jpxb' in the list */
} hv_ftyp;

/* A File Type box, that decodes as FtypHeader, then one or more Brands
 * filling it: 0 and *ftyp; otherwise -1. */
int hv_ftyp_decode(const uint8_t *buf, const hv_box *box, hv_ftyp *ftyp);

/* The File Type box of a file, where the file starts with a box and its
 * second box is one: as hv_ftyp_decode; -1 where it is not. */
int hv_ftyp_read(const uint8_t *buf, size_t size, hv_ftyp *ftyp);

/* Whether the standard layer reads a file as a JPX file: its brand is
 * 'jpx ' (T.801 M.11.7.2 tells a JP2 file and a JPX file apart by BR, "a
 * file with 'jpx\040' in the BR field"). Any other file is read as a JP2
 * file, whose rules then require 'jp2 ' in the compatibility list. */
int hv_ftyp_is_jpx(const hv_ftyp *ftyp);

/* The File Type box's brand and compatibility list for the file's kind:
 * JP2 (T.800 I.5.2), 'jp2 ' in the list, whatever the brand ("If the value
 * of the Brand field is not 'jp2\040', then a value of 'jp2\040' in the
 * Compatibility list indicates that a JP2 reader can interpret the file");
 * JPX (T.801 M.8), the brand 'jpx ' and 'jpx ' in the list. The profile
 * (the server's ReadFileType) requires the brand of the file's extension
 * in both: file.ftyp-brand, file.ftyp-compatibility. */
const char *hv_rule_ftyp(const hv_ftyp *ftyp, int jpx, int profile);

/* MinV: 0 in a JP2 file (T.800 I.5.2: "The value of this field shall be
 * zero", although readers parse the file whatever it is), 1 in a JPX
 * file (T.801 M.8); file.ftyp-minor. Standard layer only: esajpip ignores
 * MinV. */
const char *hv_rule_ftyp_minor(uint64_t minv, int jpx);

/* The payload extent of a box. */
typedef struct {
    size_t start, end;
} hv_extent;

/* The box tree of a JP2 or (jpx) JPX file at the standard layer: every
 * box at every level the file's kind defines superboxes for, to
 * HV_BOX_DEPTH_MAX. A deeper superbox returns box.depth-limit at its
 * header: validation is incomplete, not a standard conformance verdict.
 * The top-level superbox is level 1; children of the last level are checked.
 * A JP2 file (T.800) has three: jp2h, res and uinf, and
 * every other box is opaque to it (I.8). A JPX file (T.801) adds ftbl,
 * jpch, jplh, cgrp, comp, asoc, drep, j2cx, jclx and grp, and the url
 * boxes of a dtbl, after NDR. Pass `mdat` room for mdat_cap Media Data
 * boxes (or NULL); the walk records the payload of each, in file order,
 * as far as there is room, for hv_rule_fragment_here, and counts them all.
 *
 * The rules, of both kinds (T.800):
 *   file.one-signature, file.one-ftyp: jP and ftyp only as the first and
 *     the second box (I.5.1, I.5.2: "one and only one").
 *   box.nested-top-level: jp2h (I.5.3) and uinf (I.7.3) only at the top
 *     level.
 *   box.top-level: bpcc, pclr, cmap, cdef and res, and in a JP2 file colr,
 *     not at the top level (I.2.2: Figure I.1 "specifies ... the
 *     containment relationship between the boxes"); an ihdr may be
 *     ("Instances of this box in other places in the file shall be
 *     ignored", I.5.3.1), as may the boxes T.801 defines (nlst, lbl, ...),
 *     for which it states no such rule.
 *   JP2: box.nested-jp2c, jp2c only at the top level (Figure I.1);
 *     box.url-placement, url only in uinf (Figure I.1); box.containment,
 *     bpcc, colr, pclr, cmap, cdef and res only in jp2h, resc and resd only
 *     in res, ulst only in uinf (Figure I.1).
 *   uinf.contents: a uinf holds one ulst, then one url (I.7.3), besides
 *     boxes a reader skips (I.8).
 *   ulst.nu-count: a ulst holds NU UUIDs, filling it (I.7.3.1).
 *   uuid.id: a uuid box starts with a 16-byte UUID (I.7.2).
 *   url.version-flags, url.terminator, box.extent: a url in a uinf, and
 *     in a JPX file in a dtbl (M.11.2), has VERS and FLAG 0, and LOC,
 *     NUL-terminated, ends the box (I.7.3.2).
 * The rules of a JPX file (T.801), besides:
 *   box.nested-top-level: also dtbl (M.11.2), rreq (M.11.1), comp
 *     (M.11.10) and jclx (M.11.21); jpch and jplh except as the children
 *     of a jclx (M.11.6, M.11.7); ftbl and j2cx except as the children of
 *     a j2cx (M.11.3, M.11.23). jp2c below the top level but in a j2cx
 *     (M.11.8, M.11.23) is jpch.nested-jp2c in a jpch, box.nested-jp2c
 *     elsewhere (hv_rule_child's names).
 *   box.flst-placement: flst only in ftbl; box.url-placement: url only in
 *     dtbl or uinf; dtbl.non-url: only url in dtbl (hv_rule_child).
 *   cgrp.placement: cgrp only in jplh (M.11.7.1); colr.placement: colr
 *     only in jp2h or cgrp (M.11.7.2: "Colour Specification boxes may be
 *     found in either the JP2 Header box or in Colour Group boxes").
 *   opct.placement, opct.once, opct.cdef: opct only in jplh, at most one
 *     in each, and not with a cdef (M.11.7, M.11.7.6).
 *   creg.placement, creg.once: creg only in jplh, at most one in each
 *     (M.11.7.7).
 *   pxfm.once: at most one pxfm in each jp2h or jplh (M.11.7.8).
 *   cref.placement: cref only in jpch, jplh or asoc (M.11.4).
 *   comp.once, comp.copt-first, copt.placement: at most one comp, whose
 *     first box is copt, and copt nowhere else (M.11.10, M.11.10.1).
 *   inst.placement: inst only in comp or jclx (M.11.10.2).
 *   drep.once, gtso.placement, gtso.once: at most one drep in the file,
 *     gtso only in drep and at most one (M.11.15, M.11.15.1).
 *   asoc.children: an asoc holds two or more boxes (M.11.11).
 *   ftbl.one-flst: an ftbl holds one flst (M.11.3); dtbl.ndr-count: a
 *     dtbl starts with NDR and holds that many url boxes.
 *   lbl.characters: a lbl is UTF-8 without U+0000 to U+001F, U+007F to
 *     U+009F, '/', ';', '?', ':' and '#' (M.11.13).
 *   jpx.colr: a colr in jp2h or in a cgrp (M.11.7.2: "all JPX files shall
 *     contain at least one Colour Specification box"; M.11.7.1: with no
 *     colr in jp2h, a cgrp).
 *   j2cx.order: every top-level jp2c and ftbl before any j2cx (M.11.23).
 *   j2ci.placement, j2ci.length, j2cx.contents, j2ci.ncs, j2ci.ltbl: a
 *     j2cx holds a j2ci first, of 8 bytes (and a j2ci is nowhere else),
 *     then jp2c, ftbl and j2cx boxes, then mdat and free boxes; Ncs is
 *     the number of its codestreams, not 0; and a nonzero Ltbl, R * 2^26 +
 *     L, gives the length L and the 2^R codestreams of each of those
 *     sub-boxes but the last (M.11.23, M.11.24).
 *   j2cx.top-codestreams: with a j2cx, a top-level jp2c or ftbl, and
 *     without creg boxes one for each top-level jplh (M.11.23).
 *   jclx.order, jclx.headers: every top-level jpch and jplh, and comp,
 *     before any jclx, and with a jclx at least one of each (M.11.6,
 *     M.11.7, M.11.21).
 *   jlxi.placement, jlxi.length, jclx.contents, jlxi.counts,
 *     jlxi.repetition, jlxi.frames: a jclx holds a jlxi first, of 20 bytes,
 *     or 24 with LIFE-START when T is not 0 (and a jlxi is nowhere else),
 *     then its jpch, then one or more compositing groups, each jplh boxes
 *     then inst boxes, which only the last group may lack, then other
 *     boxes; C, L and T are its jpch, its jplh and its groups with inst; M
 *     is 0 only with C not 0, and M, and F with T not 0, only in the last
 *     jclx (M.11.21, M.11.22).
 *   grp.placement: a grp not the first box of an asoc (M.11.25).
 * box.framing: a box inside a superbox that is not framed as
 * hv_boxes_next requires. NULL, or the rule and *at the box at fault (the
 * end of the file for jpx.colr, the first jclx for jclx.headers). */
typedef struct {
    hv_extent *mdat;            /* in: room for mdat_cap extents, or NULL */
    size_t mdat_cap;
    size_t mdat_count;          /* out: the mdat boxes */
    uint64_t ndr;               /* out: validated number of data references */
    int ipr;                    /* jp2i boxes, and cref boxes for one (Rtyp) */
    int jp2h, jp2c;             /* at the top level */
    int jp2h_late;              /* a top-level jp2h after a top-level jp2c */
    int jp2h_late_baseline;     /* ... after a jp2c, ftbl, mdat, jpch or jplh,
                                 * at any depth */
    int j2cx, jclx;             /* at the top level */
} hv_box_tree;

const char *hv_rule_box_tree(const uint8_t *buf, size_t size, int jpx, hv_box_tree *tree,
                             size_t *at);

/* The box tree rules of a JPX file for `box` of buf, as a child of a box
 * of type `parent` (0: at the top level), and for everything in it: for a
 * writer that copies a box from one file into another. The rules on the
 * whole file (file.*, comp.once, drep.once, gtso.once, jpx.colr) count
 * only what `box` holds. On success, *ipr is the number of IPR boxes
 * in the copied tree. NULL, or the rule and *at the box at fault. */
const char *hv_rule_box_placed(const uint8_t *buf, const hv_box *box, uint32_t parent,
                               int *ipr, size_t *at);

/* The fragments of the codestreams of a JPX file (the flst of each ftbl,
 * at the top level or in a j2cx), after hv_rule_box_tree has successfully
 * walked the file into `tree`: exactly NF complete tuples (flst.nf-count),
 * each decoded against the model (decode), DR <= NDR (flst.dr-range),
 * and each in this file (DR 0) as hv_rule_fragment_here
 * requires, and in a baseline file (jpxb), those of the first codestream
 * as hv_rule_jpxb_fragment. NULL, or the rule and *at the flst box. */
const char *hv_rule_fragments(const uint8_t *buf, size_t size, const hv_box_tree *tree,
                              int jpxb, size_t *at);

/* A fragment of a codestream in this file (DR 0) in a JPX file: within
 * the payload of one mdat box (flst.mdat; T.801 M.4: "If the codestream is
 * contained within the JPX file in multiple fragments, then the codestream
 * shall be encapsulated within one or more Media Data boxes", so that a
 * fragment is never in a jp2c either); the first of its list at the
 * codestream's first byte, SOC (flst.codestream-start; M.11.3.1: "the
 * first offset in the fragment list shall point directly to the first
 * byte codestream data"). mdat: hv_box_tree's extents, n of them, in file
 * order. */
const char *hv_rule_fragment_here(const hv_extent *mdat, size_t n, const uint8_t *buf,
                                  uint64_t off, uint64_t len, int first);

/* ------------------------------------------------------------------------
 * JPX boxes, T.801 Annex M (listed at the end of ../spec/jp2-boxes.asn1)
 * ------------------------------------------------------------------------ */

/* A box inside a top-level superbox that the server walks (jpch, ftbl and
 * dtbl; at the standard layer, hv_rule_box_tree names the others): jp2c,
 * jpch, ftbl and dtbl only at the top level (M.11.2, M.11.3, M.11.6,
 * M.11.8; box.nested-top-level, box.nested-jp2c, jpch.nested-jp2c), flst
 * only in ftbl, url only in dtbl, and a dtbl holds url boxes only. */
const char *hv_rule_child(uint32_t parent, uint32_t child);

/* A top-level flst or url, which ReadJPX rejects (profile layer:
 * box.flst-placement, box.url-placement). The whole-file model has no
 * top-level alternative for them: reason `decode` at both layers. */
const char *hv_rule_top_box(uint32_t type);

/* The only URL scheme the profile links with, and its length. */
#define HV_FILE_SCHEME "file://"
enum { HV_FILE_SCHEME_LENGTH = sizeof HV_FILE_SCHEME - 1 };

/* The profile's Data Entry URL box (ReadUrlBox, ReadJPX): VERS and FLAG 0,
 * and LOC, `n` bytes with its NUL, a file:// URL naming a .jp2 file whose
 * path hv_url_path decodes (url.percent-encoding). */
const char *hv_rule_url(uint64_t vers, uint64_t flag, const uint8_t *loc, size_t n);

/* The path of a file:// URL: the `n` characters of LOC after the scheme,
 * percent-decoded as the server decodes them (g_uri_unescape_string): each
 * '%' starts an escape of two hex digits, and no escape gives NUL. When
 * out is not NULL, the path is written there, NUL-terminated. 0; -1 when
 * an escape is invalid; -2 when out (out_size bytes) is too small. */
int hv_url_path(const uint8_t *path, size_t n, char *out, size_t out_size);

/* Check the file scheme, then validate/decode its path in one pass. With
 * out == NULL, validate only; otherwise preserve bounded decoding errors. */
const char *hv_file_url_path(const uint8_t *loc, size_t n, char *out, size_t out_size);

/* The profile's Fragment List box: one fragment (ReadFlstBox). */
const char *hv_rule_flst(uint64_t nf, int fragments);

/* A fragment's data reference: 0 (this file) or at most NDR; the profile
 * links to other files only. */
const char *hv_rule_fragment_dr(uint64_t dr, uint64_t ndr, int profile);

/* The top-level boxes of a JPX file, counted. */
typedef struct {
    int jp2c, jpch, ftbl, dtbl, rreq;
    int rreq_third;         /* the third box is rreq */
    int extensions;         /* a j2cx or jclx box: codestreams or headers
                             * the top-level count does not see */
} hv_jpx_boxes;

/* The file rules on those counts: at most one dtbl, a codestream per jpch
 * where there is any and no j2cx or jclx box (whose codestreams and
 * headers M.11.6 counts too), and rreq, once and third (standard only); in
 * the profile, at least one jpch, embedded or linked codestreams but not
 * both, and a dtbl where they are linked. */
const char *hv_rule_jpx(const hv_jpx_boxes *boxes, int profile);

/* A standard feature (SF) of the Reader Requirements box: not one of those
 * Table M.14 lists as deprecated (T.801 M.11.1: "Writers shall not include
 * these features"); rreq.deprecated-feature. */
const char *hv_rule_rreq_feature(uint64_t sf);

/* ------------------------------------------------------------------------
 * JP2 header boxes, T.800 I.5.3 and T.801 M.11.5 to M.11.7 (listed at the
 * end of ../spec/jp2-boxes.asn1). Standard layer only: the server reads
 * none of them.
 * ------------------------------------------------------------------------ */

/* Where the JP2 Header box is, as hv_rule_box_tree counts the top-level
 * boxes. JP2 (T.800 I.2, I.5.3, I.5.4): one or more codestreams
 * (jp2.one-codestream, the name the profile's "exactly one" rule has too),
 * exactly one jp2h (jp2.one-jp2h; T.800 I.5.3: "one and only one"), before
 * the first codestream (jp2h.position). JPX (T.801 M.11.5): at most one
 * jp2h (jpx.one-jp2h: T.801 M.11.5 lets a JPX file omit the JP2 Header
 * box, T.800 I.5.3 allows only one), anywhere at the top level, but in a
 * baseline file (jpxb in the compatibility list) before the first jp2c,
 * ftbl, mdat, jpch and jplh, at any depth (jp2h.position; M.9.2.7). */
const char *hv_rule_jp2h_place(const hv_box_tree *tree, int jpx, int jpxb);

/* The contents of one header box: a JP2 Header box (jp2h), or in a JPX
 * file a Codestream Header box (jpch) or Compositing Layer Header box
 * (jplh). Start with hv_header_init, then pass each child box in order to
 * hv_rule_header_child and its contents to the rule for its type; for a
 * Resolution box, pass its children to hv_rule_res_child and end it with
 * hv_rule_res_end; for a Colour Group box, pass its children to
 * hv_rule_cgrp_child and each colr to hv_rule_colr on a second hv_header
 * (hv_header_init with HV_BOX_CGRP), and end it with hv_rule_cgrp_end,
 * which records the group in the header box's. The first box of each type
 * is recorded (the rules require at most one of each but colr, and of
 * ihdr only in jpch): its values, and of the bpcc, pclr, cmap and cdef
 * boxes where their entries are, which are not copied and must stay in
 * place until hv_rule_codestream_header has read h. */
typedef struct {
    uint32_t parent;            /* HV_BOX_JP2H, HV_BOX_JPCH, HV_BOX_JPLH, or
                                 * HV_BOX_CGRP for a colour group's colr boxes */
    int jpx;                    /* in a JPX file: T.801's box structures */
    int jp2;                    /* a JP2 file's jp2h: in a JP2 file, or in a JPX
                                 * file that lists 'jp2 ' (T.801 M.3.6) */
    int children;
    int ihdr, bpcc, colr, pclr, cmap, cdef, res;   /* boxes of each type */
    int colr_ended;             /* a box followed a colr box */
    int meth1, meth2;           /* colr boxes with METH 1, METH 2 */
    unsigned colours;           /* the first colr's colours (Table I.18), or 0 */
    int rgb;                    /* the first colr's colourspace is RGB */
    int baseline_colr;          /* a colr of a method M.9.2.4 lists */
    int approx_colr;            /* a colr with APPROX 1 to 3 (M.9.2.4) */
    int cgrp;                   /* cgrp children (a jplh) */
    int cgrp_colr, cgrp_baseline, cgrp_approx;     /* their colr boxes, as above */
    Ihdr image;                 /* the first ihdr */
    const uint8_t *bpcc_depths; /* the entries of the first bpcc, one byte
                                 * each as in the box */
    size_t bpcc_count;          /* how many */
    uint32_t npc;               /* palette columns of the first pclr */
    uint8_t pclr_depths[255];   /* and their BitDepths */
    uint64_t pclr_ne;           /* the pclr being read: NE, */
    uint32_t pclr_columns;      /* its columns so far, */
    uint8_t pclr_column_depths[255];   /* and their BitDepths */
    const uint8_t *cmap_entries;       /* the entries of the first cmap, as
                                        * in the box (CmapEntry) */
    uint32_t cmap_count;        /* how many */
    uint32_t cmap_cmp;          /* its largest CMP */
    int cmap_palette;           /* it has an entry with MTYP 1 */
    uint32_t cmap_pcol;         /* the largest PCOL of those entries */
    const uint8_t *cdef_entries;       /* the entries of the first cdef, as
                                        * in the box (CdefEntry) */
    size_t cdef_count;          /* how many */
    int opacity, premultiplied; /* first cdef has Typ 1 or Typ 2 */
    uint32_t cdef_cn;           /* their largest Cn */
    int resc, resd;             /* children of the current res box */
    int ipr, creg;              /* jp2i and creg children */
} hv_header;

/* h->jp2 is set for a JP2 file (!jpx); set it after for the jp2h of a
 * JPX file that lists 'jp2 '. */
void hv_header_init(hv_header *h, uint32_t parent, int jpx);

/* The next child box: the box order and counts. jp2h starts with ihdr and
 * holds its colr boxes contiguously (T.800 I.5.3); a header box holds at
 * most one bpcc, pclr, cmap, cdef and res, and a jpch at most one ihdr (in
 * jp2h and jplh a later ihdr is accepted and ignored). */
const char *hv_rule_header_child(hv_header *h, uint32_t type);

/* The contents of each child type. Each checks what the box alone
 * determines and records what hv_rule_codestream_header needs. The decoded
 * values must satisfy their types' constraints (Ihdr_IsConstraintValid and
 * so on). */
/* C (ihdr.c): 7 in a JP2 file (T.800 I.5.3.1, JPEG 2000), a value of
 * T.801 Table M.19 (0 to 12) in a JPX file. */
const char *hv_rule_ihdr(hv_header *h, const Ihdr *ihdr);
/* The bytes a box holds after its fields (`extra` of Ihdr, Cdef,
 * Resolution and Rreq): none. type is the box's, as HV_BOX_IHDR; the rule
 * is "<box>.extent" (ihdr: "the length of the Image Header box shall be
 * 22 bytes", I.5.3.1). */
const char *hv_rule_extent(uint32_t type, uint64_t extra);
/* The n entries of a bpcc box, one byte each as in the box, each a
 * BitDepth (the caller decodes them one at a time). For the first bpcc, h
 * keeps the pointer. */
void hv_rule_bpcc(hv_header *h, const uint8_t *depths, size_t n);
/* METH, APPROX, EnumCS, and `rest`, the n bytes after them: none when
 * METH is 1, but in a JPX file the EP parameters of CIELab and CIEJab
 * (T.801 M.11.7.4) (colr.enumcs-length); when METH is 2, a restricted ICC
 * profile (T.800 I.5.3.3, Table I.9; T.801 M.11.7.2): an ICC header whose
 * size is n and signature 'acsp' (colr.icc-header), of the input class
 * 'scnr' with the colourspace 'GRAY' (Monochrome Input) or 'RGB '
 * (Three-Component Matrix-Based Input) (colr.icc-class), with the PCS
 * 'XYZ ' (colr.icc-pcs), and the tags ICC.1:1998-09 requires of the class
 * in its tag table, each within the profile (colr.icc-tags). In a JP2
 * file, PREC and APPROX 0 (colr.prec-approx; I.5.3.3, Table I.11), and
 * the first colr's METH 1 or 2 (colr.method), and EnumCS 16, 17
 * or 18 if it is 1 (colr.enumcs) (I.5.3.3: a reader ignores a colr of
 * another METH, and "Valid EnumCS values for the first colourspace
 * specification box in conforming files are limited to 16, 17, and 18");
 * in a JPX file, APPROX 1 to 4 (M.11.7.2), and no two enumerated or two
 * restricted ICC colr in jp2h or in a colour group (M.11.7.1). */
const char *hv_rule_colr(hv_header *h, const ColrHeader *colr, const uint8_t *rest,
                         size_t n);
/* A pclr box in order: NE and NPC, the BitDepth of each of the NPC
 * columns, then `entries`, the n bytes of the table: NE x NPC values, each
 * padded with zero bits to whole bytes (pclr.entries-length,
 * pclr.padding; I.5.3.4). */
void hv_rule_pclr(hv_header *h, uint64_t ne, uint64_t npc);
void hv_rule_pclr_column(hv_header *h, uint64_t depth);
const char *hv_rule_pclr_end(hv_header *h, const uint8_t *entries, uint64_t n);
/* One cmap entry, in order: PCOL 0 when MTYP is 0. Then the entries of the
 * box, `n` of them as in the box, for h to keep. */
const char *hv_rule_cmap_entry(hv_header *h, const CmapEntry *entry);
void hv_rule_cmap_end(hv_header *h, const uint8_t *entries, size_t n);
/* The n cdef entries, decoded, and as in the box (`raw`, which h keeps for
 * the first cdef). T.800 I.5.3.6: no two channels with the same Typ and
 * Asoc, except where either is 65,535 (cdef.pairs; the box "may specify
 * multiple descriptions for a single channel"); at most one opacity or
 * premultiplied opacity channel for each colour, an opacity channel for
 * the whole image (Asoc 0) standing for every colour (cdef.opacity).
 * These restrictions apply only to headers used by a JP2 reader (h->jp2);
 * T.801 M.11.7.5 relaxes them for JPX-only headers.
 * Takes any n without allocating: up to HV_CDEF_PAIRWISE entries are
 * compared pairwise, more are sorted in `scratch`, room for n values, which
 * may be NULL for fewer. */
enum { HV_CDEF_PAIRWISE = 64 };
const char *hv_rule_cdef(hv_header *h, const CdefEntry *entries, size_t n,
                         const uint8_t *raw, uint64_t *scratch);
/* A child of a res box, then its end: resc, resd or both (I.5.3.7). */
void hv_rule_res_child(hv_header *h, uint32_t type);
const char *hv_rule_res_end(hv_header *h);
/* A child of a cgrp box in a JPX file, then its end: at least one colr
 * (T.801 M.11.7.1: a set of equivalent colour specifications); other
 * boxes are ignored, as in res (T.800 I.8). hv_rule_cgrp_end records the
 * group in h, the header box that holds it. */
void hv_rule_cgrp_child(hv_header *g, uint32_t type);
const char *hv_rule_cgrp_end(hv_header *h, const hv_header *g);

/* The header of one codestream of a JP2 or (jpx) JPX file: its own header
 * box h (NULL for none) over the JP2 Header box's defaults (NULL for none),
 * SIZ, the codestream's (NULL when the codestream is not at hand: its
 * checks are skipped), and `mct`, its main COD's multiple component
 * transform (0 or 1; -1 when not at hand).
 *   JP2 (T.800 I.5.3), h the jp2h and no defaults: at least one colr; bpcc
 *     exactly when BPC is 255; pclr exactly with cmap.
 *   JPX (T.801 M.11.6), h the jpch and defaults the jp2h: an ihdr in
 *     either; no IPR box in the jpch when that ihdr's IPR is 0; bpcc in
 *     the ihdr's own box when BPC is 255 (M.11.5.1: "the superbox that
 *     contains this Image Header box ... shall contain a Bits Per
 *     Component box"); pclr needs cmap, and an MTYP 1 cmap entry needs
 *     pclr.
 *   Both: bpcc has NC entries; cmap maps components below NC and palette
 *     columns below NPC.
 *   JP2, the channels (I.5.3.6): cdef describes channels there are
 *     (cdef.channel); when present, cdef covers every channel and colour
 *     (cdef.complete); an opacity channel comes from an unsigned component
 *     or palette column (cdef.opacity-signed); Asoc 0, 65,535 or a colour
 *     of the colourspace, where the first colr gives one (cdef.asoc; Table
 *     I.18: RGB and YCbCr 3, greyscale 1); with the codestream's multiple
 *     component transform, an RGB colourspace and R, G and B as channels
 *     0, 1 and 2 (jp2.mct-colourspace).
 *   Against SIZ (T.800 I.5.3.1, I.5.3.2), where the codestream is at hand:
 *     JP2, a T.800 codestream (jp2.rsiz); C 7 (ihdr.c); HEIGHT = Ysiz -
 *     YOsiz (ihdr.height), WIDTH = Xsiz - XOsiz (ihdr.width), NC = Csiz
 *     (ihdr.nc), BPC the components' common Ssiz or 255 when they differ
 *     (ihdr.bpc), each bpcc entry the component's Ssiz (bpcc.depth) --
 *     the last two not in a JPX file whose codestream signals the Part 2
 *     multiple component or non-linear point transformation (T.801 Table
 *     A.2), after which the depths are those of the transformed
 *     components (M.11.5.1, NOTE). */
const char *hv_rule_codestream_header(const hv_header *h, const hv_header *defaults,
                                      const hv_siz *siz, int mct, int jpx);

/* A JP2 file's IPR (T.800 I.5.3.1): the jp2h's ihdr has IPR 1 exactly
 * when the file contains an IPR box, of which hv_rule_box_tree found
 * ipr_boxes (ihdr.ipr; I.6: IPR boxes "may be found at other locations"
 * than the top level). */
const char *hv_rule_ihdr_ipr(const hv_header *jp2h, int ipr_boxes);

/* A JPX codestream's rights information (T.801 M.11.5.1): when the ihdr
 * that describes it (its jpch's, h, NULL for none, else the jp2h's,
 * defaults) has IPR 1, the file holds an IPR box, or a cref for one, of
 * which hv_rule_box_tree found ipr_boxes (jpx.ipr). */
const char *hv_rule_jpx_ipr(const hv_header *h, const hv_header *defaults, int ipr_boxes);

/* A JPX file's Compositing Layer Header boxes (T.801 M.11.7): if one holds
 * a Codestream Registration box, every one does (jpx.creg). */
const char *hv_rule_jpx_creg(int jplh, int with_creg);

/* A baseline JPX file's first compositing layer (T.801 M.9.2, with 'jpxb'
 * in the compatibility list): its colour specifications (those of the
 * first jplh's cgrp, else jp2h's) include one of a method M.9.2.4 lists
 * (EnumCS sRGB, sRGB-grey, ROMM-RGB, sYCC, e-sRGB, e-sYCC, CIELab or
 * CIEJab, or the Restricted or Any ICC method) and one with APPROX 3 or
 * less (jpxb.colour); the first jpch and the first jplh hold no header box
 * of a type jp2h holds (jpxb.header-override; M.9.2.7). first_jpch and
 * first_jplh may be NULL. */
const char *hv_rule_jpxb_layer(const hv_header *jp2h, const hv_header *first_jpch,
                               const hv_header *first_jplh);

/* A fragment of a baseline JPX file's first codestream (M.9.2.5): in this
 * file (DR 0), and after the one before it, whose end *end holds (0 before
 * the first); jpxb.fragments. */
const char *hv_rule_jpxb_fragment(uint64_t *end, uint64_t dr, uint64_t off, uint64_t len);

#ifdef __cplusplus
}
#endif

#endif
