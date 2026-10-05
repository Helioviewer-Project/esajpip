/* Owning local JP2/JPX codestream access, independent of serving and decoding. */
#ifndef HV_LOCAL_H
#define HV_LOCAL_H
#include <stddef.h>
#include <stdint.h>
#include "hv_presentation.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_local hv_local;
/* Maps regular files and closes their descriptors. Files must remain immutable
 * until close. Kind is detected from content. Supports top-level JP2C/FTBL,
 * mixed sources, local MDAT fragments, and referenced local files. Relative
 * locations resolve against path's directory; file:// uses the project's path
 * convention. Other schemes/queries are unsupported when referenced.
 * JPX codestream extensions (J2CX) are unsupported. Other box bodies are opaque.
 * This is codestream access, not conformance validation or movie presentation.
 * NULL on failure with error; all acquired resources are released. POSIX I/O. */
hv_local *hv_local_open(const char *path, char *error, size_t error_size);
void hv_local_close(hv_local *source); /* accepts NULL */
size_t hv_local_codestreams(const hv_local *source);
/* Lazily read the file's independent codestream/layer presentation. Both
 * outputs borrow source storage until close; do not free or modify them.
 * data is the base for the presentation's box offsets and shared field readers.
 * Returns 0, or -1 with error and NULL outputs. Failure leaves byte access
 * available and permits retry. This does not establish rendering support or
 * change codestream ordering. Serialize access with other calls on source. */
int hv_local_presentation(hv_local *source, const uint8_t **data,
                           const hv_presentation **presentation,
                           char *error, size_t error_size);
/* Query with out=NULL. Otherwise capacity must hold the complete codestream;
 * insufficient capacity writes nothing. Returns the size, or 0 with error.
 * Original bytes are copied in fragment order. Caller owns out independently
 * of source lifetime. No packet parsing, decoding or hidden assembly cache. */
size_t hv_local_copy(const hv_local *source, size_t codestream, uint8_t *out,
                     size_t capacity, char *error, size_t error_size);
/* Read up to capacity bytes at a codestream-relative offset. Reads across
 * fragments; no whole-stream assembly or retained read position. Caller owns
 * out. Returns bytes read, 0 at the exact end or for capacity=0, or 0 with error.
 * Offset past the end, invalid index or NULL out with nonzero capacity fail
 * before writing. Source mappings must outlive reads; files remain immutable. */
size_t hv_local_read(const hv_local *source, size_t codestream, size_t offset,
                     uint8_t *out, size_t capacity, char *error, size_t error_size);
/* Read the codestream's SIZ fields using hv_read_siz's caller-buffer contract.
 * Reads only its bounded header, including across fragments. No metadata or
 * coding-feature checks; failure leaves copying available. These are reference
 * grid/component fields, not JPX presentation or decoder resolution levels. */
int hv_local_siz(const hv_local *source, size_t codestream, SizFixed *fixed,
                  Component *components, size_t capacity, char *error, size_t error_size);
/* Lazily indexes metadata; a metadata error does not prevent copying bytes.
 * First codestream-associated XML, falling back to first file-level XML.
 * Layer associations do not imply a codestream association. Returned bytes
 * are borrowed until close. Returns 0 (including absent XML), or -1 with error. */
int hv_local_xml(hv_local *source, size_t codestream, const uint8_t **xml,
                 size_t *size, char *error, size_t error_size);
/* Layer-associated XML, including registered codestreams and file fallback,
 * with the shared hv_metadata_layer_xml contract. Layer indices are independent. */
int hv_local_layer_xml(hv_local *source, size_t layer, const uint8_t **xml,
                       size_t *size, char *error, size_t error_size);
/* Same palette/capacity contract as hv_metadata_palette in hv_metadata.h. */
int hv_local_palette(hv_local *source, size_t codestream, int *channels,
                     uint8_t *table, size_t capacity, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
