/* Local OpenJPEG extension. One coordinator serializes all workspace calls.
 * Bind only a raw J2K decoder. Unbind drains jobs and restores its original
 * pool. Keep the workspace active until its images and codec are destroyed. */
#ifndef OPJ_DECODER_WORKSPACE_H
#define OPJ_DECODER_WORKSPACE_H
#include "openjpeg.h"
#include "thread.h"
typedef struct opj_decoder_workspace opj_decoder_workspace;
opj_decoder_workspace *opj_decoder_workspace_create(int threads);
void opj_decoder_workspace_destroy(opj_decoder_workspace *workspace);
void opj_decoder_workspace_activate(opj_decoder_workspace *workspace);
void opj_decoder_workspace_bind(opj_decoder_workspace *workspace, opj_codec_t *codec);
void opj_decoder_workspace_unbind(opj_decoder_workspace *workspace, opj_codec_t *codec);
size_t opj_decoder_workspace_retained(const opj_decoder_workspace *workspace);
size_t opj_decoder_workspace_coefficient_allocations(const opj_decoder_workspace *workspace);
size_t opj_decoder_workspace_coefficient_hits(const opj_decoder_workspace *workspace);
/* After the pool has drained: retained bytes (0), hits (1), misses (2). */
size_t opj_decoder_workspace_codeblock_stat(const opj_decoder_workspace *workspace, unsigned which);
int opj_decoder_workspace_threads(const opj_decoder_workspace *workspace);
OPJ_BOOL opj_decoder_workspace_submit(opj_decoder_workspace *workspace, opj_job_fn function, void *user);
void opj_decoder_workspace_wait(opj_decoder_workspace *workspace);
/* Zero if fused gray output is unavailable; otherwise its required size. */
size_t opj_decoder_gray8_size(opj_codec_t *codec, const opj_image_t *image, OPJ_UINT32 component, OPJ_UINT32 reduce);
/* Select after a nonzero gray8_size result, before decode. pixels must hold
 * that many bytes and remain writable through decode completion. The selected
 * plane bypasses the normal integer DC-shift output: after decode, consume
 * pixels, not that component's ordinary opj_image sample array. Other planes
 * retain normal output. The caller owns pixels independently of codec lifetime. */
void opj_decoder_gray8_output(opj_codec_t *codec, OPJ_UINT32 component, OPJ_BYTE *pixels);
/* Internal image allocator hooks, used while a workspace is active. */
void *opj_workspace_image_data_alloc(size_t size);
void opj_workspace_image_data_free(void *data);
#endif
