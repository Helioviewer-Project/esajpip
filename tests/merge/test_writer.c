/* test_writer.c: output accounting at INT_MAX and failed patching. */
#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "merge/merge.c"
#include <errno.h>
#include <unistd.h>
#include "tools/hv_file.h"

static int failures, checks;

static void check(int ok, const char *message) {
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void test_limit(void) {
    static const uint8_t reservation[44] = {0};
    char error[512] = {0};
    FILE *file = tmpfile();
    writer w = {.file = file, .written = INT_MAX - sizeof reservation,
                .error = error, .error_size = sizeof error};
    if (file == NULL) {
        check(0, "create accounting output");
        return;
    }
    hv_out_init(&w.out);
    check(write_file(&w, reservation, sizeof reservation) == 0 && w.written == INT_MAX &&
          ftell(file) == sizeof reservation, "reservation counted at INT_MAX");
    check(write_file(&w, reservation, 1) != 0 && strstr(error, "file.size-limit") != NULL &&
          w.written == INT_MAX && ftell(file) == sizeof reservation, "write-time size guard");
    w.written = INT_MAX - sizeof reservation + 1;
    check(write_file(&w, reservation, sizeof reservation) != 0 &&
          ftell(file) == sizeof reservation, "reservation cannot cross INT_MAX");
    w.written = sizeof reservation;
    check(patch(&w, 0, reservation, sizeof reservation) == 0 &&
          w.written == sizeof reservation && ftell(file) == sizeof reservation,
          "patch preserves byte count and append position");
    hv_out_free(&w.out);
    fclose(file);
}

enum { PATCH_SHORT_WRITE = 1, PATCH_FLUSH, PATCH_RESTORE };

typedef struct {
    long position, size;
    int fail_seek;
    int failure, patching, faulted;
    uint32_t patch_type;
    FILE *base;
} stream;

/* funopen's write callback reports an error with -1; fopencookie's must
 * return 0 instead, and never a negative value. */
#ifdef __APPLE__
#define WRITE_ERROR (-1)
static int stream_write(void *context, const char *bytes, int size) {
#else
#define WRITE_ERROR 0
static ssize_t stream_write(void *context, const char *bytes, size_t size) {
#endif
    stream *s = context;
    if (s->patching) {
        if (size >= 8)
            s->patch_type = (uint32_t)(unsigned char)bytes[4] << 24 |
                            (uint32_t)(unsigned char)bytes[5] << 16 |
                            (uint32_t)(unsigned char)bytes[6] << 8 | (unsigned char)bytes[7];
        if (s->failure == PATCH_SHORT_WRITE || s->failure == PATCH_FLUSH) {
            s->faulted++;
            if (s->failure == PATCH_FLUSH)
                errno = ENOSPC;
            return s->failure == PATCH_SHORT_WRITE ? 0 : WRITE_ERROR;
        }
    } else if (s->failure == PATCH_SHORT_WRITE) {
        /* A stale errno that only the merge's own reset before the patch clears. */
        errno = EDOM;
    }
    if (s->base != NULL && fwrite(bytes, 1, size, s->base) != (size_t)size)
        return WRITE_ERROR;
    s->position += size;
    if (s->position > s->size)
        s->size = s->position;
    return size;
}

#ifdef __APPLE__
static fpos_t stream_seek(void *context, fpos_t offset, int whence) {
#else
static int stream_seek(void *context, off64_t *offset, int whence) {
#endif
    stream *s = context;
#ifdef __APPLE__
    long value = offset;
#else
    long value = *offset;
#endif
    if (s->fail_seek) {
        errno = ESPIPE;
        return -1;
    }
    if (s->patching && whence == SEEK_SET && value == s->size &&
        s->failure == PATCH_RESTORE) {
        s->faulted++;
        errno = ESPIPE;
        return -1;
    }
    /* Stays set after the first patch, so a fault also reaches any later
     * patch; faulted == 1 then shows that no later patch was attempted. */
    if (whence == SEEK_SET && value < s->size)
        s->patching = 1;
    s->position = (whence == SEEK_CUR ? s->position : whence == SEEK_END ? s->size : 0) + value;
#ifdef __APPLE__
    return s->position;
#else
    *offset = s->position;
    return 0;
#endif
}

static int stream_close(void *context) {
    stream *s = context;
    return s->base != NULL ? fclose(s->base) : 0;
}

typedef struct {
    hv_merge_input input;
    stream *output;
    int opens, closes;
    int opened[2], closed[2], fail_seek;
} patch_input;

static int input_open(void *context, size_t i, hv_merge_input *in, char *error, size_t size) {
    patch_input *p = context;
    (void)error; (void)size;
    p->opens++;
    p->opened[i]++;
    *in = p->input;
    return 0;
}

static void input_close(void *context, size_t i, hv_merge_input *in) {
    patch_input *p = context;
    (void)in;
    p->closes++;
    p->closed[i]++;
    if (i == 1 && p->fail_seek)
        p->output->fail_seek = 1;
}

static void test_seek(void) {
    size_t size = 0;
    uint8_t *bytes = hv_load_file(MERGE_FIXTURES "/input/swap_000.jp2", INT_MAX, &size);
    stream s = {0};
    patch_input p = {.input = {"frame.jp2", bytes, size}, .output = &s, .fail_seek = 1};
    hv_merge_inputs inputs = {input_open, input_close, &p};
    FILE *file, *existing;
    hv_file output;
    char error[512], dir[PATH_MAX], path[PATH_MAX + 32], *temporary;
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/test_merge_seek.XXXXXX",
             tmp != NULL && tmp[0] != 0 ? tmp : "/tmp");
    if (mkdtemp(dir) == NULL) {
        check(0, "create seek failure directory");
        free(bytes);
        return;
    }
    snprintf(path, sizeof path, "%s/movie.jpx", dir);
    existing = fopen(path, "wb");
    if (existing == NULL) {
        check(0, "create existing destination");
        free(bytes);
        rmdir(dir);
        return;
    }
    fwrite("existing output", 1, 15, existing);
    fclose(existing);
    if (hv_file_create(&output, path, HV_FILE_KEEP_MODE, error, sizeof error) != 0) {
        check(0, "create temporary output");
        free(bytes);
        unlink(path);
        rmdir(dir);
        return;
    }
    s.base = output.file;
    temporary = strdup(output.tmp);
#ifdef __APPLE__
    file = funopen(&s, NULL, stream_write, stream_seek, stream_close);
#else
    cookie_io_functions_t io = {.write = stream_write, .seek = stream_seek, .close = stream_close};
    file = fopencookie(&s, "w", io);
#endif
    if (file == NULL || bytes == NULL) {
        check(0, "create seek failure fixture");
    } else {
        setvbuf(file, NULL, _IONBF, 0);
        check(hv_merge_files(&inputs, 2, 0, HV_OUTPUT_JPEG2000, 0, 0, file, error, sizeof error) != 0 &&
              strstr(error, "cannot patch the JPX file: ") != NULL &&
              strstr(error, strerror(ESPIPE)) != NULL && strstr(error, "frame.jp2") == NULL,
              "late seek failure reports the output error");
        check(s.size > 44 && p.opens == 2 && p.closes == 2,
              "late seek failure releases both inputs after writing");
    }
    if (file != NULL)
        output.file = file;
    hv_file_abort(&output);
    check(temporary != NULL && access(temporary, F_OK) != 0, "seek failure removes temporary output");
    existing = fopen(path, "rb");
    if (existing != NULL) {
        char sentinel[16] = {0};
        check(fread(sentinel, 1, sizeof sentinel, existing) == 15 &&
              memcmp(sentinel, "existing output", 15) == 0, "seek failure preserves destination");
        fclose(existing);
    } else {
        check(0, "seek failure lost destination");
    }
    free(temporary);
    free(bytes);
    unlink(path);
    rmdir(dir);
}

static void test_patch_failures(void) {
    size_t size = 0;
    uint8_t *bytes = hv_load_file(MERGE_FIXTURES "/input/swap_000.jp2", INT_MAX, &size);
    static const struct {
        int failure, links, error;
        const char *name;
    } cases[] = {
        {PATCH_SHORT_WRITE, 0, EIO, "Reader Requirements short write without errno"},
        {PATCH_FLUSH, 0, ENOSPC, "Reader Requirements flush failure"},
        {PATCH_RESTORE, 0, ESPIPE, "Reader Requirements append-position seek failure"},
        {PATCH_SHORT_WRITE, 1, EIO, "Data Reference length patch failure"},
    };
    if (bytes == NULL) {
        check(0, "load patch failure input");
        return;
    }
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        stream s = {.failure = cases[i].failure};
        patch_input p = {.input = {MERGE_FIXTURES "/input/swap_000.jp2", bytes, size}, .output = &s};
        hv_merge_inputs inputs = {input_open, input_close, &p};
        char error[512] = {0}, expected[512], message[600], buffer[4096];
        FILE *file;
#ifdef __APPLE__
        file = funopen(&s, NULL, stream_write, stream_seek, stream_close);
#else
        cookie_io_functions_t io = {.write = stream_write, .seek = stream_seek, .close = stream_close};
        file = fopencookie(&s, "w", io);
#endif
        if (file == NULL) {
            check(0, cases[i].name);
            continue;
        }
        /* Buffer the patch so its write fails when the stream is flushed. */
        check(setvbuf(file, cases[i].failure == PATCH_FLUSH ? buffer : NULL,
                      cases[i].failure == PATCH_FLUSH ? _IOFBF : _IONBF,
                      cases[i].failure == PATCH_FLUSH ? sizeof buffer : 0) == 0,
              cases[i].name);
        snprintf(expected, sizeof expected, "cannot patch the JPX file: %s", strerror(cases[i].error));
        snprintf(message, sizeof message, "%s (error)", cases[i].name);
        check(hv_merge_files(&inputs, 2, cases[i].links, HV_OUTPUT_JPEG2000, 0, 0,
                             file, error, sizeof error) == -1 && strcmp(error, expected) == 0,
              message);
        snprintf(message, sizeof message, "%s (fault and box)", cases[i].name);
        check(s.faulted == 1 && s.patch_type == (cases[i].links ? HV_BOX_DTBL : HV_BOX_RREQ),
              message);
        snprintf(message, sizeof message, "%s (open and close)", cases[i].name);
        check(p.opens == 2 && p.closes == 2 && p.opened[0] == 1 && p.closed[0] == 1 &&
              p.opened[1] == 1 && p.closed[1] == 1, message);
        fclose(file);
    }
    free(bytes);
}

static void test_counting(void) {
    size_t size = 0;
    uint8_t *bytes = hv_load_file(MERGE_FIXTURES "/input/swap_000.jp2", INT_MAX, &size);
    hv_merge_input input = {"frame.jp2", bytes, size};
    source s = {0};
    FILE *file = tmpfile();
    char error[512] = {0};
    writer w = {.file = file, .error = error, .error_size = sizeof error};
    hv_out_init(&w.out);
    if (file == NULL || bytes == NULL || read_source(&input, &s, 0, HV_OUTPUT_JPEG2000, error, sizeof error) != 0) {
        check(0, "prepare counted headers");
    } else {
        headers h = {&s, &s, 1};
        sink count = {&w, 1, 0}, output = {&w, 0, 0};
        check(emit_superbox(&count, HV_BOX_JP2H, jp2h_contents, &h) == 0 &&
              emit_superbox(&count, HV_BOX_JPCH, jpch_contents, &h) == 0 &&
              emit_superbox(&count, HV_BOX_JPLH, jplh_contents, &h) == 0 &&
              emit_superbox(&output, HV_BOX_JP2H, jp2h_contents, &h) == 0 &&
              emit_superbox(&output, HV_BOX_JPCH, jpch_contents, &h) == 0 &&
              emit_superbox(&output, HV_BOX_JPLH, jplh_contents, &h) == 0 && flush(&w) == 0 &&
              w.written == count.size && ftell(file) == (long)count.size, "counted and written headers agree");
    }
    free(s.ipr);
    free(bytes);
    hv_out_free(&w.out);
    if (file != NULL)
        fclose(file);
}

static void test_append(void) {
    size_t size = 0;
    uint8_t *bytes = hv_load_file(MERGE_FIXTURES "/input/swap_000.jp2", INT_MAX, &size);
    hv_merge_input input = {"frame.jp2", bytes, size};
    const char *tmp = getenv("TMPDIR");
    char path[PATH_MAX], error[512];
    FILE *file = NULL;
    int fd;
    snprintf(path, sizeof path, "%s/test_merge_append.XXXXXX",
             tmp != NULL && tmp[0] != 0 ? tmp : "/tmp");
    fd = mkstemp(path);
    if (fd >= 0) {
        close(fd);
        file = fopen(path, "ab");
    }
    if (file == NULL || bytes == NULL) {
        check(0, "prepare append-mode output");
    } else {
        check(hv_merge_buffers(&input, 1, 0, HV_OUTPUT_JPEG2000, 0, 0,
                               file, error, sizeof error) != 0 &&
              strstr(error, "cannot patch the JPX file: ") != NULL &&
              strstr(error, strerror(EIO)) != NULL,
              "append-mode output fails when the patch lands at the end");
    }
    if (file != NULL)
        fclose(file);
    if (fd >= 0)
        unlink(path);
    free(bytes);
}

static void test_dtbl_flush(void) {
    char error[512] = {0};
    writer w = {.error = error, .error_size = sizeof error};
    hv_out_init(&w.out);
    w.out.error = "flush failure";
    check(write_dtbl(&w, NULL, 0) != 0 && strcmp(error, "flush failure") == 0,
          "dtbl preserves the flush error");
    hv_out_free(&w.out);
}

int main(void) {
    test_limit();
    test_counting();
    test_seek();
    test_patch_failures();
    test_append();
    test_dtbl_flush();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
