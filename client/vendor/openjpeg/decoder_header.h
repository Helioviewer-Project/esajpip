/* Local OpenJPEG extension: scalar main-header coding parameters without
 * allocating a diagnostic codestream-info snapshot. Read after a successful
 * opj_read_header on a raw J2K decoder, before reading tile-part headers.
 * Invalid codec/component returns false with outputs unchanged. */
#ifndef OPJ_DECODER_HEADER_H
#define OPJ_DECODER_HEADER_H
#include "openjpeg.h"
#ifdef __cplusplus
extern "C" {
#endif
OPJ_BOOL opj_decoder_get_coding_parameters(opj_codec_t *codec, OPJ_UINT32 component,
    OPJ_UINT32 *resolutions, OPJ_UINT32 *layers);
#ifdef __cplusplus
}
#endif
#endif
