/* Pixel conversion on lossless codestreams with known sample values. */
#include "hvc_openjpeg.h"
#define OPJ_SKIP_POISON
#include "opj_includes.h"
#include "t1_cache.h"
#include "decoder_header.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t at, size, capacity;
} output;

static OPJ_SIZE_T write_bytes(void *bytes, OPJ_SIZE_T size, void *user) {
    output *out = user;
    if (size > out->capacity - out->at) return (OPJ_SIZE_T)-1;
    memcpy(out->data + out->at, bytes, size);
    out->at += size;
    if (out->at > out->size) out->size = out->at;
    return size;
}

static OPJ_BOOL seek_bytes(OPJ_OFF_T at, void *user) {
    output *out = user;
    if (at < 0 || (uint64_t)at > out->capacity) return OPJ_FALSE;
    out->at = (size_t)at;
    return OPJ_TRUE;
}

static OPJ_OFF_T skip_bytes(OPJ_OFF_T count, void *user) {
    output *out = user;
    if (count < 0 || (uint64_t)count > out->capacity - out->at) return -1;
    out->at += (size_t)count;
    return count;
}

static void check(int ok, const char *why) {
    if (!ok) {
        fprintf(stderr, "%s\n", why);
        exit(1);
    }
}

/* Context outputs are borrowed; keep fresh-decoder references separately. */
static void verify_reuse(const output *encoded, const uint8_t *full, const hv_render *render, size_t components) {
    for (int threads = 0; threads <= 4; threads += 4) {
        if (threads && !opj_has_thread_support()) continue;
        hvc_decoder *decoder = hvc_decoder_create(threads);
        check(decoder != NULL, "context allocation");
        hvc_image image, reference;
        char error[256] = {0};
        for (int reduce = 2; reduce >= 0; --reduce) {
            check(hvc_openjpeg_decode(encoded->data, encoded->size, reduce,
                render, &reference, error, sizeof error) == 0, error);
            for (int repeat = 0; repeat < 2; ++repeat) {
                check(hvc_openjpeg_decode_context(decoder, encoded->data, encoded->size,
                    reduce, render, &image, error, sizeof error) == 0, error);
                check(image.width == reference.width && image.height == reference.height &&
                    memcmp(image.pixels, reference.pixels, image.width * image.height * components) == 0,
                    "context pixels differ from fresh decoding");
            }
            free(reference.pixels);
        }
        check(hvc_decoder_codeblock_stat(decoder, 1) > 0, "codeblock reuse did not run");
        check(hvc_decoder_codeblock_stat(decoder, 0) <= 8u * 1024 * 1024, "codeblock budget");
        check(hvc_decoder_retained(decoder) <= 72u * 1024 * 1024 + 768, "workspace budget");
        const uint32_t region[4] = {1, 3, 11, 13};
        check(hvc_openjpeg_decode_region(decoder, encoded->data, encoded->size, 0,
            render, region, &image, error, sizeof error) == 0, error);
        check(image.width == 10 && image.height == 10 && image.x == 1 && image.y == 3 &&
            image.grid_width == 16 && image.grid_height == 16, "crop geometry");
        for (unsigned y = 0; y < image.height; ++y)
            check(memcmp(image.pixels + y * image.width * components, full + ((y + 3) * 16 + 1) * components, 10 * components) == 0,
                "crop pixels differ from whole image");
        const uint32_t bad[4] = {0, 0, 17, 16};
        check(hvc_openjpeg_decode_region(decoder, encoded->data, encoded->size, 0,
            render, bad, &image, error, sizeof error) == -1, "invalid crop accepted");
        check(hvc_openjpeg_decode_context(decoder, encoded->data, 3, 0,
            render, &image, error, sizeof error) == -1, "truncated input accepted");
        check(hvc_openjpeg_decode_context(decoder, encoded->data, encoded->size, 0,
            render, &image, error, sizeof error) == 0, error);
        check(image.width == 16 && image.height == 16 && memcmp(image.pixels, full, 256 * components) == 0,
            "whole-frame recovery retained region or damaged pixels");
        hvc_decoder_destroy(decoder);
    }
}

static OPJ_SIZE_T read_bytes(void *bytes, OPJ_SIZE_T size, void *user) {
    output *in = user;
    if (in->at == in->size) return (OPJ_SIZE_T)-1;
    if (size > in->size - in->at) size = in->size - in->at;
    memcpy(bytes, in->data + in->at, size);
    in->at += size;
    return size;
}

static void verify_header(const output *encoded, OPJ_UINT32 components, const OPJ_UINT32 *expected_resolutions) {
    output in = *encoded;
    in.at = 0;
    opj_codec_t *codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_stream_t *stream = opj_stream_create(8192, OPJ_TRUE);
    opj_image_t *image = NULL;
    opj_dparameters_t parameters;
    check(codec != NULL && stream != NULL, "header decoder allocation");
    opj_set_default_decoder_parameters(&parameters);
    opj_stream_set_user_data(stream, &in, NULL);
    opj_stream_set_user_data_length(stream, in.size);
    opj_stream_set_read_function(stream, read_bytes);
    opj_stream_set_seek_function(stream, seek_bytes);
    opj_stream_set_skip_function(stream, skip_bytes);
    check(opj_setup_decoder(codec, &parameters) && opj_read_header(stream, codec, &image), "read encoded header");
    for (OPJ_UINT32 c = 0; c < components; c++) {
        OPJ_UINT32 resolutions = 0, layers = 0;
        check(opj_decoder_get_coding_parameters(codec, c, &resolutions, &layers) &&
              resolutions == (expected_resolutions ? expected_resolutions[c] : 3) && layers == 1,
              "main-header coding parameters differ from COD/COC settings");
    }
    OPJ_UINT32 resolutions = 123, layers = 456;
    check(!opj_decoder_get_coding_parameters(codec, components, &resolutions, &layers) &&
          resolutions == 123 && layers == 456, "missing header component changed outputs");
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
}

static void encode(opj_image_t *input, output *out, int irreversible) {
    opj_codec_t *codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_stream_t *stream = opj_stream_create(8192, OPJ_FALSE);
    check(codec != NULL && stream != NULL, "encoder allocation");
    opj_cparameters_t parameters;
    opj_set_default_encoder_parameters(&parameters);
    parameters.numresolution = 3;
    parameters.tcp_numlayers = 1;
    parameters.cp_disto_alloc = 1;
    parameters.irreversible = irreversible;
    opj_stream_set_user_data(stream, out, NULL);
    opj_stream_set_write_function(stream, write_bytes);
    opj_stream_set_seek_function(stream, seek_bytes);
    opj_stream_set_skip_function(stream, skip_bytes);
    check(opj_setup_encoder(codec, &parameters, input) && opj_start_compress(codec, input, stream) &&
          opj_encode(codec, stream) && opj_end_compress(codec, stream), "encode known samples");
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    verify_header(out, input->numcomps, NULL);
}

static void verify(unsigned bits, int signed_samples) {
    uint8_t bytes[8192];
    output out = {bytes, 0, 0, sizeof bytes};
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

    encode(input, &out, 0);
    opj_image_destroy(input);

    hvc_image image;
    char error[256];
    check(hvc_openjpeg_decode(out.data, out.size, 0, NULL, &image, error, sizeof error) == 0,
          error);
    check(image.width == 16 && image.height == 16 && image.components == 1, "decoded dimensions");
    for (unsigned i = 0; i < 256; i++) {
        unsigned value = i * max / 255;
        unsigned expected = bits > 8 ? value >> (bits - 8) : value << (8 - bits);
        check(image.pixels[i] == expected, "sample scaling differs");
    }
    verify_reuse(&out, image.pixels, NULL, 1);
    free(image.pixels);

    hv_render render = {.channel_count = 1, .channel = {{0, 0}}};
    int result = hvc_openjpeg_decode(out.data, out.size, 0, &render, &image, error, sizeof error);
    if (signed_samples || bits > 8) {
        check(result == -1 && image.pixels == NULL && strstr(error, "palette indices") != NULL,
              "unsupported palette index representation accepted");
    } else {
        check(result == 0, error);
        for (unsigned i = 0; i < 256; i++)
            check(image.pixels[i] == i * max / 255, "palette index changed");
        verify_reuse(&out, image.pixels, &render, 1);
        free(image.pixels);
    }
}

static void verify_channels(void) {
    uint8_t bytes[8192];
    output out = {bytes, 0, 0, sizeof bytes};
    opj_image_cmptparm_t components[3] = {{0}};
    for (int c = 0; c < 3; c++) {
        components[c].dx = components[c].dy = 1;
        components[c].w = components[c].h = 16;
        components[c].prec = 8;
    }
    opj_image_t *input = opj_image_create(3, components, OPJ_CLRSPC_SRGB);
    check(input != NULL, "color image allocation");
    input->x1 = input->y1 = 16;
    for (int c = 0; c < 3; c++)
        for (unsigned i = 0; i < 256; i++)
            input->comps[c].data[i] = (i + 37 * c) % 256;
    encode(input, &out, 0);
    opj_image_destroy(input);

    /* A main-header COC overrides COD for component 1 only. No pixel decode
     * of this header-only variant: its original packets still encode 3 levels. */
    uint8_t coc_bytes[sizeof bytes + 11];
    const uint8_t coc[] = {0xff, 0x53, 0, 9, 1, 0, 1, 4, 4, 0, 1};
    size_t sot = 0;
    while (sot + 1 < out.size && !(bytes[sot] == 0xff && bytes[sot + 1] == 0x90)) sot++;
    check(sot + 1 < out.size, "encoded SOT marker");
    memcpy(coc_bytes, bytes, sot);
    memcpy(coc_bytes + sot, coc, sizeof coc);
    memcpy(coc_bytes + sot + sizeof coc, bytes + sot, out.size - sot);
    output with_coc = {coc_bytes, 0, out.size + sizeof coc, sizeof coc_bytes};
    const OPJ_UINT32 resolutions[] = {3, 2, 3};
    verify_header(&with_coc, 3, resolutions);

    hvc_image image;
    char error[256];
    hv_render render = {.channel_count = 3, .channel = {{2, -1}, {0, -1}, {1, -1}}};
    check(hvc_openjpeg_decode(out.data, out.size, 0, &render, &image, error, sizeof error) == 0, error);
    check(image.components == 3, "mapped RGB channel count");
    for (unsigned i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++)
            check(image.pixels[i * 3 + c] == (i + 37 * render.channel[c].component) % 256,
                  "display channels do not follow their selected components");
    verify_reuse(&out, image.pixels, &render, 3);
    free(image.pixels);

    render.channel_count = 1;
    for (int column = -1; column <= 0; column++) {
        render.channel[0].palette_column = column;
        check(hvc_openjpeg_decode(out.data, out.size, 0, &render, &image, error, sizeof error) == 0, error);
        check(image.components == 1, "selected gray/index plane count");
        for (unsigned i = 0; i < 256; i++)
            check(image.pixels[i] == (i + 74) % 256, "wrong gray/index component");
        verify_reuse(&out, image.pixels, &render, 1);
        free(image.pixels);
    }
    render.channel[0].component = 3;
    check(hvc_openjpeg_decode(out.data, out.size, 0, &render, &image, error, sizeof error) == -1 &&
          image.pixels == NULL && strstr(error, "does not exist"), "missing selected component accepted");
}

static void verify_codeblock_cache(void) {
    opj_t1_cache *cache = opj_t1_cache_create();
    check(cache != NULL, "codeblock cache allocation");
    OPJ_BYTE bytes[] = {0x55, 0x66};
    opj_tcd_seg_data_chunk_t chunk = {bytes, sizeof bytes};
    opj_tcd_cblk_dec_t block = {0};
    block.x1 = block.y1 = 2;
    block.numbps = 1;
    block.Mb = 1;
    block.numchunks = 1;
    block.chunks = &chunk;
    OPJ_INT32 pixels[] = {11, 22, 33, 44};
    check(opj_t1_cache_transfer(cache, &block, 0, 0, J2K_CCP_CBLKSTY_HT, pixels, OPJ_TRUE), "cache insert");
    block.Mb = 31;
    memset(pixels, 0, sizeof pixels);
    check(!opj_t1_cache_transfer(cache, &block, 0, 0, J2K_CCP_CBLKSTY_HT, pixels, OPJ_FALSE) && pixels[0] == 0,
          "cache hit bypassed HT bit-plane validation");
    block.Mb = 1;
    check(opj_t1_cache_transfer(cache, &block, 0, 0, J2K_CCP_CBLKSTY_HT, pixels, OPJ_FALSE) &&
          pixels[0] == 11 && pixels[3] == 44, "unchanged codeblock was not reused");
    opj_t1_cache_destroy(cache);
}

/* Wide images and odd origins must have the same geometry and pixels in
 * whole-frame and region decodes, across reductions and worker counts. */
static void verify_wide_rows(void) {
    enum { width = 33000, height = 20 };
    output out = {malloc(1 << 20), 0, 0, 1 << 20};
    check(out.data != NULL, "wide output allocation");
    for (unsigned x0 = 0; x0 <= 2; x0++) {
        opj_image_cmptparm_t component = {0};
        component.dx = component.dy = 1;
        component.x0 = x0;
        component.w = width + x0;
        component.h = height;
        component.prec = 8;
        opj_image_t *input = opj_image_create(1, &component, OPJ_CLRSPC_GRAY);
        check(input != NULL, "wide image allocation");
        input->x0 = x0;
        input->x1 = x0 + component.w;
        input->y1 = height;
        uint32_t seed = 1;
        for (unsigned i = 0; i < component.w * height; i++) {
            seed = seed * 1103515245u + 12345u;
            input->comps[0].data[i] = (int)((i % component.w) / 64 + (seed >> 26)) & 255;
        }
        out.at = out.size = 0;
        encode(input, &out, 1);
        opj_image_destroy(input);

        /* Whole rows, so that both row ends are compared. */
        const uint32_t region[4] = {0, 4, component.w, height - 4};
        for (int threads = 0; threads <= 4; threads += 4) {
            if (threads && !opj_has_thread_support()) continue;
            /* Context outputs are borrowed, so each decode gets its own. */
            hvc_decoder *whole = hvc_decoder_create(threads), *decoder = hvc_decoder_create(threads);
            check(whole != NULL && decoder != NULL, "wide decoder allocation");
            char error[256] = {0};
            for (int reduce = 0; reduce <= 2; reduce++) {
                hvc_image full, part;
                check(hvc_openjpeg_decode_context(whole, out.data, out.size, reduce, NULL, &full,
                    error, sizeof error) == 0, error);
                check(hvc_openjpeg_decode_region(decoder, out.data, out.size, reduce, NULL,
                    region, &part, error, sizeof error) == 0, error);
                check(part.x + part.width <= full.width && part.y + part.height <= full.height,
                    "wide region geometry");
                for (unsigned y = 0; y < part.height; y++)
                    check(memcmp(part.pixels + y * part.width,
                        full.pixels + (part.y + y) * full.width + part.x, part.width) == 0,
                        "wide rows differ from the region decode");
            }
            hvc_decoder_destroy(whole);
            hvc_decoder_destroy(decoder);
        }
    }
    free(out.data);
}

/* A real 4096x4096 9/7 frame, where the Tier-1 cache budget, the strip count
 * of the vertical pass and the worker count matter: every worker count and
 * every repeat, with cache hits, must give the pixels of a fresh decode. */
static void verify_large_frame(void) {
    FILE *file = fopen(AIA, "rb");
    check(file != NULL, "open " AIA);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)size);
    check(bytes != NULL && fread(bytes, 1, (size_t)size, file) == (size_t)size, "read " AIA);
    fclose(file);
    /* The codestream box of the JP2 file. */
    long at = 0;
    while (at + 8 <= size && memcmp(bytes + at + 4, "jp2c", 4) != 0)
        at += (long)((uint32_t)bytes[at] << 24 | bytes[at + 1] << 16 | bytes[at + 2] << 8 | bytes[at + 3]);
    check(at + 8 <= size, "codestream box");
    const uint8_t *codestream = bytes + at + 8;
    size_t length = (size_t)(size - at - 8);

    char error[256] = {0};
    hvc_image reference[2];
    for (int reduce = 0; reduce <= 1; reduce++)
        check(hvc_openjpeg_decode(codestream, length, reduce, NULL, &reference[reduce],
            error, sizeof error) == 0, error);
    for (int threads = 0; threads <= 8; threads += 4) {
        if (threads && !opj_has_thread_support()) continue;
        hvc_decoder *decoder = hvc_decoder_create(threads);
        check(decoder != NULL, "large decoder allocation");
        static const int reduces[] = {1, 0, 0, 1};
        for (int i = 0; i < 4; i++) {
            const hvc_image *expected = &reference[reduces[i]];
            hvc_image image;
            check(hvc_openjpeg_decode_context(decoder, codestream, length, reduces[i], NULL, &image,
                error, sizeof error) == 0, error);
            check(image.width == expected->width && image.height == expected->height &&
                memcmp(image.pixels, expected->pixels, (size_t)image.width * image.height) == 0,
                "large frame differs between worker counts or with cached blocks");
        }
        check(hvc_decoder_codeblock_stat(decoder, 1) > 0, "large frame reused no blocks");
        hvc_decoder_destroy(decoder);
    }
    free(reference[0].pixels);
    free(reference[1].pixels);
    free(bytes);
}

int main(void) {
    verify_codeblock_cache();
    verify(1, 0);
    verify(4, 0);
    verify(8, 0);
    verify(10, 0);
    verify(8, 1);
    verify_channels();
    verify_wide_rows();
    verify_large_frame();
    return 0;
}
