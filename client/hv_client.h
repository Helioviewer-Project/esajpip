/* One source's cache, metadata and whole-frame request bookkeeping.
 * The host owns HTTP transport and the channel. Serialize calls per source.
 * This API uses the served profile described in README.md. */
#ifndef HV_CLIENT_H
#define HV_CLIENT_H

#include "hv_metadata.h"
#include "hv_reconstruct.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_client hv_client;

/* {0} requests full resolution and quality. Positive fit dimensions select
 * the coarsest resolution covering the aspect-ratio-preserving viewport fit.
 * Otherwise reduce removes highest resolutions (INT_MAX selects the lowest).
 * layers=0 means all layers; positive values are clamped to the source. */
typedef struct {
    int reduce;
    double fit_width, fit_height;
    int layers;
} hv_client_options;

enum { HV_CLIENT_READY, HV_CLIENT_HEADER, HV_CLIENT_FRAME };
typedef struct {
    hv_status source;
    int reduce;
    uint32_t width, height;
    int layers;                 /* minimum cached layers at this reduction */
    int ready;
    int request;                /* READY, HEADER or FRAME */
    int requested_layers;       /* clamped quality for a FRAME request */
} hv_client_view;

hv_client *hv_client_create(void);
/* create returns NULL on allocation failure. Other calls require a live
 * client; destroy accepts NULL. Consult error only after a failed call. */
void hv_client_destroy(hv_client *client);
const char *hv_client_error(const hv_client *client);

/* Submit the initial response before asking for the frame count. Returns its
 * EOR reason, or -1. A prepared request is confirmed only after a complete
 * WINDOW_DONE/IMAGE_DONE response. Other EOR reasons retain bytes without
 * confirming quality; prepare again to retry. Messages preceding an error
 * remain cached, with the pending request available after restoration. */
int hv_client_response(hv_client *client, const uint8_t *body, size_t size);
size_t hv_client_frames(hv_client *client);

/* Inspect without changing the pending request. A missing header gives HEADER
 * with zero geometry. Returns 0, or -1 with hv_client_error. */
int hv_client_status(hv_client *client, uint64_t frame, const hv_client_options *options,
                     hv_client_view *view);

/* Inspect and remember the next request. HEADER means stream=frame,layers=0;
 * FRAME means stream=frame,fsiz=width,height,closest,layers=<clamped options>.
 * No len or ROI. With layers=0, omit the layers field. After submitting its
 * response, prepare again until READY. Only one request may be pending. */
int hv_client_prepare(hv_client *client, uint64_t frame, const hv_client_options *options,
                      hv_client_view *view);

/* Replacement channel for the same immutable target: declare these batches
 * with stream=frame count,layers=0; the host handles cnew/cid and HTTP errors.
 * Start cursor at zero, repeat until an empty batch. Do not ingest normal
 * responses between batches. Restoration preserves the pending frame request.
 * restore_response checks metadata replay and requires a normal EOR. */
int hv_client_model(hv_client *client, size_t *cursor, char *text, size_t capacity);
int hv_client_restore_response(hv_client *client, const uint8_t *body, size_t size);

/* XML is borrowed until destroy; palette is copied into the supplied buffer.
 * reconstruct follows hv_reconstruct's size-query/caller-buffer convention,
 * preserving sample precision for the host's decoder. */
int hv_client_xml(hv_client *client, uint64_t frame, const uint8_t **xml, size_t *size);
int hv_client_palette(hv_client *client, uint64_t frame, int *channels,
                      uint8_t *table, size_t capacity);
size_t hv_client_reconstruct(hv_client *client, uint64_t frame, uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
