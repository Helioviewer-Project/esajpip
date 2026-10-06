/* One source's cache, metadata and whole-frame request bookkeeping.
 * The host owns HTTP transport and the channel. Serialize calls per source.
 * JPIP reconstruction uses the served profile described in README.md. */
#ifndef HVC_H
#define HVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* End-of-response reasons, from T.808 D.3. */
enum {
    HVC_EOR_IMAGE_DONE = 1,
    HVC_EOR_WINDOW_DONE = 2,
    HVC_EOR_WINDOW_CHANGE = 3,
    HVC_EOR_BYTE_LIMIT_REACHED = 4,
    HVC_EOR_QUALITY_LIMIT_REACHED = 5,
    HVC_EOR_SESSION_LIMIT_REACHED = 6,
    HVC_EOR_RESPONSE_LIMIT_REACHED = 7,
    HVC_EOR_NON_SPECIFIED = 0xFF
};

typedef struct hvc hvc;

/* {0} requests full resolution and quality. Positive fit dimensions select
 * the coarsest resolution covering the aspect-ratio-preserving viewport fit.
 * Otherwise reduce removes highest resolutions (INT_MAX selects the lowest).
 * layers=0 means all layers; positive values are clamped to the source. */
typedef struct {
    int reduce;
    double fit_width, fit_height;
    int layers;
} hvc_options;

/* Source image information: the image's size and
 * components, its resolution levels, and how many of them, from the
 * lowest, are complete at full quality. `quality` also describes confirmed
 * previews: the minimum whole layers across the precinct bins of each
 * resolution. All 0 until the main header is complete. */
typedef struct {
    uint32_t width, height;
    int components;
    int resolutions;
    int complete;
    int layers;          /* total source quality layers */
    int quality[33];     /* whole layers per resolution, lowest first */
} hvc_frame_status;

enum { HVC_READY, HVC_HEADER, HVC_FRAME };
typedef struct {
    hvc_frame_status source;
    int reduce;
    uint32_t width, height;
    int layers;                 /* minimum cached layers at this reduction */
    int ready;
    int request;                /* READY, HEADER or FRAME */
    int requested_layers;       /* clamped quality for a FRAME request */
    uint64_t codestream;        /* JPIP stream ID for this layer */
} hvc_view;

/* Decoder geometry, highest resolution first. The inspector checks rendering
 * support and supplies exact dimensions across all tiles/output components.
 * It may use hvc_input_open and hvc_render_read, but must not call status or
 * prepare. Successful results are cached per frame; failures may be retried. */
typedef struct {
    int components, resolutions, layers;
    uint32_t width[33], height[33];
} hvc_info;
typedef int (*hvc_inspect_fn)(hvc *client, size_t frame, void *context,
                             hvc_info *info, char *error, size_t error_size);
/* Both sources use the same inspector and frame API. Context outlives client.
 * With no inspector, JPIP uses its restricted coding profile's geometry;
 * local sources allow metadata/bytes but cannot supply status. */
hvc *hvc_create(hvc_inspect_fn inspect, void *context);
/* Native mapped-file source. NULL with error on failure. */
hvc *hvc_open_local(const char *path, hvc_inspect_fn inspect, void *context,
                     char *error, size_t error_size);
/* create returns NULL on allocation failure. Other calls require a live
 * client; destroy accepts NULL. Consult error only after a failed call. */
void hvc_destroy(hvc *client);
const char *hvc_error(const hvc *client);

/* Submit the initial response before asking for the frame count. Returns its
 * EOR reason, or -1. A prepared request is confirmed only after a complete
 * WINDOW_DONE/IMAGE_DONE response. Other EOR reasons retain bytes without
 * confirming quality; prepare again to retry. Messages preceding an error
 * remain cached. After a refusal, the host must discard the source and channel;
 * subsequent calls are not automatically disabled. Existing immutable decode
 * inputs remain subject to their documented source lifetime. */
int hvc_response(hvc *client, const uint8_t *body, size_t size);
/* Frames are JPX layers in file order, or JP2's single implicit layer. */
size_t hvc_frames(hvc *client);
/* Source count, used for a metadata-only restoration request. */
size_t hvc_codestreams(hvc *client);

/* Inspect without changing the pending request. A missing header gives HEADER
 * with zero geometry. Returns 0, or -1 with hvc_error. */
int hvc_status(hvc *client, uint64_t frame, const hvc_options *options,
               hvc_view *view);

/* Inspect and remember the next request. HEADER means stream=view.codestream,layers=0;
 * FRAME means stream=view.codestream,fsiz=width,height,closest,layers=view.requested_layers.
 * Whole-frame windows only. A byte-limit EOR retains bytes without confirmation;
 * prepare again and use the newly returned request. If unconfirmed precinct
 * bytes precede a layer-limited request, requested_layers is raised to full
 * quality so a cached mid-packet tail cannot be confirmed as a layer boundary.
 * Always send view.requested_layers, not the original option. A full-quality
 * host may instead use response/status without prepare; readiness then comes
 * only from completed bins. If WINDOW_DONE/IMAGE_DONE leaves the requested view
 * unready, report failure rather than repeating a completed window forever.
 * Only one request may be pending.
 * Local sources return READY immediately, without a network request. */
int hvc_prepare(hvc *client, uint64_t frame, const hvc_options *options,
                hvc_view *view);

/* Replacement channel for the same immutable target: declare these batches
 * with stream=codestream count,layers=0; the host handles cnew/cid and HTTP errors.
 * Start cursor at zero, repeat until an empty batch. Do not ingest normal
 * responses between batches. Restoration preserves the pending frame request.
 * restore_response checks metadata replay and requires a normal EOR. */
int hvc_model(hvc *client, size_t *cursor, char *text, size_t capacity);
int hvc_restore_response(hvc *client, const uint8_t *body, size_t size);

/* XML is borrowed until destroy; palette is copied into the supplied buffer.
 * codestream follows the size-query/caller-buffer convention,
 * preserving sample precision for the host's decoder. */
int hvc_xml(hvc *client, uint64_t frame, const uint8_t **xml, size_t *size);
int hvc_palette(hvc *client, uint64_t frame, int *channels,
                uint8_t *table, size_t capacity);
size_t hvc_codestream(hvc *client, uint64_t frame, uint8_t *out, size_t capacity);

/* Host disk cache: a JPIP frame's complete data-bins, size-query/caller-buffer,
 * and back into a frame of the same image. A refused import changes nothing. */
size_t hvc_export(hvc *client, uint64_t frame, uint8_t *out, size_t capacity);
int hvc_import(hvc *client, uint64_t frame, const uint8_t *block, size_t size);

#ifdef __cplusplus
}
#endif
#endif
