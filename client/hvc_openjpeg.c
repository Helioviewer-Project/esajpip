/* hvc_openjpeg.c: see hvc_openjpeg.h. */
#include "hvc_openjpeg.h"

#include <stdlib.h>
#include <string.h>

#include "jpeg2000/hv_error.h"
#include "openjpeg.h"

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

/* One sample as 8 bits: unsigned, and its most significant bits. */
static uint8_t sample(const opj_image_comp_t *component, size_t i, hvc_image_mode mode) {
    int64_t value = component->data[i];
    int64_t max = ((int64_t)1 << component->prec) - 1;
    if (component->sgnd)
        value += (int64_t)1 << (component->prec - 1);
    value = value < 0 ? 0 : value > max ? max : value;
    if (mode == HVC_IMAGE_INDICES)
        return (uint8_t)value;
    return (uint8_t)(component->prec > 8 ? value >> (component->prec - 8)
                                         : value << (8 - component->prec));
}

int hvc_openjpeg_decode(const uint8_t *codestream, size_t size, int reduce, hvc_image_mode mode, hvc_image *out,
                        char *error, size_t error_size) {
    memory input = {codestream, size, 0};
    message failure = {error, error_size, 0};
    const char *why = "out of memory";
    opj_dparameters_t parameters;
    opj_stream_t *stream = opj_stream_create(1 << 16, OPJ_TRUE);
    opj_codec_t *codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_codestream_info_v2_t *info = NULL;
    opj_image_t *image = NULL;
    size_t count, i;
    int status = -1, c;

    memset(out, 0, sizeof *out);
    if (stream == NULL || codec == NULL)
        goto done;
    opj_stream_set_read_function(stream, read_memory);
    opj_stream_set_skip_function(stream, skip_memory);
    opj_stream_set_seek_function(stream, seek_memory);
    opj_stream_set_user_data(stream, &input, NULL);
    opj_stream_set_user_data_length(stream, size);
    opj_set_error_handler(codec, keep_error, &failure);
    opj_set_default_decoder_parameters(&parameters);

    why = "cannot decode the codestream";
    if (!opj_setup_decoder(codec, &parameters) || !opj_read_header(stream, codec, &image) ||
        (info = opj_get_cstr_info(codec)) == NULL)
        goto done;
    if (mode == HVC_IMAGE_INDICES && (image->numcomps != 1 || image->comps[0].sgnd ||
                                   image->comps[0].prec > 8)) {
        why = "palette indices require one unsigned component of at most 8 bits";
        goto done;
    }
    out->resolutions = (int)info->m_default_tile_info.tccp_info[0].numresolutions;
    out->full_width = image->x1 - image->x0;
    out->full_height = image->y1 - image->y0;
    if (reduce < 0)
        reduce = 0;
    if (reduce > out->resolutions - 1)
        reduce = out->resolutions - 1;
    if (!opj_set_decoded_resolution_factor(codec, (OPJ_UINT32)reduce) ||
        !opj_decode(codec, stream, image) || !opj_end_decompress(codec, stream))
        goto done;

    out->components = image->numcomps >= 3 ? 3 : 1;
    out->width = image->comps[0].w;
    out->height = image->comps[0].h;
    for (c = 0; c < out->components; c++) {
        const opj_image_comp_t *component = &image->comps[c];
        if (component->data == NULL || component->w != out->width ||
            component->h != out->height || component->prec < 1 || component->prec > 31) {
            why = "unsupported components";
            goto done;
        }
    }
    count = (size_t)out->width * out->height;
    why = "out of memory";
    if (count > SIZE_MAX / 3 || (out->pixels = malloc(count * (size_t)out->components)) == NULL)
        goto done;
    for (c = 0; c < out->components; c++)
        for (i = 0; i < count; i++)
            out->pixels[i * (size_t)out->components + (size_t)c] = sample(&image->comps[c], i, mode);
    status = 0;

done:
    if (status != 0 && !failure.reported)
        hv_fail(error, error_size, "%s", why);
    if (info != NULL)
        opj_destroy_cstr_info(&info);
    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return status;
}
