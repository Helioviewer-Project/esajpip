/* hv_rewrite.h: writes a file again with hv_writer from what hv_reader
 * decodes, to check the writer against real files (hv_walk -w,
 * test/test_rewrite.c, ../spec/harness/writers.c).
 *
 * Every box is written with hv_begin_box and hv_end_box (XLBox where the
 * input has it), superboxes by their children, a codestream box by its
 * items: SIZ, COD, QCD, COM and PLT (hv_write_plt_segment, split as in
 * the input) from their decoded form, SOT with a measured Psot, other
 * segments from their body. The JPX boxes the writer
 * has functions for are written by them when the input is in their form
 * (a short box header, and no bytes the function could not write): ftyp,
 * flst with one fragment, url, a dtbl's NDR, rreq, and the entries of a
 * cmap. Everything else is copied.
 *
 * A file in the writer's form comes out byte for byte. Where the writer
 * writes the same content in its own form, the result says why the bytes
 * differ (uncomparable): LBox = 0 or Psot = 0 (written as lengths), zero
 * PLT entries (dropped), and PLT entries in more bytes than their values
 * need (written in the fewest). A tile-part header whose PLT segments are
 * not in the order of their Zplt (T.800 A.7.3 allows any) is refused: the
 * writer writes them in segment order, Zplt 0, 1, 2, ... */
#ifndef HV_REWRITE_H
#define HV_REWRITE_H

#include <stddef.h>
#include <stdint.h>

#include "hv_reader.h"
#include "hv_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *error;          /* the reader's or the writer's error, or NULL */
    size_t at;                  /* where, in the input */
    const char *uncomparable;   /* why the output differs from the input, or NULL */
} hv_rewrite_result;

/* Appends the rewrite of buf[0, size) to out: a raw codestream when `raw`,
 * otherwise a box structure (the whole file, as hv_boxes_file reads it).
 * Codestreams are read with hv_codestream_open and `mode`; packet and index modes
 * are refused because rewriting needs decoded bodies and lengths. 0, or -1 with
 * r->error set and out rewound to its original size. An earlier out->error
 * fails immediately and remains set. */
int hv_rewrite(const uint8_t *buf, size_t size, int raw, hv_read_mode mode, hv_out *out,
               hv_rewrite_result *r);

#ifdef __cplusplus
}
#endif

#endif
