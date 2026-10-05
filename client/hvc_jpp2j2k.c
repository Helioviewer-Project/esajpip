/* hvc_jpp2j2k: writes the codestream that JPIP responses delivered, as a
 * JPEG 2000 codestream file a decoder opens.
 *
 *   hvc_jpp2j2k [-c codestream] -o file.j2k response [response ...]
 *
 *   -o  output file
 *   -c  codestream of the target (frame of a JPX movie), 0 by default
 *   -h  print the usage
 *
 * The responses are the bodies of the responses of one channel, in the
 * order they were received, as curl saves them:
 *
 *   curl -o frame.jpp 'http://localhost:8090/image.jp2?cnew=http&type=jpp-stream&stream=0&fsiz=1024,1024,closest'
 *   hvc_jpp2j2k -o frame.j2k frame.jpp
 *
 * The codestream must have been delivered whole for the window requested
 * (hvc_reconstruct.h): a response limited with len or layers is refused.
 * The output replaces the file named (hv_file). Exit status: 0 on success
 * (and for -h), 1 on error, 2 on usage errors. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hvc_cache.h"
#include "hvc_jpp.h"
#include "hvc_reconstruct.h"
#include "jpeg2000/hv_served.h"
#include "tools/hv_file.h"

static void usage(FILE *f) {
    fprintf(f, "usage: hvc_jpp2j2k [-c codestream] -o file.j2k response [response ...]\n");
}

static const char *reason_name(int reason) {
    switch (reason) {
    case HVC_EOR_IMAGE_DONE: return "image done";
    case HVC_EOR_WINDOW_DONE: return "window done";
    case HVC_EOR_WINDOW_CHANGE: return "window change";
    case HVC_EOR_BYTE_LIMIT_REACHED: return "byte limit reached";
    case HVC_EOR_QUALITY_LIMIT_REACHED: return "quality limit reached";
    case HVC_EOR_SESSION_LIMIT_REACHED: return "session limit reached";
    case HVC_EOR_RESPONSE_LIMIT_REACHED: return "response limit reached";
    default: return "unspecified";
    }
}

/* Adds the data-bins of one response body to the store. */
static int apply(hvc_cache *cache, const char *path) {
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    size_t size, messages = 0;
    uint8_t *body = hv_load_file(path, INT_MAX, &size);
    int status;

    if (body == NULL) {
        fprintf(stderr, "hvc_jpp2j2k: %s: %s\n", path, strerror(errno));
        return -1;
    }
    hvc_jpp_begin(&reader, body, size);
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) {
        if (!hvc_cache_apply(cache, &message)) {
            fprintf(stderr, "hvc_jpp2j2k: %s: %s\n", path, hvc_cache_error(cache));
            break;
        }
        messages++;
    }
    if (status == HVC_JPP_ERROR)
        fprintf(stderr, "hvc_jpp2j2k: %s: %s\n", path, hvc_jpp_error(&reader));
    else if (status == HVC_JPP_EOR)
        printf("%s: %zu messages, %s\n", path, messages, reason_name(hvc_jpp_reason(&reader)));
    free(body);
    return status == HVC_JPP_EOR ? 0 : -1;
}

static int write_file(const char *path, const uint8_t *bytes, size_t size) {
    char error[512];
    hv_file f;
    if (hv_file_create(&f, path, HV_FILE_KEEP_MODE, error, sizeof error) == 0) {
        if (hv_file_write(&f, bytes, size, error, sizeof error) == 0 &&
            hv_file_commit(&f, error, sizeof error) == 0)
            return 0;
        hv_file_abort(&f);
    }
    fprintf(stderr, "hvc_jpp2j2k: %s\n", error);
    return -1;
}

int main(int argc, char **argv) {
    const char *output = NULL;
    unsigned long long codestream = 0;
    char error[512], *end;
    hvc_cache cache;
    uint8_t *out = NULL;
    size_t size;
    int i, status = 1;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            errno = 0;
            codestream = strtoull(argv[++i], &end, 10);
            if (errno != 0 || *end != 0 || argv[i][0] < '0' || argv[i][0] > '9') {
                usage(stderr);
                return 2;
            }
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (output == NULL || i == argc) {
        usage(stderr);
        return 2;
    }

    hvc_cache_begin(&cache);
    for (; i < argc; i++)
        if (apply(&cache, argv[i]) != 0)
            goto done;
    size = hvc_reconstruct(&cache, codestream, NULL, 0, error, sizeof error);
    if (size == 0 || (out = malloc(size)) == NULL ||
        hvc_reconstruct(&cache, codestream, out, size, error, sizeof error) != size) {
        fprintf(stderr, "hvc_jpp2j2k: codestream %llu: %s\n", codestream,
                size != 0 && out == NULL ? "out of memory" : error);
        goto done;
    }
    if (write_file(output, out, size) != 0)
        goto done;
    printf("%s: %zu bytes\n", output, size);
    status = 0;

done:
    free(out);
    hvc_cache_release(&cache);
    return status;
}
