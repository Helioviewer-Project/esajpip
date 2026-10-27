/* hvc_openjpeg.c: see hvc_openjpeg.h. */
#include "hvc_openjpeg.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_error.h"
#include "decoder_workspace.h"
#include "decoder_header.h"
#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

static uint32_t ceil_div_pow2(uint32_t value, uint32_t power) {
    return (uint32_t)((value + ((uint64_t)1 << power) - 1) >> power);
}

typedef struct {
    const uint8_t *data;
    size_t size, position;
} memory;

static OPJ_SIZE_T read_memory(void *buffer, OPJ_SIZE_T count, void *user) {
    memory *m = user;
    if (m->position == m->size)
        return (OPJ_SIZE_T)-1;
    if (count > m->size - m->position)
        count = m->size - m->position;
    memcpy(buffer, m->data + m->position, count);
    m->position += count;
    return count;
}

static OPJ_OFF_T skip_memory(OPJ_OFF_T count, void *user) {
    memory *m = user;
    if (count < 0)
        return -1;
    if ((uint64_t)count > m->size - m->position)
        count = (OPJ_OFF_T)(m->size - m->position);
    m->position += (size_t)count;
    return count;
}

static OPJ_BOOL seek_memory(OPJ_OFF_T position, void *user) {
    memory *m = user;
    if (position < 0 || (uint64_t)position > m->size)
        return OPJ_FALSE;
    m->position = (size_t)position;
    return OPJ_TRUE;
}

/* OpenJPEG reports why it failed through this, a line at a time: the last
 * line is kept. */
typedef struct {
    char *text;
    size_t size;
    int reported;
} message;

static void keep_error(const char *text, void *user) {
    message *m = user;
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == '\n')
        length--;
    hv_fail(m->text, m->size, "%.*s", (int)length, text);
    m->reported = 1;
}

struct hvc_decoder {
    opj_decoder_workspace *workspace;
    uint8_t *input, *output;
    size_t input_capacity, output_capacity, growths;
    uint8_t *target;        /* caller's buffer for the next decode */
    size_t target_capacity;
    int target_rgba;
};
static void *reserve(uint8_t **buffer, size_t *capacity, size_t size, size_t *growths) {
    if (size > *capacity) {
        /* Exact high-water capacity: no speculative multiplication of 4K buffers. */
        uint8_t *next = realloc(*buffer, size);
        if (!next) return NULL;
        *buffer = next; *capacity = size; ++*growths;
    }
    return *buffer;
}
hvc_decoder *hvc_decoder_create(int threads) {
    hvc_decoder *decoder = calloc(1, sizeof *decoder);
    if (!decoder) return NULL;
    decoder->workspace = opj_decoder_workspace_create(threads);
    if (!decoder->workspace) { free(decoder); return NULL; }
    return decoder;
}
void hvc_decoder_destroy(hvc_decoder *decoder) {
    if (!decoder) return;
    opj_decoder_workspace_destroy(decoder->workspace);
    free(decoder->input); free(decoder->output); free(decoder);
}
void *hvc_decoder_input(hvc_decoder *decoder, size_t size) {
    return reserve(&decoder->input, &decoder->input_capacity, size, &decoder->growths);
}
size_t hvc_decoder_retained(const hvc_decoder *decoder) {
    return decoder ? decoder->input_capacity + decoder->output_capacity + opj_decoder_workspace_retained(decoder->workspace) : 0;
}
size_t hvc_decoder_coefficient_allocations(const hvc_decoder *d) { return opj_decoder_workspace_coefficient_allocations(d ? d->workspace : NULL); }
size_t hvc_decoder_coefficient_hits(const hvc_decoder *d) { return opj_decoder_workspace_coefficient_hits(d ? d->workspace : NULL); }
size_t hvc_decoder_codeblock_stat(const hvc_decoder *d, unsigned which) { return opj_decoder_workspace_codeblock_stat(d ? d->workspace : NULL, which); }
size_t hvc_decoder_growths(const hvc_decoder *decoder) { return decoder ? decoder->growths : 0; }
void hvc_decoder_output(hvc_decoder *decoder, uint8_t *pixels, size_t capacity, int rgba) {
    decoder->target = pixels; decoder->target_capacity = capacity; decoder->target_rgba = rgba;
}

/* One sample as 8 bits: unsigned, and its most significant bits. */
static uint8_t sample(const opj_image_comp_t *component, size_t i, int indexed) {
    int64_t value = component->data[i];
    int64_t max = ((int64_t)1 << component->prec) - 1;
    if (component->sgnd)
        value += (int64_t)1 << (component->prec - 1);
    value = value < 0 ? 0 : value > max ? max : value;
    if (indexed)
        return (uint8_t)value;
    return (uint8_t)(component->prec > 8 ? value >> (component->prec - 8)
                                         : value << (8 - component->prec));
}

/* DC shift has already clamped samples to their declared precision. Keep the
 * general scalar path for RGB/interleaving and >16-bit scientific components. */
static void pack_gray(const opj_image_comp_t *component, size_t count, uint8_t *out) {
    size_t i = 0;
    int bias = component->sgnd ? 1 << (component->prec - 1) : 0;
#ifdef __wasm_simd128__
    v128_t offset = wasm_i32x4_splat(bias);
    for (; i + 16 <= count; i += 16) {
        v128_t a = wasm_i32x4_add(wasm_v128_load(component->data + i), offset);
        v128_t b = wasm_i32x4_add(wasm_v128_load(component->data + i + 4), offset);
        v128_t c = wasm_i32x4_add(wasm_v128_load(component->data + i + 8), offset);
        v128_t d = wasm_i32x4_add(wasm_v128_load(component->data + i + 12), offset);
        if (component->prec > 8) {
            a = wasm_u32x4_shr(a, component->prec - 8); b = wasm_u32x4_shr(b, component->prec - 8);
            c = wasm_u32x4_shr(c, component->prec - 8); d = wasm_u32x4_shr(d, component->prec - 8);
        } else {
            a = wasm_i32x4_shl(a, 8 - component->prec); b = wasm_i32x4_shl(b, 8 - component->prec);
            c = wasm_i32x4_shl(c, 8 - component->prec); d = wasm_i32x4_shl(d, 8 - component->prec);
        }
        wasm_v128_store(out + i, wasm_u8x16_narrow_i16x8(
            wasm_i16x8_narrow_i32x4(a, b), wasm_i16x8_narrow_i32x4(c, d)));
    }
#endif
    /* Locals, as out may alias the component: compilers vectorize these loops. */
    const OPJ_INT32 *data = component->data;
    int prec = (int)component->prec;
    if (prec > 8) {
        for (; i < count; ++i) out[i] = (uint8_t)((unsigned)(data[i] + bias) >> (prec - 8));
    } else {
        for (; i < count; ++i) out[i] = (uint8_t)((unsigned)(data[i] + bias) << (8 - prec));
    }
}

/* Samples [start, end) of the selected components, interleaved in pixels of
 * stride bytes; a byte past the components is opaque alpha. */
typedef struct {
    const opj_image_comp_t *components[3];
    int count, indexed;
    size_t start, end;
    uint8_t *pixels;
    size_t stride;
} pack_band;

static void pack(const pack_band *b) {
    if (b->count == 1 && b->components[0]->prec <= 16 && !b->indexed) {
        opj_image_comp_t shifted = *b->components[0];
        shifted.data += b->start;
        pack_gray(&shifted, b->end - b->start, b->pixels + b->start);
        return;
    }
    for (int c = 0; c < b->count; c++)
        for (size_t i = b->start; i < b->end; i++)
            b->pixels[i * b->stride + (size_t)c] = sample(b->components[c], i, b->indexed);
    if (b->stride > (size_t)b->count)
        for (size_t i = b->start; i < b->end; i++)
            b->pixels[i * b->stride + (size_t)b->count] = 255;
}

static void pack_job(void *user, opj_tls_t *tls) {
    (void)tls;
    pack(user);
    free(user);
}

/* The pixels of a decode: the caller's target if they fit, else NULL. */
static uint8_t *output_pixels(hvc_decoder *context, uint8_t *target, size_t capacity, size_t bytes) {
    if (target) return bytes <= capacity ? target : NULL;
    return context ? reserve(&context->output, &context->output_capacity, bytes, &context->growths) : malloc(bytes);
}

static int decode(hvc_decoder *context, const uint8_t *codestream, size_t size, int reduce, const hv_render *render, const uint32_t *region, hvc_image *out,
                        char *error, size_t error_size) {
    memory input = {codestream, size, 0};
    message failure = {error, error_size, 0};
    const char *why = "out of memory";
    opj_dparameters_t parameters;
    opj_stream_t *stream = opj_stream_create(1 << 16, OPJ_TRUE);
    opj_codec_t *codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_image_t *image = NULL;
    int bound = 0;
    size_t count, packed = 0;
    int status = -1, c, indexed = render && render->channel[0].palette_column >= 0;
    OPJ_UINT32 components[3] = {0, 1, 2};

    uint8_t *target = context ? context->target : NULL;
    size_t target_capacity = context ? context->target_capacity : 0;
    int rgba = context && context->target_rgba;

    opj_decoder_workspace_activate(context ? context->workspace : NULL);
    memset(out, 0, sizeof *out);
    if (context) context->target = NULL;
    if (stream == NULL || codec == NULL)
        goto done;
    if (context) {
        opj_decoder_workspace_bind(context->workspace, codec);
        bound = 1;
    }
    opj_stream_set_read_function(stream, read_memory);
    opj_stream_set_skip_function(stream, skip_memory);
    opj_stream_set_seek_function(stream, seek_memory);
    opj_stream_set_user_data(stream, &input, NULL);
    opj_stream_set_user_data_length(stream, size);
    opj_set_error_handler(codec, keep_error, &failure);
    opj_set_default_decoder_parameters(&parameters);

    why = "cannot decode the codestream";
    if (!opj_setup_decoder(codec, &parameters) || !opj_read_header(stream, codec, &image))
        goto done;
    out->components = image->numcomps >= 3 ? 3 : 1;
    if (render) {
        if (render->channel_count != 1 && render->channel_count != 3) {
            why = "unsupported channel count";
            goto done;
        }
        out->components = indexed ? 1 : (int)render->channel_count;
        for (size_t channel = 0; channel < render->channel_count; channel++) {
            if (render->channel[channel].component >= image->numcomps) {
                why = "selected component does not exist";
                goto done;
            }
            if ((render->channel[channel].palette_column >= 0) != indexed ||
                (indexed && render->channel[channel].component != render->channel[0].component)) {
                why = "palette channels require one shared index component";
                goto done;
            }
            if (!indexed || channel == 0)
                components[channel] = (OPJ_UINT32)render->channel[channel].component;
        }
    }
    if (indexed && (image->comps[components[0]].sgnd || image->comps[components[0]].prec > 8)) {
        why = "palette indices require one unsigned component of at most 8 bits";
        goto done;
    }
    for (c = 0; c < out->components; c++) {
        const opj_image_comp_t *component = &image->comps[components[c]];
        OPJ_UINT32 resolutions, layers;
        if (!opj_decoder_get_coding_parameters(codec, components[c], &resolutions, &layers)) goto done;
        if (c == 0 || (int)resolutions < out->resolutions) out->resolutions = (int)resolutions;
        if (render && (component->dx != 1 || component->dy != 1 ||
                       component->x0 != image->x0 || component->y0 != image->y0)) {
            why = "selected output components require resampling";
            goto done;
        }
    }
    out->full_width = image->x1 - image->x0;
    out->full_height = image->y1 - image->y0;
    if (reduce < 0)
        reduce = 0;
    if (reduce > out->resolutions - 1)
        reduce = out->resolutions - 1;
    if (!opj_set_decoded_resolution_factor(codec, (OPJ_UINT32)reduce)) goto done;
    if (region) {
        why = "invalid decode region";
        if (region[0] >= region[2] || region[1] >= region[3] ||
            region[2] > out->full_width || region[3] > out->full_height ||
            image->x1 > INT_MAX || image->y1 > INT_MAX) goto done;
        for (c = 0; c < (int)image->numcomps; ++c)
            if (image->comps[c].dx != 1 || image->comps[c].dy != 1) goto done;
        const uint32_t gx = ceil_div_pow2(image->x0, reduce);
        const uint32_t gy = ceil_div_pow2(image->y0, reduce);
        out->grid_width = ceil_div_pow2(image->x1, reduce) - gx;
        out->grid_height = ceil_div_pow2(image->y1, reduce) - gy;
        out->x = ceil_div_pow2(image->x0 + region[0], reduce) - gx;
        out->y = ceil_div_pow2(image->y0 + region[1], reduce) - gy;
        if (!opj_set_decode_area(codec, image, image->x0 + region[0], image->y0 + region[1],
                                image->x0 + region[2], image->y0 + region[3])) goto done;
    }
    /* One whole gray tile: the DC shift packs each row while it is in cache. */
    if (context && !region && out->components == 1 && !indexed)
        packed = opj_decoder_gray8_size(codec, image, components[0], (OPJ_UINT32)reduce);
    if (packed) {
        why = target ? "output buffer too small" : "out of memory";
        if ((out->pixels = output_pixels(context, target, target_capacity, packed)) == NULL) goto done;
        opj_decoder_gray8_output(codec, components[0], out->pixels);
    }
    why = "cannot decode the codestream";
    if (!opj_decode(codec, stream, image) || !opj_end_decompress(codec, stream))
        goto done;

    out->width = image->comps[components[0]].w;
    out->height = image->comps[components[0]].h;
    if (!region) { out->grid_width = out->width; out->grid_height = out->height; }
    for (c = 0; c < out->components; c++) {
        const opj_image_comp_t *component = &image->comps[components[c]];
        if (component->data == NULL || component->w != out->width ||
            component->h != out->height || component->prec < 1 || component->prec > 31) {
            why = "unsupported components";
            goto done;
        }
    }
    count = (size_t)out->width * out->height;
    if (packed) {
        why = "unsupported components";
        if (count != packed) goto done;
        status = 0;
        goto done;
    }
    why = "out of memory";
    if (count > SIZE_MAX / 4) goto done;
    size_t stride = target && rgba && out->components == 3 ? 4 : (size_t)out->components;
    size_t bytes = count * stride;
    why = target ? "output buffer too small" : "out of memory";
    out->pixels = output_pixels(context, target, target_capacity, bytes);
    if (!out->pixels) goto done;
    pack_band whole = {{NULL, NULL, NULL}, out->components, indexed, 0, count, out->pixels, stride};
    for (c = 0; c < out->components; c++)
        whole.components[c] = &image->comps[components[c]];
    /* Bands of at least 64K samples, a few per worker. */
    size_t bands = context ? (size_t)opj_decoder_workspace_threads(context->workspace) * 4 : 1;
    if (bands > count / 65536) bands = count / 65536;
    if (bands < 2) {
        pack(&whole);
    } else {
        size_t step = (count + bands - 1) / bands;
        for (size_t start = 0; start < count; start += step) {
            pack_band band = whole, *job = malloc(sizeof band);
            band.start = start;
            band.end = count - start < step ? count : start + step;
            if (job) {
                *job = band;
                if (opj_decoder_workspace_submit(context->workspace, pack_job, job)) continue;
                free(job);
            }
            pack(&band);
        }
        opj_decoder_workspace_wait(context->workspace);
    }
    status = 0;

done:
    if (bound) {
        /* Drain before freeing tile state, including on malformed input. */
        opj_decoder_workspace_unbind(context->workspace, codec);
    }
    if (status != 0 && !failure.reported)
        hv_fail(error, error_size, "%s", why);
    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    opj_decoder_workspace_activate(NULL);
    return status;
}

int hvc_openjpeg_decode(const uint8_t *data, size_t size, int reduce, const hv_render *render,
                        hvc_image *out, char *error, size_t error_size) {
    return decode(NULL, data, size, reduce, render, NULL, out, error, error_size);
}
int hvc_openjpeg_decode_context(hvc_decoder *context, const uint8_t *data, size_t size, int reduce,
    const hv_render *render, hvc_image *out, char *error, size_t error_size) {
    return decode(context, data, size, reduce, render, NULL, out, error, error_size);
}

int hvc_openjpeg_decode_region(hvc_decoder *context, const uint8_t *data, size_t size, int reduce,
    const hv_render *render, const uint32_t region[4], hvc_image *out, char *error, size_t error_size) {
    return decode(context, data, size, reduce, render, region, out, error, error_size);
}
