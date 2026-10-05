/* Pixel conversion on lossless codestreams with known sample values. */
#include "hvc_openjpeg.h"
#include "openjpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[8192];
    size_t at, size;
} output;

static OPJ_SIZE_T write_bytes(void *bytes, OPJ_SIZE_T size, void *user) {
    output *out = user;
    if (size > sizeof out->data - out->at) return (OPJ_SIZE_T)-1;
    memcpy(out->data + out->at, bytes, size);
    out->at += size;
    if (out->at > out->size) out->size = out->at;
    return size;
}

static OPJ_BOOL seek_bytes(OPJ_OFF_T at, void *user) {
    output *out = user;
    if (at < 0 || (uint64_t)at > sizeof out->data) return OPJ_FALSE;
    out->at = (size_t)at;
    return OPJ_TRUE;
}

static OPJ_OFF_T skip_bytes(OPJ_OFF_T count, void *user) {
    output *out = user;
    if (count < 0 || (uint64_t)count > sizeof out->data - out->at) return -1;
    out->at += (size_t)count;
    return count;
}

static void check(int ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}

static void verify(unsigned bits, int signed_samples) {
    output out = {{0}, 0, 0};
    opj_image_cmptparm_t component = {0};
    component.dx = component.dy = 1;
    component.w = component.h = 16;
    component.prec = bits;
    component.sgnd = (OPJ_UINT32)signed_samples;
    opj_image_t *input = opj_image_create(1, &component, OPJ_CLRSPC_GRAY);
    check(input != NULL, "image allocation");
    input->x1 = input->y1 = 16;
    unsigned max = (1u << bits) - 1;
    for (unsigned i = 0; i < 256; i++)
        input->comps[0].data[i] = (int)(i * max / 255) - (signed_samples ? (1 << (bits - 1)) : 0);

    opj_codec_t *codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_stream_t *stream = opj_stream_create(8192, OPJ_FALSE);
    check(codec != NULL && stream != NULL, "encoder allocation");
    opj_cparameters_t parameters;
    opj_set_default_encoder_parameters(&parameters);
    parameters.numresolution = 1;
    parameters.tcp_numlayers = 1;
    parameters.cp_disto_alloc = 1;
    opj_stream_set_user_data(stream, &out, NULL);
    opj_stream_set_write_function(stream, write_bytes);
    opj_stream_set_seek_function(stream, seek_bytes);
    opj_stream_set_skip_function(stream, skip_bytes);
    check(opj_setup_encoder(codec, &parameters, input) && opj_start_compress(codec, input, stream) &&
          opj_encode(codec, stream) && opj_end_compress(codec, stream), "encode known samples");
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    opj_image_destroy(input);

    hvc_image image;
    char error[256];
    check(hvc_openjpeg_decode(out.data, out.size, 0, HVC_IMAGE_SAMPLES, &image, error, sizeof error) == 0,
          error);
    check(image.width == 16 && image.height == 16 && image.components == 1, "decoded dimensions");
    for (unsigned i = 0; i < 256; i++) {
        unsigned value = i * max / 255;
        unsigned expected = bits > 8 ? value >> (bits - 8) : value << (8 - bits);
        check(image.pixels[i] == expected, "sample scaling differs");
    }
    free(image.pixels);

    int result = hvc_openjpeg_decode(out.data, out.size, 0, HVC_IMAGE_INDICES, &image, error, sizeof error);
    if (signed_samples || bits > 8) {
        check(result == -1 && image.pixels == NULL && strstr(error, "palette indices") != NULL,
              "unsupported palette index representation accepted");
    } else {
        check(result == 0, error);
        for (unsigned i = 0; i < 256; i++)
            check(image.pixels[i] == i * max / 255, "palette index changed");
        free(image.pixels);
    }
}

int main(void) {
    verify(1, 0);
    verify(4, 0);
    verify(8, 0);
    verify(10, 0);
    verify(8, 1);
    return 0;
}
