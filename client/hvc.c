#include "hvc_decode.h"
#ifndef __wasi__
#include "jpeg2000/hv_local.h"
#endif
#include "hvc_metadata.h"
#include "hvc_reconstruct.h"
#include "hvc_frame.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *table;
    int entries, channels, ready;
} hvc_palette_table;

struct hvc {
#ifndef __wasi__
    hv_local *local;
#endif
    hvc_info *info;
    hvc_palette_table *palettes;
    size_t palette_count;
    hvc_inspect_fn inspect;
    void *decoder;
    hv_presentation presentation;
    const uint8_t *presentation_data;
    hvc_cache cache;
    hv_metadata metadata;
    char error[256];
    int pending;
    uint64_t codestream;
    int reduce, layers;
};

static int fail(hvc *client, const char *message) {
    snprintf(client->error, sizeof client->error, "%s", message);
    return -1;
}

hvc *hvc_create(hvc_inspect_fn inspect, void *context) {
    hvc *client = calloc(1, sizeof *client);
    if (client) { client->inspect = inspect; client->decoder = context; }
    return client;
}

#ifndef __wasi__
hvc *hvc_open_local(const char *path, hvc_inspect_fn inspect, void *context,
                     char *error, size_t error_size) {
    hvc *client = hvc_create(inspect, context);
    if (!client) { snprintf(error, error_size, "out of memory"); return NULL; }
    client->local = hv_local_open(path, error, error_size);
    if (!client->local) { hvc_destroy(client); return NULL; }
    return client;
}

#endif

static const hv_presentation *presentation(hvc *client, const uint8_t **data) {
#ifndef __wasi__
    if (client->local) {
        const hv_presentation *view;
        if (hv_local_presentation(client->local, data, &view, client->error, sizeof client->error))
            return NULL;
        return view;
    }
#endif
    if (!client->presentation.layers &&
        hvc_metadata_presentation(&client->cache, &client->presentation_data, &client->presentation,
                                   client->error, sizeof client->error)) return NULL;
    *data = client->presentation_data;
    return &client->presentation;
}

static const hv_presentation *frame_presentation(hvc *client, uint64_t frame, const uint8_t **data) {
    const hv_presentation *view = presentation(client, data);
    if (!view) return NULL;
    if (frame >= view->layers) { fail(client, "frame index out of range"); return NULL; }
    return view;
}

static int frame_codestream(hvc *client, uint64_t frame, size_t *codestream) {
    const uint8_t *data;
    const hv_presentation *view = frame_presentation(client, frame, &data);
    if (!view) return -1;
    const hv_registration *registration = &view->layer[frame].registration;
    if (registration->count != 1)
        return fail(client, "unsupported multiple-codestream composition");
    hv_registration_entry entry;
    const char *reason = hv_registration_read(registration, 0, &entry);
    if (reason) return fail(client, reason);
    *codestream = entry.codestream;
    return 0;
}

int hvc_render_read(hvc *client, uint64_t frame, size_t output_components, hv_render *render) {
    const uint8_t *data;
    const hv_presentation *view = frame_presentation(client, frame, &data);
    if (!view) return -1;
    size_t at;
    const char *reason = hv_render_read(data, view, (size_t)frame, output_components, render, &at);
    return reason ? fail(client, reason) : 0;
}

static const hvc_info *decoder_info(hvc *client, size_t frame, size_t codestream) {
    if (!client->inspect) { fail(client, "status requires decoder information"); return NULL; }
    if (!client->info) {
        size_t count = hvc_frames(client);
        if (!count) return NULL;
        client->info = calloc(count, sizeof *client->info);
        if (!client->info) { fail(client, "out of memory"); return NULL; }
    }
    hvc_info *info = &client->info[frame];
    if (info->resolutions) return info;
    hvc_info result = {0};
    if (client->inspect(client, frame, client->decoder, &result,
                        client->error, sizeof client->error) != 0) return NULL;
    if (result.components < 1 || result.resolutions < 1 || result.resolutions > 33 || result.layers < 1) {
        fail(client, "invalid decoder information"); return NULL;
    }
    for (int r = 0; r < result.resolutions; r++)
        if (!result.width[r] || !result.height[r] ||
            (r && (result.width[r] > result.width[r-1] || result.height[r] > result.height[r-1]))) {
            fail(client, "invalid decoder dimensions"); return NULL;
        }
    const hvc_frame *profile = NULL;
#ifndef __wasi__
    if (!client->local)
#endif
    {
        profile = hvc_frame_get(&client->cache, codestream, client->error, sizeof client->error);
        if (!profile) return NULL;
    }
    if (profile) {
        /* Serving admits zero-origin, matching grids with uniform coding.
         * An inspector must not silently change request geometry. */
        if (result.resolutions != profile->resolutions || result.layers != profile->layers) {
            fail(client, "decoder information differs from the JPIP coding profile"); return NULL;
        }
        for (int r = 0; r < result.resolutions; r++)
            if (result.width[r] != (uint32_t)ceil(ldexp(profile->width, -r)) ||
                result.height[r] != (uint32_t)ceil(ldexp(profile->height, -r))) {
                fail(client, "decoder geometry differs from the JPIP coding profile"); return NULL;
            }
    }
    *info = result;
    return info;
}

const hvc_info *hvc_info_read(hvc *client, uint64_t frame) {
    size_t codestream;
    if (frame_codestream(client, frame, &codestream)) return NULL;
    return decoder_info(client, (size_t)frame, codestream);
}

struct hvc_input {
#ifndef __wasi__
    hv_local_input *local;
#endif
    size_t size;
    uint8_t *bytes;
};

hvc_input *hvc_input_open(hvc *client, uint64_t frame, int reduce) {
    if (reduce < 0) { fail(client, "negative input reduction"); return NULL; }
    size_t codestream;
    if (frame_codestream(client, frame, &codestream)) return NULL;
    hvc_input *input = calloc(1, sizeof *input);
    if (!input) { fail(client, "out of memory"); return NULL; }
#ifndef __wasi__
    if (client->local) {
        input->local = hv_local_input_open(client->local, codestream, client->error, sizeof client->error);
        if (!input->local) goto fail;
        input->size = hv_local_copy(client->local, codestream, NULL, 0, client->error, sizeof client->error);
        if (input->size) return input;
        goto fail;
    }
#endif
    input->size = hvc_reconstruct_alloc(&client->cache, codestream, reduce, &input->bytes,
                                        client->error, sizeof client->error);
    if (!input->size) goto fail;
    return input;
fail:
    hvc_input_close(input);
    return NULL;
}

void hvc_input_close(hvc_input *input) {
    if (!input) return;
#ifndef __wasi__
    hv_local_input_close(input->local);
#endif
    free(input->bytes);
    free(input);
}
size_t hvc_input_size(const hvc_input *input) { return input->size; }
const uint8_t *hvc_input_data(const hvc_input *input) { return input->bytes; }
size_t hvc_input_read(const hvc_input *input, size_t offset, uint8_t *out, size_t capacity) {
    if (offset > input->size || (capacity && !out)) return SIZE_MAX;
    size_t count = input->size - offset;
    if (count > capacity) count = capacity;
    if (!count) return 0;
#ifndef __wasi__
    if (input->local) {
        char error[256];
        size_t read = hv_local_input_read(input->local, offset, out, count, error, sizeof error);
        return read == count ? count : SIZE_MAX;
    }
#endif
    memcpy(out, input->bytes + offset, count);
    return count;
}

void hvc_destroy(hvc *client) {
    if (!client) return;
#ifndef __wasi__
    hv_local_close(client->local);
#endif
    free(client->info);
    for (size_t i = 0; i < client->palette_count; i++) free(client->palettes[i].table);
    free(client->palettes);
    hv_presentation_free(&client->presentation);
    hv_metadata_close(&client->metadata);
    hvc_cache_release(&client->cache);
    free(client);
}

const char *hvc_error(const hvc *client) { return client->error; }

static int ingest(hvc *client, const uint8_t *body, size_t size, int restoring) {
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    int status;
    hvc_jpp_begin(&reader, body, size);
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) {
        if (restoring) {
            if (!hvc_cache_match_metadata(&client->cache, &message))
                return fail(client, "replacement channel metadata differs from the cache");
        } else if (!hvc_cache_apply(&client->cache, &message))
            return fail(client, hvc_cache_error(&client->cache));
    }
    if (status == HVC_JPP_ERROR) return fail(client, hvc_jpp_error(&reader));
    return hvc_jpp_reason(&reader);
}

static int completed(int reason) {
    return reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_IMAGE_DONE;
}

int hvc_response(hvc *client, const uint8_t *body, size_t size) {
#ifndef __wasi__
    if (client->local) return fail(client, "JPIP operations require a remote source");
#endif
    int reason = ingest(client, body, size, 0);
    if (reason < 0 || !client->pending) return reason;
    if (!completed(reason)) {
        client->pending = HVC_READY;
        return reason;
    }
    if (client->pending == HVC_FRAME) {
        if (hvc_reconstruct_confirm(&client->cache, client->codestream, client->reduce, client->layers,
                                   client->error, sizeof client->error) != 0)
            return -1;
    } else {
        hvc_frame_status status;
        if (hvc_reconstruct_status(&client->cache, client->codestream, &status,
                                  client->error, sizeof client->error) != 0) return -1;
        if (!status.resolutions) return fail(client, "the server did not send the frame header");
    }
    client->pending = HVC_READY;
    return reason;
}

size_t hvc_frames(hvc *client) {
    const uint8_t *data;
    const hv_presentation *view = presentation(client, &data);
    return view ? view->layers : 0;
}

size_t hvc_codestreams(hvc *client) {
    const uint8_t *data;
    const hv_presentation *view = presentation(client, &data);
    return view ? view->codestreams : 0;
}

int hvc_status(hvc *client, uint64_t frame, const hvc_options *options,
               hvc_view *view) {
    hvc_options defaults = {0};
    if (!options) options = &defaults;
    *view = (hvc_view){0};
    if (options->reduce < 0 || options->layers < 0 ||
        !isfinite(options->fit_width) || !isfinite(options->fit_height) ||
        options->fit_width < 0 || options->fit_height < 0 ||
        ((options->fit_width > 0) != (options->fit_height > 0)) ||
        (options->fit_width > 0 && options->reduce != 0))
        return fail(client, "invalid frame options");
    size_t codestream;
    if (frame_codestream(client, frame, &codestream)) return -1;
    view->codestream = codestream;
    view->request = HVC_HEADER;
    int local = 0;
#ifndef __wasi__
    local = client->local != NULL;
#endif
    if (!local) {
        if (hvc_reconstruct_status(&client->cache, codestream, &view->source,
                                   client->error, sizeof client->error)) return -1;
        if (!view->source.resolutions) return 0;
    }
    const uint32_t *widths = NULL, *heights = NULL;
    if (local || client->inspect) {
        const hvc_info *info = decoder_info(client, (size_t)frame, codestream);
        if (!info) return -1;
        view->source.width = info->width[0];
        view->source.height = info->height[0];
        view->source.components = info->components;
        view->source.resolutions = info->resolutions;
        view->source.layers = info->layers;
        if (local) {
            view->source.complete = info->resolutions;
            for (int r = 0; r < info->resolutions; r++) view->source.quality[r] = info->layers;
        }
        widths = info->width; heights = info->height;
    }
    const hvc_frame_status *source = &view->source;
    int reduce = options->reduce;
    if (reduce >= source->resolutions) reduce = source->resolutions - 1;
    if (options->fit_width > 0) {
        double scale = fmin(options->fit_width / source->width, options->fit_height / source->height);
        while (reduce + 1 < source->resolutions &&
               (widths ? widths[reduce + 1] : ceil(ldexp(source->width, -reduce - 1))) >= source->width * scale &&
               (heights ? heights[reduce + 1] : ceil(ldexp(source->height, -reduce - 1))) >= source->height * scale)
            reduce++;
    }
    view->reduce = reduce;
    view->width = widths ? widths[reduce] : (uint32_t)ceil(ldexp(source->width, -reduce));
    view->height = heights ? heights[reduce] : (uint32_t)ceil(ldexp(source->height, -reduce));
    view->layers = source->layers;
    for (int r = 0; r < source->resolutions - reduce; r++)
        if (source->quality[r] < view->layers) view->layers = source->quality[r];
    int layers = options->layers;
    if (!layers || layers > source->layers) layers = source->layers;
    view->requested_layers = layers;
    view->ready = view->layers >= layers;
    view->request = view->ready ? HVC_READY : HVC_FRAME;
    return 0;
}

void hvc_cancel_request(hvc *client) { client->pending = HVC_READY; }

int hvc_prepare(hvc *client, uint64_t frame, const hvc_options *options,
                hvc_view *view) {
    if (client->pending) return fail(client, "a frame request is already pending");
    if (hvc_status(client, frame, options, view) != 0) return -1;
    if (view->request == HVC_FRAME && view->requested_layers < view->source.layers) {
        const hvc_frame *coding = hvc_frame_get(&client->cache, view->codestream,
                                                client->error, sizeof client->error);
        if (!coding) return -1;
        uint64_t end = coding->precinct_end[coding->resolutions - 1 - view->reduce];
        for (uint64_t id = 0; id < end; id++) {
            const hvc_bin *bin = hvc_cache_find(&client->cache, HVC_BIN_PRECINCT, view->codestream, id);
            if (bin && !bin->complete && bin->length > bin->packet_bytes) {
                /* A previous byte-limited window may have passed the requested
                 * layer and stopped inside a later packet. Only full delivery
                 * gives an unambiguous boundary without parsing packets. */
                view->requested_layers = view->source.layers;
                break;
            }
        }
    }
    client->pending = view->request;
    client->codestream = view->codestream;
    client->reduce = view->reduce;
    client->layers = view->requested_layers;
    return 0;
}

int hvc_model(hvc *client, size_t *cursor, char *text, size_t capacity) {
#ifndef __wasi__
    if (client->local) return fail(client, "JPIP operations require a remote source");
#endif
    int result = hvc_cache_model(&client->cache, cursor, text, capacity);
    if (result < 0) return fail(client, "cache cannot be declared within the request limit");
    return result;
}

int hvc_restore_response(hvc *client, const uint8_t *body, size_t size) {
#ifndef __wasi__
    if (client->local) return fail(client, "JPIP operations require a remote source");
#endif
    int reason = ingest(client, body, size, 1);
    if (reason >= 0 && !completed(reason) && reason != HVC_EOR_BYTE_LIMIT_REACHED)
        return fail(client, "replacement channel did not complete cache restoration");
    return reason;
}

static int metadata(hvc *client) {
    if (client->metadata.frames) return 0;
    return hvc_metadata_open(&client->cache, &client->metadata, client->error, sizeof client->error);
}

int hvc_xml(hvc *client, uint64_t frame, const uint8_t **xml, size_t *size) {
    *xml = NULL; *size = 0;
    const uint8_t *data;
    const hv_presentation *view = frame_presentation(client, frame, &data);
    if (!view) return -1;
#ifndef __wasi__
    if (client->local)
        return hv_local_layer_xml(client->local, (size_t)frame, xml, size, client->error, sizeof client->error);
#endif
    if (metadata(client)) return -1;
    return hv_metadata_layer_xml(&client->metadata, (size_t)frame, &view->layer[frame].registration,
                                  xml, size, client->error, sizeof client->error);
}

static int read_palette(hvc *client, size_t codestream, int *channels, uint8_t *table, size_t capacity) {
#ifndef __wasi__
    if (client->local)
        return hv_local_palette(client->local, codestream, channels, table, capacity,
                                 client->error, sizeof client->error);
#endif
    if (metadata(client)) return -1;
    return hv_metadata_palette(&client->metadata, codestream, channels, table, capacity,
                               client->error, sizeof client->error);
}

/* Palettes belong to immutable codestream metadata, shared by every layer
 * that uses that codestream. Cache absence too; failed reads remain retryable. */
int hvc_palette(hvc *client, uint64_t frame, int *channels, uint8_t *table, size_t capacity) {
    size_t codestream;
    *channels = 0;
    if (frame_codestream(client, frame, &codestream)) return -1;
    if (!client->palettes) {
        size_t count = hvc_codestreams(client);
        if (!count) return -1;
        client->palettes = calloc(count, sizeof *client->palettes);
        if (!client->palettes) return fail(client, "out of memory");
        client->palette_count = count;
    }
    hvc_palette_table *palette = &client->palettes[codestream];
    if (!palette->ready) {
        int count = 0;
        int entries = read_palette(client, codestream, &count, NULL, 0);
        if (entries < 0) return -1;
        size_t size = (size_t)entries * count;
        uint8_t *bytes = NULL;
        if (size) {
            bytes = malloc(size);
            if (!bytes) return fail(client, "out of memory");
            if (read_palette(client, codestream, &count, bytes, size) < 0) {
                free(bytes);
                return -1;
            }
        }
        *palette = (hvc_palette_table){bytes, entries, count, 1};
    }
    *channels = palette->channels;
    size_t size = (size_t)palette->entries * palette->channels;
    if (size && capacity >= size) memcpy(table, palette->table, size);
    return palette->entries;
}

size_t hvc_codestream(hvc *client, uint64_t frame, uint8_t *out, size_t capacity) {
    size_t codestream;
    if (frame_codestream(client, frame, &codestream)) return 0;
#ifndef __wasi__
    if (client->local)
        return hv_local_copy(client->local, codestream, out, capacity, client->error, sizeof client->error);
#endif
    return hvc_reconstruct(&client->cache, codestream, out, capacity, client->error, sizeof client->error);
}

/* Appends a complete bin as one message of codestream 0. */
static void export_bin(const hvc_cache *cache, int bin_class, uint64_t codestream, uint64_t id,
                       uint8_t *out, size_t capacity, size_t *size) {
    const hvc_bin *bin = hvc_cache_find(cache, bin_class, codestream, id);
    if (!bin || !bin->complete) return;
    hvc_jpp_message message = {bin_class, 0, id, 0, bin->length, 1, bin->data};
    size_t room = *size < capacity ? capacity - *size : 0;
    *size += hvc_jpp_write(&message, room ? out + *size : NULL, room);
}

size_t hvc_export(hvc *client, uint64_t frame, uint8_t *out, size_t capacity) {
    size_t codestream, size = 0;
#ifndef __wasi__
    if (client->local) { fail(client, "JPIP operations require a remote source"); return 0; }
#endif
    if (frame_codestream(client, frame, &codestream)) return 0;
    const hvc_frame *coding = hvc_frame_get(&client->cache, codestream, client->error, sizeof client->error);
    if (!coding) return 0;
    if (!out) capacity = 0;
    export_bin(&client->cache, HVC_BIN_MAIN_HEADER, codestream, 0, out, capacity, &size);
    export_bin(&client->cache, HVC_BIN_TILE_HEADER, codestream, 0, out, capacity, &size);
    for (uint64_t id = 0; id < coding->precinct_end[coding->resolutions - 1]; id++)
        export_bin(&client->cache, HVC_BIN_PRECINCT, codestream, id, out, capacity, &size);
    return size;
}

/* One pass over a block: check, or apply. */
static int import_pass(hvc *client, uint64_t codestream, const uint8_t *block, size_t size, int apply) {
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    uint64_t next = 0;
    hvc_jpp_begin(&reader, block, size);
    do {
        if (hvc_jpp_next(&reader, &message) != HVC_JPP_MESSAGE || message.offset || !message.last_byte)
            return fail(client, "not an exported frame");
        /* Export order: no bin twice. */
        uint64_t order;
        switch (message.bin_class) {
        case HVC_BIN_MAIN_HEADER: order = 0; break;
        case HVC_BIN_TILE_HEADER: order = 1; break;
        case HVC_BIN_PRECINCT: order = message.bin_id + 2; break;
        default: return fail(client, "not an exported frame");
        }
        if (order < next || (order < 2 && message.bin_id))
            return fail(client, "not an exported frame");
        next = order + 1;
        message.codestream = codestream;
        if (apply) {
            if (!hvc_cache_apply(&client->cache, &message))
                return fail(client, hvc_cache_error(&client->cache));
            continue;
        }
        const hvc_bin *bin = hvc_cache_find(&client->cache, message.bin_class, codestream, message.bin_id);
        if (bin && (bin->length > message.length || (bin->complete && bin->length != message.length) ||
                    (bin->length && memcmp(bin->data, message.data, bin->length))))
            return fail(client, "exported frame differs from the cached data-bins");
    } while (reader.position < size);
    return 0;
}

int hvc_import(hvc *client, uint64_t frame, const uint8_t *block, size_t size) {
    size_t codestream;
#ifndef __wasi__
    if (client->local) return fail(client, "JPIP operations require a remote source");
#endif
    if (frame_codestream(client, frame, &codestream)) return -1;
    /* Check everything first. */
    if (import_pass(client, codestream, block, size, 0)) return -1;
    return import_pass(client, codestream, block, size, 1);
}
