/* transcode.h: rewrites a JPEG 2000 codestream in RPCL order with the given
 * precincts and PLT markers, without recompressing it, as
 *   kdu_transcode Corder=RPCL ORGgen_plt=yes Cprecincts={W,H}
 * does. A port of hvJP2K's jp2_precincts.transcode_codestream.
 *
 * Every code-block keeps its coding passes, bytes and zero bit-planes; only
 * Tier-2 changes. That is possible while the new precincts leave the
 * code-block partition unchanged.
 *
 * Supported input: one tile (any number of tile-parts), any progression
 * order, no COC, POC or PPM, no tile-part header markers other than PLT
 * and COM, and code-block styles without selective arithmetic coding bypass
 * or termination on each coding pass. SOP and EPH markers are checked
 * (A.8) and not written; input PLT bodies remain opaque because their lengths are not
 * used. Of the main header, QCD, QCC, RGN, CRG and COM are kept, TLM and
 * PLM dropped, and any other marker rejected: 0xFF30 to 0xFF3F and codes
 * T.800 does not define too. Of the tile-part headers, PLT and COM are
 * dropped with the headers, and any other marker rejected. main_marker and
 * tile_marker in transcode.c decide this, for both functions below. */
#ifndef HV_TRANSCODE_H
#define HV_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>

#include "jpeg2000/hv_profile.h"
#include "jpeg2000/hv_writer.h"

/* Transcodes the codestream in buf[start, end) and appends the result to
 * out. Precinct width and height exponents: 1 to 15 (2 to 32,768 samples).
 * HV_OUTPUT_JPIP requires serving geometry and main-header marker restrictions.
 * HV_OUTPUT_JPEG2000 also supports nonzero origins and subsampling; its
 * writer retains the standard validator's Rsiz declaration support limit.
 * Input markers that are copied
 * or dropped have their framing checked, with their bodies left opaque.
 * Packet headers are checked by the Tier-2 reader, independently of PLT.
 * 0, or -1 with a message in error; on failure out is as it was (hv_out_rewind), and
 * fails at once if out->error is set. */
int hv_transcode_codestream(const uint8_t *buf, size_t start, size_t end, int ppx, int ppy,
                            hv_output_profile profile, hv_out *out, char *error, size_t error_size);

/* Transcodes one JP2 file and appends the result to out, with the same output
 * profile as the raw operation. Reads the container and image headers it keeps;
 * HV_OUTPUT_JPIP additionally requires serving container/geometry restrictions.
 * Both outputs retain code-block bytes, emit RPCL/PLT and omit SOP/EPH.
 * JPIP output passes the serving checks. Output is limited to INT_MAX bytes.
 * The boxes are
 * kept in order: the codestream box is transcoded, a top-level XML box ends
 * before its first NUL (XML has none; old Kakadu wrote one at the end),
 * other boxes are copied in order. JP2 superboxes are checked by the shared
 * reader; JPX-only extension bodies remain opaque in this JP2. MinV, PREC
 * and APPROX are ignored on input and emitted as zero (I.5.2, I.5.3.3).
 * 0, or -1 with a message in error; on failure out is as it was
 * (hv_out_rewind), and fails at once if out->error is set. */
int hv_transcode_file(const uint8_t *buf, size_t size, int ppx, int ppy, hv_output_profile profile, hv_out *out,
                      char *error, size_t error_size);

#endif
