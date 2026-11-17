/* merge.h: merges JP2 files into a JPX file for the JPIP server, as
 * hvJP2K's hv_jpx_merge does: a JP2 Header box from the first input, then
 * per input a Codestream Header and a Compositing Layer Header box holding
 * what differs from it, the codestream, embedded (jp2c) or linked (ftbl
 * and a url in a final dtbl), and the first XML box, associated with the
 * codestream (asoc, nlst). A port of hvJP2K's jpx_merge.
 *
 * Every input must have one bounded codestream (hv_read_jp2), exactly
 * one JP2 header before it, and pass selected header/image checks
 * (hv_read_jp2h_boxes). Linked or validated inputs additionally pass
 * the served container checks (hv_served_jp2), as do transcoded inputs.
 * The default leaves the remaining codestream opaque. With HV_OUTPUT_JPIP,
 * serving validation checks the complete codestream too, including PLT.
 * Copied-box placement and output header requirements are checked in both
 * modes. What a JP2 file may hold and the JPX file may not is rejected too:
 * in the
 * first input more than one colr box with the same METH (colr.one-method,
 * as the JPX file's jp2h), and in a later input no cdef or res box where
 * the first has one (its jplh would inherit the first's). Limits: at most
 * 16 colr boxes per input, 16,777,215 inputs, 65,535 when linked; INT_MAX
 * bytes per input and INT_MAX output bytes. Rsiz
 * must be 0, 1 or 2 for the Reader Requirements this merger writes. */
#ifndef HV_MERGE_H
#define HV_MERGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "jpeg2000/hv_profile.h"

/* One immutable input: its bytes and size must remain unchanged throughout
 * hv_merge_files or hv_merge_buffers.
 * Mutation is a configuration/operational error outside this contract.
 * With links, buf must hold the file at path, and that file must remain
 * unchanged for as long as the linked JPX is used. */
typedef struct {
    const char *path;       /* for messages, and for links */
    const uint8_t *buf;     /* the whole file */
    size_t size;
} hv_merge_input;

/* The inputs, each opened exactly once: open fills *in for input i, whose
 * path stays valid until hv_merge_files returns and whose bytes stay valid
 * until close; 0, or -1 with a message in error. An unsuccessful open must
 * release its own resources. The first input stays open for header comparisons;
 * every later input is checked and emitted before close. At most two inputs
 * are open at a time. IPR spans are recorded during discovery; XML and IPR
 * payloads are copied directly from the mapping. */
typedef struct {
    int (*open)(void *context, size_t i, hv_merge_input *in, char *error, size_t error_size);
    void (*close)(void *context, size_t i, hv_merge_input *in);
    void *context;
} hv_merge_inputs;

/* Writes the JPX file merging n inputs to out, which must be seekable and
 * not opened in append mode. Their codestreams are embedded, or with `links`,
 * linked. With ppx = ppy = 0, HV_OUTPUT_JPEG2000
 * copies opaque codestreams; HV_OUTPUT_JPIP also requires serving-compatible
 * containers and codestreams. Otherwise ppx and ppy are precinct exponents
 * 1 to 15: every input is transcoded once to HV_OUTPUT_JPIP and embedded.
 * Transcoding cannot be combined with HV_OUTPUT_JPIP validation or links.
 * The original headers are compared without normalization.
 * Reader Requirements are patched in a 44-byte reservation followed by a
 * zero-filled free box, without changing the output byte count. Seek or flush
 * failures abort. 0, or -1 with a message in error; out may then hold part
 * of the file, including an unpatched reservation. Library callers must
 * discard partial output. The CLI removes its temporary file on failure. */
int hv_merge_files(const hv_merge_inputs *inputs, size_t n, int links, hv_output_profile profile,
                   int ppx, int ppy, FILE *out, char *error, size_t error_size);

/* The same, for inputs already in memory. */
int hv_merge_buffers(const hv_merge_input *inputs, size_t n, int links, hv_output_profile profile,
                     int ppx, int ppy, FILE *out, char *error, size_t error_size);

#endif
