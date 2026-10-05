/* Decoder access shared by local and JPIP sources. */
#ifndef HVC_DECODE_H
#define HVC_DECODE_H
#include "hvc.h"
#include "jpeg2000/hv_render.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Channel instructions and exact palette samples. Borrowed until client
 * destruction. output_components is the decoder's transformed output count. */
int hvc_render_read(hvc *client, uint64_t frame, size_t output_components, hv_render *render);

typedef struct hvc_input hvc_input;
/* Immutable native input for one decode. Local inputs borrow mapped extents;
 * JPIP inputs own one reconstruction of the current confirmed data. Later
 * responses cannot change an open input. Open/close around the decoder lifetime,
 * and close all inputs before destroying client. Open is serialized with client
 * calls; read/size/data do not access its mutable cache and can run independently.
 * NULL with hvc_error on failure. No bytes pass through the host language. */
hvc_input *hvc_input_open(hvc *client, uint64_t frame);
void hvc_input_close(hvc_input *input);
size_t hvc_input_size(const hvc_input *input);
/* EOF returns 0, invalid arguments or a failed read return SIZE_MAX. */
size_t hvc_input_read(const hvc_input *input, size_t offset, uint8_t *out, size_t capacity);
/* Optional contiguous view, NULL for mapped/fragmented inputs. Borrowed until
 * input close. Decoders using read need no source-specific path. */
const uint8_t *hvc_input_data(const hvc_input *input);
#ifdef __cplusplus
}
#endif
#endif
