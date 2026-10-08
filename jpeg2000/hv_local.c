#define _XOPEN_SOURCE 700
#include "hv_local.h"
#include "hv_reader.h"
#include "hv_metadata.h"
#include "hv_error.h"
#include "hv_decode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define local_seek _fseeki64
#define local_tell _ftelli64
#define local_lock _lock_file
#define local_unlock _unlock_file
#else
#include <unistd.h>
#define local_seek fseeko
#define local_tell ftello
#define local_lock flockfile
#define local_unlock funlockfile
#endif

HV_DEFINE_DECODE(BoxHeader)

typedef struct { char *path; size_t size; } local_file;
typedef struct { hv_box box; hv_fragments fragments; size_t size; } local_stream;
struct hv_local {
    local_file main;
    FILE *file; /* Only the main JPX file stays open between operations. */
    uint8_t *headers;
    size_t header_size;
    local_file *companions;
    hv_box *references;
    size_t reference_count;
    local_stream *streams;
    size_t count;
    hv_metadata metadata;
    hv_presentation presentation;
};
struct hv_local_input {
    const hv_local *source;
    const local_stream *stream;
    FILE *file; /* Owns the JP2 or current external fragment file. */
    size_t reference;
};

static FILE *open_file(const char *path, size_t *size, char *error, size_t error_size) {
    FILE *file = NULL;
#ifdef _WIN32
    struct _stat64 st;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n) { hv_fail(error, error_size, "invalid UTF-8 path"); return NULL; }
    wchar_t *wide = malloc((size_t)n * sizeof *wide);
    if (!wide) { hv_fail(error, error_size, "out of memory"); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n);
    file = _wfopen(wide, L"rbN");
    free(wide);
    if (!file || _fstat64(_fileno(file), &st) != 0) goto failed;
    int regular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st;
    /* Opening a FIFO must not block before we can reject it. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) goto failed;
    file = fdopen(fd, "rb");
    if (!file) {
        int saved = errno;
        close(fd);
        errno = saved;
        goto failed;
    }
    if (fstat(fileno(file), &st) != 0) goto failed;
    int regular = S_ISREG(st.st_mode);
#endif
    if (!regular || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        hv_fail(error, error_size, "%s: requires a nonempty regular file of addressable size", path);
        fclose(file);
        return NULL;
    }
    *size = (size_t)st.st_size;
    return file;
failed:
    hv_fail(error, error_size, "%s: %s", path, strerror(errno));
    if (file) fclose(file);
    return NULL;
}

/* Keep seek and read together when parallel decode jobs share the JPX file. */
static int read_file(FILE *file, size_t offset, uint8_t *out, size_t size,
                      char *error, size_t error_size) {
    int result = 0;
    local_lock(file);
    if ((uint64_t)local_tell(file) != offset && local_seek(file, offset, SEEK_SET) != 0)
        result = hv_fail(error, error_size, "file seek: %s", strerror(errno));
    else if (fread(out, 1, size, file) != size)
        result = hv_fail(error, error_size, "file read: %s", ferror(file) ? strerror(errno) : "unexpected end of file");
    local_unlock(file);
    return result;
}

void hv_local_close(hv_local *source) {
    if (!source) return;
    if (source->file) fclose(source->file);
    hv_metadata_close(&source->metadata);
    hv_presentation_free(&source->presentation);
    if (source->companions)
        for (size_t i = 0; i < source->reference_count; i++) free(source->companions[i].path);
    free(source->main.path);
    free(source->headers);
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

/* Copy interpreted boxes, retaining their nested structure. Source payloads
 * and opaque top-level boxes need only an empty box to preserve inventory. */
static int retain_body(uint32_t type) {
    switch (type) {
    case HV_BOX_JP: case HV_BOX_FTYP: case HV_BOX_JP2H: case HV_BOX_JPCH:
    case HV_BOX_JPLH: case HV_BOX_ASOC: case HV_BOX_GRP: case HV_BOX_XML:
    case HV_BOX_FTBL: case HV_BOX_DTBL:
        return 1;
    default: return 0;
    }
}

static int scan_file(hv_local *source, hv_extent **mdat, size_t *mdat_count,
                      char *error, size_t error_size) {
    size_t pos = 0, stream_capacity = 0, mdat_capacity = 0, capacity = 0;
    while (pos < source->main.size) {
        uint8_t bytes[16];
        BoxHeader header;
        size_t used, available = source->main.size - pos;
        size_t n = available < sizeof bytes ? available : sizeof bytes;
        if (read_file(source->file, pos, bytes, n, error, error_size)) return -1;
        if (HV_DECODE_USED(BoxHeader, &header, bytes, 0, n, &used))
            return hv_fail(error, error_size, "invalid or truncated box header at %zu", pos);
        uint64_t length = header.lbox == 0 ? available : header.lbox == 1 ? header.xlbox : header.lbox;
        if (length > available) return hv_fail(error, error_size, "box overruns its container at %zu", pos);
        hv_box box = {(uint32_t)header.tbox, pos, pos + used, pos + (size_t)length, header.lbox == 0};
        size_t kept = retain_body(box.type) ? (size_t)length : 8;
        if (kept > SIZE_MAX - source->header_size) goto oom;
        size_t total = source->header_size + kept;
        if (total > capacity) {
            size_t grown = capacity <= SIZE_MAX / 2 ? capacity * 2 : SIZE_MAX;
            if (grown < total) grown = total;
            uint8_t *data = realloc(source->headers, grown);
            if (!data) goto oom;
            source->headers = data; capacity = grown;
        }
        uint8_t *out = source->headers + source->header_size;
        if (retain_body(box.type)) {
            if (read_file(source->file, pos, out, kept, error, error_size)) return -1;
        } else {
            memset(out, 0, 8); out[3] = 8;
            memcpy(out + 4, bytes + 4, 4);
        }
        if (box.type == HV_BOX_JP2C || box.type == HV_BOX_FTBL) {
            local_stream *grown = reserve(source->streams, &stream_capacity, source->count, sizeof *grown);
            if (!grown) goto oom;
            source->streams = grown;
            local_stream *stream = &source->streams[source->count++];
            *stream = (local_stream){0};
            stream->box = box;
            if (box.type == HV_BOX_FTBL) {
                stream->box.start = source->header_size;
                stream->box.payload = source->header_size + used;
                stream->box.end = total;
            } else stream->size = box.end - box.payload;
        } else if (box.type == HV_BOX_MDAT) {
            hv_extent *grown = reserve(*mdat, &mdat_capacity, *mdat_count, sizeof *grown);
            if (!grown) goto oom;
            *mdat = grown;
            (*mdat)[(*mdat_count)++] = (hv_extent){box.payload, box.end};
        }
        source->header_size = total;
        pos = box.end;
    }
    return 0;
oom:
    return hv_fail(error, error_size, "out of memory");
}
/* Preserve LOC during parsing; filesystem interpretation happens only here. */
static char *reference_path(const uint8_t *loc, size_t n, const char *main_path,
                            char *error, size_t error_size) {
    const uint8_t *text = loc;
    size_t length = n, directory = 0;
    const char *slash = strrchr(main_path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(main_path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    char *decoded, *path;
    int status;
    if (n >= 7 && memcmp(loc, "file://", 7) == 0) {
        text += 7; length -= 7;
    } else if (memchr(loc, ':', n) != NULL
#ifdef _WIN32
               && !(n >= 3 && ((loc[0] >= 'A' && loc[0] <= 'Z') || (loc[0] >= 'a' && loc[0] <= 'z')) &&
                    loc[1] == ':' && (loc[2] == '/' || loc[2] == '\\'))
#endif
    ) {
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
#ifdef _WIN32
    if (strlen(decoded) >= 3 && decoded[0] == '/' && decoded[2] == ':')
        memmove(decoded, decoded + 1, strlen(decoded));
    if (decoded[0] == '\\' || (strlen(decoded) >= 2 && decoded[1] == ':')) return decoded;
#endif
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
    if (file->path != NULL) return file;
    rule = hv_url_read(source->headers, &source->references[fragment->dr - 1], &loc, &n, &at);
    if (rule != NULL) {
        hv_fail(error, error_size, "%s at %zu", rule, at);
        return NULL;
    }
    target = reference_path(loc, n, path, error, error_size);
    if (target == NULL) return NULL;
    FILE *opened = open_file(target, &file->size, error, error_size);
    if (!opened) {
        free(target);
        return NULL;
    }
    fclose(opened);
    file->path = target;
    return file;
}

hv_local *hv_local_open(const char *path, char *error, size_t error_size) {
    hv_local *source = calloc(1, sizeof *source);
    hv_boxes boxes, references;
    hv_ftyp type;
    hv_box box, dtbl = {0};
    hv_extent *mdat = NULL;
    size_t mdat_count = 0, at = 0;
    const char *rule;
    int status;
    if (!source) goto oom;
    source->main.path = malloc(strlen(path) + 1);
    if (!source->main.path) goto oom;
    strcpy(source->main.path, path);
    source->file = open_file(path, &source->main.size, error, error_size);
    if (!source->file || scan_file(source, &mdat, &mdat_count, error, error_size)) goto fail;
    rule = hv_container_open(source->headers, source->header_size, &boxes, &type, &at);
    if (rule) goto invalid;
    int jpx = hv_ftyp_is_jpx(&type);
    if (!jpx) {
        rule = hv_read_jp2(source->headers, source->header_size, &box, &at);
        if (rule) goto invalid;
        /* JP2 discovery ignores other source-like boxes. */
        size_t index = 0;
        while (index < source->count && source->streams[index].box.type != HV_BOX_JP2C) index++;
        source->streams[0] = source->streams[index];
        source->count = 1;
    } else {
        while ((status = hv_boxes_next(&boxes, &box, &rule, &at)) == 1) {
            at = box.start;
            if (box.type == HV_BOX_J2CX) { rule = "unsupported extended codestream boxes"; goto invalid; }
            if (box.type == HV_BOX_DTBL) {
                if (dtbl.type) { rule = "jpx.one-dtbl"; goto invalid; }
                dtbl = box;
            }
        }
        if (status < 0) goto invalid;
    }
    if (dtbl.type != 0) {
        rule = hv_references_open(source->headers, &dtbl, &references, &source->reference_count, &at);
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
            rule = hv_fragments_open(source->headers, &stream->box, &stream->fragments, &at);
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
                    rule = hv_rule_fragment_here(mdat, mdat_count, NULL,
                                                  fragment.off, fragment.len, 0);
                    if (rule) goto invalid;
                    if (j == 0) {
                        uint8_t prefix[2];
                        size_t n = fragment.len < sizeof prefix ? (size_t)fragment.len : sizeof prefix;
                        if (read_file(source->file, (size_t)fragment.off, prefix, n, error, error_size)) goto fail;
                        if (!n || prefix[0] != 0xFF || (n > 1 && prefix[1] != 0x4F)) {
                            rule = "flst.codestream-start"; goto invalid;
                        }
                    }
                }
                if (fragment.len > SIZE_MAX - stream->size) { rule = "codestream size overflow"; goto invalid; }
                stream->size += (size_t)fragment.len;
            }
        }
        if (stream->size == 0) { rule = "empty codestream"; at = stream->box.start; goto invalid; }
    }
    free(mdat);
    if (!jpx) { fclose(source->file); source->file = NULL; }
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
        const char *rule = hv_presentation_open(source->headers, source->header_size,
                                                &source->presentation, &at);
        if (rule != NULL) return hv_fail(error, error_size, "%s at %zu", rule, at);
    }
    *data = source->headers;
    *presentation = &source->presentation;
    return 0;
}

static void close_input(hv_local_input *input) {
    if (input->file) fclose(input->file);
    input->file = NULL;
}

static int open_input(hv_local_input *input, const hv_local *source, size_t codestream,
                       char *error, size_t error_size) {
    *input = (hv_local_input){0};
    if (codestream >= source->count) return hv_fail(error, error_size, "codestream index out of range");
    input->source = source;
    input->stream = &source->streams[codestream];
    if (!source->file) {
        size_t size;
        input->file = open_file(source->main.path, &size, error, error_size);
        if (!input->file) return -1;
        if (size != source->main.size) {
            close_input(input);
            return hv_fail(error, error_size, "local file size changed");
        }
    }
    return 0;
}

hv_local_input *hv_local_input_open(const hv_local *source, size_t codestream,
                                    char *error, size_t error_size) {
    hv_local_input *input = malloc(sizeof *input);
    if (!input) { hv_fail(error, error_size, "out of memory"); return NULL; }
    if (open_input(input, source, codestream, error, error_size)) { free(input); return NULL; }
    return input;
}
void hv_local_input_close(hv_local_input *input) {
    if (input) { close_input(input); free(input); }
}

static int read_fragment(hv_local_input *input, size_t reference, size_t offset,
                          uint8_t *out, size_t size, char *error, size_t error_size) {
    const hv_local *source = input->source;
    if (reference == 0 && source->file)
        return read_file(source->file, offset, out, size, error, error_size);
    if (!input->file || input->reference != reference) {
        close_input(input);
        const local_file *file = reference ? &source->companions[reference - 1] : &source->main;
        size_t length;
        input->file = open_file(file->path, &length, error, error_size);
        if (!input->file) return -1;
        if (length != file->size) {
            close_input(input);
            return hv_fail(error, error_size, "local file size changed");
        }
        input->reference = reference;
    }
    return read_file(input->file, offset, out, size, error, error_size);
}

size_t hv_local_input_read(hv_local_input *input, size_t offset, uint8_t *out,
                            size_t capacity, char *error, size_t error_size) {
    const local_stream *stream = input->stream;
    size_t written = 0;
    if (offset > stream->size || (capacity && !out)) {
        hv_fail(error, error_size, "invalid codestream read"); return 0;
    }
    size_t size = stream->size - offset;
    if (size > capacity) size = capacity;
    if (!size) return 0;
    if (stream->box.type == HV_BOX_JP2C)
        return read_fragment(input, 0, stream->box.payload + offset, out, size, error, error_size) ? 0 : size;
    for (size_t i = 0; i < stream->fragments.count && written < size; i++) {
        Fragment fragment;
        size_t at, length;
        const char *rule = hv_fragment_read(&stream->fragments, i, &fragment, &at);
        if (rule) { hv_fail(error, error_size, "%s at %zu", rule, at); return 0; }
        if (offset >= fragment.len) { offset -= (size_t)fragment.len; continue; }
        length = fragment.len - offset < size - written ? (size_t)fragment.len - offset : size - written;
        if (read_fragment(input, fragment.dr, (size_t)fragment.off + offset, out + written, length, error, error_size)) return 0;
        written += length; offset = 0;
    }
    return written;
}

size_t hv_local_read(const hv_local *source, size_t codestream, size_t offset,
                     uint8_t *out, size_t capacity, char *error, size_t error_size) {
    hv_local_input input;
    if (open_input(&input, source, codestream, error, error_size)) return 0;
    size_t size = hv_local_input_read(&input, offset, out, capacity, error, error_size);
    close_input(&input);
    return size;
}

size_t hv_local_copy(const hv_local *source, size_t codestream, uint8_t *out,
                     size_t capacity, char *error, size_t error_size) {
    if (codestream >= source->count) { hv_fail(error, error_size, "codestream index out of range"); return 0; }
    size_t size = source->streams[codestream].size;
    if (!out) return size;
    if (capacity < size) { hv_fail(error, error_size, "codestream buffer too small"); return 0; }
    return hv_local_read(source, codestream, 0, out, size, error, error_size);
}

int hv_local_siz(const hv_local *source, size_t codestream, SizFixed *fixed,
                  Component *components, size_t capacity, char *error, size_t error_size) {
    uint8_t prefix[6], *header = NULL;
    hv_local_input input;
    int result = -1;
    size_t at = 0;
    if (open_input(&input, source, codestream, error, error_size)) return -1;
    if (hv_local_input_read(&input, 0, prefix, sizeof prefix, error, error_size) != sizeof prefix) {
        hv_fail(error, error_size, "truncated SOC/SIZ"); goto done;
    }
    if (prefix[0] != 0xff || prefix[1] != 0x4f || prefix[2] != 0xff || prefix[3] != 0x51) {
        hv_fail(error, error_size, "expected SOC followed by SIZ"); goto done;
    }
    size_t length = 4 + ((size_t)prefix[4] << 8) + prefix[5];
    if (length > input.stream->size) { hv_fail(error, error_size, "truncated SIZ"); goto done; }
    header = malloc(length);
    if (!header) { hv_fail(error, error_size, "out of memory"); goto done; }
    if (hv_local_input_read(&input, 0, header, length, error, error_size) != length) goto done;
    const char *rule = hv_read_siz(header, length, fixed, components, capacity, &at);
    result = rule ? hv_fail(error, error_size, "%s at %zu", rule, at) : 0;
done:
    free(header);
    close_input(&input);
    return result;
}

static int local_metadata(hv_local *source, size_t layers, char *error, size_t error_size) {
    if (source->metadata.frames != NULL && source->metadata.layer_count >= layers) return 0;
    hv_metadata_close(&source->metadata);
    return hv_metadata_read(source->headers, source->header_size, source->count, layers,
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
