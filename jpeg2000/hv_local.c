#define _XOPEN_SOURCE 700
#include "hv_local.h"
#include "hv_reader.h"
#include "hv_metadata.h"
#include "hv_error.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const uint8_t *data; size_t size; } local_file;
typedef struct { hv_box box; hv_fragments fragments; size_t size; } local_stream;
struct hv_local {
    local_file main;
    local_file *companions; /* one slot per reference; only used references mapped */
    hv_box *references;
    size_t reference_count;
    local_stream *streams;
    size_t count;
    hv_metadata metadata; /* lazy, borrowing the main mapping */
    hv_presentation presentation; /* lazy; its box bytes borrow the main mapping */
};

static int map_file(const char *path, local_file *file, char *error, size_t error_size) {
    struct stat st;
    void *data;
    int saved, fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return hv_fail(error, error_size, "%s: %s", path, strerror(errno));
    if (fstat(fd, &st) != 0) {
        saved = errno;
        close(fd);
        return hv_fail(error, error_size, "%s: %s", path, strerror(saved));
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        close(fd);
        return hv_fail(error, error_size, "%s: requires a nonempty regular file of addressable size", path);
    }
    data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    saved = errno;
    close(fd);
    if (data == MAP_FAILED)
        return hv_fail(error, error_size, "%s: %s", path, strerror(saved));
    file->data = data;
    file->size = (size_t)st.st_size;
    return 0;
}

void hv_local_close(hv_local *source) {
    if (source == NULL) return;
    hv_metadata_close(&source->metadata);
    hv_presentation_free(&source->presentation);
    for (size_t i = 0; i < source->reference_count; i++) {
        if (source->companions != NULL && source->companions[i].data != NULL)
            munmap((void *)source->companions[i].data, source->companions[i].size);
    }
    if (source->main.data != NULL)
        munmap((void *)source->main.data, source->main.size);
    free(source->companions);
    free(source->references);
    free(source->streams);
    free(source);
}

static void *reserve(void *array, size_t *capacity, size_t count, size_t size) {
    size_t n;
    void *p;
    if (count < *capacity) return array;
    n = *capacity == 0 ? 16 : *capacity <= SIZE_MAX / 2 ? *capacity * 2 : SIZE_MAX;
    if (n > SIZE_MAX / size) return NULL;
    p = realloc(array, n * size);
    if (p != NULL) *capacity = n;
    return p;
}

/* Preserve LOC during parsing; filesystem interpretation happens only here. */
static char *reference_path(const uint8_t *loc, size_t n, const char *main_path,
                            char *error, size_t error_size) {
    const uint8_t *text = loc;
    size_t length = n, directory = 0;
    const char *slash = strrchr(main_path, '/');
    char *decoded, *path;
    int status;
    if (n >= 7 && memcmp(loc, "file://", 7) == 0) {
        text += 7; length -= 7;
    } else if (memchr(loc, ':', n) != NULL) {
        hv_fail(error, error_size, "unsupported local reference scheme");
        return NULL;
    }
    if (length == 0 || memchr(text, '?', length) != NULL || memchr(text, '#', length) != NULL) {
        hv_fail(error, error_size, "unsupported local reference location");
        return NULL;
    }
    decoded = malloc(length + 1);
    if (decoded == NULL) goto oom;
    status = hv_url_path(text, length, decoded, length + 1);
    if (status != 0) {
        free(decoded);
        hv_fail(error, error_size, "url.percent-encoding");
        return NULL;
    }
    if (decoded[0] == '/') return decoded;
    if (slash != NULL) directory = (size_t)(slash - main_path) + 1;
    length = strlen(decoded);
    if (directory > SIZE_MAX - length - 1) {
        free(decoded);
        goto oom;
    }
    path = malloc(directory + length + 1);
    if (path == NULL) { free(decoded); goto oom; }
    memcpy(path, main_path, directory);
    memcpy(path + directory, decoded, length + 1);
    free(decoded);
    return path;
oom:
    hv_fail(error, error_size, "out of memory");
    return NULL;
}

static local_file *fragment_file(hv_local *source, const Fragment *fragment,
                                 const char *path, char *error, size_t error_size) {
    local_file *file;
    const uint8_t *loc;
    size_t n, at;
    char *target;
    const char *rule;
    if (fragment->dr == 0) return &source->main;
    if (fragment->dr > source->reference_count) {
        hv_fail(error, error_size, "flst.dr-range");
        return NULL;
    }
    file = &source->companions[fragment->dr - 1];
    if (file->data != NULL) return file;
    rule = hv_url_read(source->main.data, &source->references[fragment->dr - 1], &loc, &n, &at);
    if (rule != NULL) {
        hv_fail(error, error_size, "%s at %zu", rule, at);
        return NULL;
    }
    target = reference_path(loc, n, path, error, error_size);
    if (target == NULL) return NULL;
    if (map_file(target, file, error, error_size) != 0) {
        free(target);
        return NULL;
    }
    free(target);
    return file;
}

hv_local *hv_local_open(const char *path, char *error, size_t error_size) {
    hv_local *source = calloc(1, sizeof *source);
    hv_boxes boxes, references;
    hv_ftyp type;
    hv_box box, dtbl = {0};
    hv_extent *mdat = NULL;
    size_t stream_capacity = 0, mdat_capacity = 0, mdat_count = 0, at = 0;
    const char *rule;
    int status;
    if (source == NULL) goto oom;
    if (map_file(path, &source->main, error, error_size) != 0) goto fail;
    rule = hv_container_open(source->main.data, source->main.size, &boxes, &type, &at);
    if (rule != NULL) goto invalid;
    if (!hv_ftyp_is_jpx(&type)) {
        rule = hv_read_jp2(source->main.data, source->main.size, &box, &at);
        if (rule != NULL) goto invalid;
        source->streams = calloc(1, sizeof *source->streams);
        if (source->streams == NULL) goto oom;
        source->streams[0].box = box;
        source->count = 1;
    } else {
        while ((status = hv_boxes_next(&boxes, &box, &rule, &at)) == 1) {
            at = box.start;
            if (box.type == HV_BOX_J2CX) {
                rule = "unsupported extended codestream boxes";
                goto invalid;
            }
            if (box.type == HV_BOX_JP2C || box.type == HV_BOX_FTBL) {
                local_stream *grown = reserve(source->streams, &stream_capacity, source->count,
                                              sizeof *source->streams);
                if (grown == NULL) goto oom;
                source->streams = grown;
                memset(&source->streams[source->count], 0, sizeof *source->streams);
                source->streams[source->count++].box = box;
            } else if (box.type == HV_BOX_DTBL) {
                if (dtbl.type != 0) { rule = "jpx.one-dtbl"; goto invalid; }
                dtbl = box;
            } else if (box.type == HV_BOX_MDAT) {
                hv_extent *grown = reserve(mdat, &mdat_capacity, mdat_count, sizeof *mdat);
                if (grown == NULL) goto oom;
                mdat = grown;
                mdat[mdat_count].start = box.payload;
                mdat[mdat_count++].end = box.end;
            }
        }
        if (status < 0) goto invalid;
    }
    if (dtbl.type != 0) {
        rule = hv_references_open(source->main.data, &dtbl, &references, &source->reference_count, &at);
        if (rule != NULL) goto invalid;
        if (source->reference_count > 0) {
            source->references = calloc(source->reference_count, sizeof *source->references);
            source->companions = calloc(source->reference_count, sizeof *source->companions);
            if (source->references == NULL || source->companions == NULL) goto oom;
            for (size_t i = 0; i < source->reference_count; i++)
                hv_boxes_next(&references, &source->references[i], &rule, &at);
        }
    }
    if (source->count == 0) { rule = "no supported codestreams"; at = source->main.size; goto invalid; }
    for (size_t i = 0; i < source->count; i++) {
        local_stream *stream = &source->streams[i];
        if (stream->box.type == HV_BOX_JP2C) {
            stream->size = stream->box.end - stream->box.payload;
        } else {
            rule = hv_fragments_open(source->main.data, &stream->box, &stream->fragments, &at);
            if (rule != NULL) goto invalid;
            for (size_t j = 0; j < stream->fragments.count; j++) {
                Fragment fragment;
                local_file *file;
                at = stream->fragments.start + j * HV_FIXED(Fragment);
                rule = hv_fragment_read(&stream->fragments, j, &fragment, &at);
                if (rule != NULL) goto invalid;
                file = fragment_file(source, &fragment, path, error, error_size);
                if (file == NULL) goto fail;
                if (fragment.off > file->size || fragment.len > file->size - fragment.off) {
                    rule = "fragment exceeds referenced file";
                    goto invalid;
                }
                if (fragment.dr == 0) {
                    rule = hv_rule_fragment_here(mdat, mdat_count, file->data,
                                                  fragment.off, fragment.len, j == 0);
                    if (rule != NULL) goto invalid;
                }
                if (fragment.len > SIZE_MAX - stream->size) { rule = "codestream size overflow"; goto invalid; }
                stream->size += (size_t)fragment.len;
            }
        }
        if (stream->size == 0) { rule = "empty codestream"; at = stream->box.start; goto invalid; }
    }
    free(mdat);
    return source;
invalid:
    hv_fail(error, error_size, "%s: %s at %zu", path, rule, at);
    goto fail;
oom:
    hv_fail(error, error_size, "out of memory");
fail:
    free(mdat);
    hv_local_close(source);
    return NULL;
}

size_t hv_local_codestreams(const hv_local *source) { return source->count; }

int hv_local_presentation(hv_local *source, const uint8_t **data,
                           const hv_presentation **presentation,
                           char *error, size_t error_size) {
    *data = NULL;
    *presentation = NULL;
    if (source->presentation.layer == NULL) {
        size_t at;
        const char *rule = hv_presentation_open(source->main.data, source->main.size,
                                                &source->presentation, &at);
        if (rule != NULL) return hv_fail(error, error_size, "%s at %zu", rule, at);
    }
    *data = source->main.data;
    *presentation = &source->presentation;
    return 0;
}

static int copy_range(const hv_local *source, const local_stream *stream,
                       size_t offset, uint8_t *out, size_t size, char *error, size_t error_size) {
    size_t written = 0;
    if (stream->box.type == HV_BOX_JP2C) {
        memcpy(out, source->main.data + stream->box.payload + offset, size);
        return 0;
    }
    for (size_t i = 0; i < stream->fragments.count && written < size; i++) {
        Fragment fragment;
        size_t at, length;
        const local_file *file;
        const char *rule = hv_fragment_read(&stream->fragments, i, &fragment, &at);
        if (rule != NULL) return hv_fail(error, error_size, "%s at %zu", rule, at);
        if (offset >= fragment.len) { offset -= (size_t)fragment.len; continue; }
        file = fragment.dr == 0 ? &source->main : &source->companions[fragment.dr - 1];
        length = fragment.len - offset < size - written ? (size_t)fragment.len - offset : size - written;
        memcpy(out + written, file->data + (size_t)fragment.off + offset, length);
        written += length;
        offset = 0;
    }
    return 0;
}

size_t hv_local_copy(const hv_local *source, size_t codestream, uint8_t *out,
                     size_t capacity, char *error, size_t error_size) {
    const local_stream *stream;
    if (codestream >= source->count) {
        hv_fail(error, error_size, "codestream index out of range");
        return 0;
    }
    stream = &source->streams[codestream];
    if (out == NULL) return stream->size;
    if (capacity < stream->size) {
        hv_fail(error, error_size, "codestream buffer too small");
        return 0;
    }
    if (copy_range(source, stream, 0, out, stream->size, error, error_size) != 0) return 0;
    return stream->size;
}

size_t hv_local_read(const hv_local *source, size_t codestream, size_t offset,
                     uint8_t *out, size_t capacity, char *error, size_t error_size) {
    const local_stream *stream;
    size_t size;
    if (codestream >= source->count) {
        hv_fail(error, error_size, "codestream index out of range");
        return 0;
    }
    stream = &source->streams[codestream];
    if (offset > stream->size || (capacity != 0 && out == NULL)) {
        hv_fail(error, error_size, "invalid codestream read");
        return 0;
    }
    size = stream->size - offset;
    if (size > capacity) size = capacity;
    if (size == 0) return 0;
    return copy_range(source, stream, offset, out, size, error, error_size) == 0 ? size : 0;
}

int hv_local_siz(const hv_local *source, size_t codestream, SizFixed *fixed,
                  Component *components, size_t capacity, char *error, size_t error_size) {
    uint8_t prefix[6], *header = NULL;
    const uint8_t *data;
    size_t length, at = 0;
    const local_stream *stream;
    const char *rule;
    if (codestream >= source->count)
        return hv_fail(error, error_size, "codestream index out of range");
    stream = &source->streams[codestream];
    length = stream->size;
    if (stream->box.type == HV_BOX_JP2C)
        data = source->main.data + stream->box.payload;
    else {
        if (length < sizeof prefix) return hv_fail(error, error_size, "truncated SOC/SIZ");
        if (copy_range(source, stream, 0, prefix, sizeof prefix, error, error_size) != 0) return -1;
        if (prefix[0] != 0xff || prefix[1] != 0x4f || prefix[2] != 0xff || prefix[3] != 0x51)
            return hv_fail(error, error_size, "expected SOC followed by SIZ");
        length = 4 + ((size_t)prefix[4] << 8) + prefix[5];
        if (length > stream->size) return hv_fail(error, error_size, "truncated SIZ");
        header = malloc(length);
        if (header == NULL) return hv_fail(error, error_size, "out of memory");
        if (copy_range(source, stream, 0, header, length, error, error_size) != 0) {
            free(header);
            return -1;
        }
        data = header;
    }
    rule = hv_read_siz(data, length, fixed, components, capacity, &at);
    free(header);
    return rule == NULL ? 0 : hv_fail(error, error_size, "%s at %zu", rule, at);
}

static int local_metadata(hv_local *source, size_t layers, char *error, size_t error_size) {
    if (source->metadata.frames != NULL && source->metadata.layer_count >= layers) return 0;
    hv_metadata_close(&source->metadata);
    return hv_metadata_read(source->main.data, source->main.size, source->count, layers,
                            NULL, NULL, &source->metadata, error, error_size);
}

int hv_local_xml(hv_local *source, size_t codestream, const uint8_t **xml,
                 size_t *size, char *error, size_t error_size) {
    *xml = NULL;
    *size = 0;
    if (local_metadata(source, 0, error, error_size) != 0) return -1;
    return hv_metadata_codestream_xml(&source->metadata, codestream, xml, size, error, error_size);
}

int hv_local_layer_xml(hv_local *source, size_t layer, const uint8_t **xml,
                       size_t *size, char *error, size_t error_size) {
    const uint8_t *data;
    const hv_presentation *view;
    *xml = NULL;
    *size = 0;
    if (hv_local_presentation(source, &data, &view, error, error_size) != 0) return -1;
    if (layer >= view->layers) return hv_fail(error, error_size, "layer index out of range");
    if (local_metadata(source, view->layers, error, error_size) != 0) return -1;
    return hv_metadata_layer_xml(&source->metadata, layer, &view->layer[layer].registration,
                                  xml, size, error, error_size);
}

int hv_local_palette(hv_local *source, size_t codestream, int *channels,
                     uint8_t *table, size_t capacity, char *error, size_t error_size) {
    *channels = 0;
    if (local_metadata(source, 0, error, error_size) != 0) return -1;
    return hv_metadata_palette(&source->metadata, codestream, channels, table, capacity, error, error_size);
}
