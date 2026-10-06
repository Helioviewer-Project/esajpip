#include "jpeg2000/hv_metadata.h"
/* hvc_wasm.c: the WebAssembly module's entry points, for js/jpip_channel.mjs.
 *
 * One module instance is one source: its data-bins and the image last
 * decoded. The host does the HTTP exchange and passes each response body
 * in. Native decoding tests also compile these entry points. In WebAssembly,
 * sizes and addresses are 32 bits, so a host passes them as numbers. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hvc_decode.h"
#include "hvc_openjpeg.h"

#ifdef __wasm__
_Static_assert(sizeof(size_t) == 4, "JavaScript uses the wasm32 size_t ABI");
#define EXPORT(name) __attribute__((export_name(#name))) name
#else
#define EXPORT(name) name
#endif

static hvc *client;
static hvc_image image;
static char error[256];

/* Memory for the host to place a response body in. */
void *EXPORT(hvc_wasm_alloc)(size_t size) {
    return malloc(size);
}

void EXPORT(hvc_wasm_free)(void *memory) {
    free(memory);
}

/* Adds the data-bins of one response body to the store: its end-of-response
 * reason (T.808 D.3), or -1 with hvc_wasm_error. */
int EXPORT(hvc_wasm_response)(const uint8_t *body, size_t size) {
    error[0] = 0;
    if (!client && !(client = hvc_create(NULL, NULL))) {
        snprintf(error, sizeof error, "out of memory");
        return -1;
    }
    return hvc_response(client, body, size);
}

int EXPORT(hvc_wasm_restore_response)(const uint8_t *body, size_t size) {
    error[0] = 0;
    if (!client) return -1;
    return hvc_restore_response(client, body, size);
}

void EXPORT(hvc_wasm_cancel_request)(void) {
    if (client) hvc_cancel_request(client);
}

static char model[2048];
static size_t model_next;

const char *EXPORT(hvc_wasm_model)(size_t cursor, size_t capacity) {
    error[0] = 0;
    if (!client) return NULL;
    model_next = cursor;
    if (capacity > sizeof model) capacity = sizeof model;
    if (hvc_model(client, &model_next, model, capacity) < 0) {
        snprintf(error, sizeof error, "cache cannot be declared within the request limit");
        return NULL;
    }
    return model;
}

size_t EXPORT(hvc_wasm_model_next)(void) { return model_next; }

/* Indexes the target's complete presentation once: its layer count, or 0 with
 * hvc_wasm_error. */
uint32_t EXPORT(hvc_wasm_frames)(void) {
    error[0] = 0;
    if (!client) return 0;
    size_t count = hvc_frames(client);
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

uint32_t EXPORT(hvc_wasm_codestreams)(void) {
    if (!client) return 0;
    size_t count = hvc_codestreams(client);
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

/* The XML associated with a frame, good until hvc_wasm_reset:
 * its size, 0 when it has none, or -1 with hvc_wasm_error;
 * and where it is. */
static const uint8_t *xml;

int EXPORT(hvc_wasm_xml_size)(uint32_t frame) {
    error[0] = 0;
    if (!client) return -1;
    size_t size;
    if (hvc_xml(client, frame, &xml, &size) != 0)
        return -1;
    return size > INT32_MAX ? INT32_MAX : (int)size;
}

const uint8_t *EXPORT(hvc_wasm_xml)(void) {
    return xml;
}

/* The color table of a frame, good until the
 * next call: its entries, 0 when it has none, or -1 with hvc_wasm_error;
 * then the values of an entry, and where the table is. */
static uint8_t palette[HV_PALETTE_MAX];
static int palette_channels;

int EXPORT(hvc_wasm_palette)(uint32_t frame) {
    error[0] = 0;
    if (!client) return -1;
    return hvc_palette(client, frame, &palette_channels, palette, sizeof palette);
}

int EXPORT(hvc_wasm_palette_channels)(void) {
    return palette_channels;
}

const uint8_t *EXPORT(hvc_wasm_palette_table)(void) {
    return palette;
}

/* Inspect or prepare a frame request. JavaScript reads the fixed-width view;
 * the client owns the pending request and confirms its completed response. */
#define VIEW_OFFSET(field, offset) \
    _Static_assert(offsetof(hvc_view, field) == offset, "JavaScript view layout: " #field)
_Static_assert(sizeof(int) == 4, "JavaScript reads 32-bit ints");
_Static_assert(sizeof(((hvc_view *)0)->source.quality) == 33 * 4, "JavaScript quality capacity");
VIEW_OFFSET(source.width, 0);
VIEW_OFFSET(source.height, 4);
VIEW_OFFSET(source.components, 8);
VIEW_OFFSET(source.resolutions, 12);
VIEW_OFFSET(source.complete, 16);
VIEW_OFFSET(source.layers, 20);
VIEW_OFFSET(source.quality, 24);
VIEW_OFFSET(reduce, 156);
VIEW_OFFSET(width, 160);
VIEW_OFFSET(height, 164);
VIEW_OFFSET(layers, 168);
VIEW_OFFSET(ready, 172);
VIEW_OFFSET(request, 176);
VIEW_OFFSET(requested_layers, 180);
VIEW_OFFSET(codestream, 184);
#undef VIEW_OFFSET
const hvc_view *EXPORT(hvc_wasm_view)(uint32_t frame, int reduce,
                                          double width, double height, int layers, int prepare) {
    error[0] = 0;
    static hvc_view view;
    if (!client) return NULL;
    hvc_options options = {reduce, width, height, layers};
    int result = prepare ? hvc_prepare(client, frame, &options, &view)
                         : hvc_status(client, frame, &options, &view);
    return result == 0 ? &view : NULL;
}

/* Decodes a frame without its `reduce` highest
 * resolutions (hvc_reconstruct, hvc_openjpeg_decode): 0, with the image in the
 * accessors below until the next call, or -1 with hvc_wasm_error. */
int EXPORT(hvc_wasm_decode)(uint32_t frame, int reduce) {
    error[0] = 0;
    if (!client) return -1;
    int channels, entries = hvc_palette(client, frame, &channels, NULL, 0);
    free(image.pixels);
    image.pixels = NULL;
    if (entries < 0) return -1;
    hvc_input *input = hvc_input_open(client, frame, reduce);
    if (!input) return -1;
    int status = hvc_openjpeg_decode(hvc_input_data(input), hvc_input_size(input), reduce,
                                     entries ? HVC_IMAGE_INDICES : HVC_IMAGE_SAMPLES,
                                     &image, error, sizeof error);
    hvc_input_close(input);
    return status;
}

const uint8_t *EXPORT(hvc_wasm_pixels)(void) { return image.pixels; }
uint32_t EXPORT(hvc_wasm_width)(void) { return image.width; }
uint32_t EXPORT(hvc_wasm_height)(void) { return image.height; }
int EXPORT(hvc_wasm_components)(void) { return image.components; }

/* Why the last call failed, NUL-terminated. */
const char *EXPORT(hvc_wasm_error)(void) {
    return error[0] ? error : client ? hvc_error(client) : "client is closed";
}

/* Forgets the channel: its data-bins and its image. */
void EXPORT(hvc_wasm_reset)(void) {
    hvc_destroy(client);
    client = NULL;
    free(image.pixels);
    image.pixels = NULL;
    xml = NULL;
}
