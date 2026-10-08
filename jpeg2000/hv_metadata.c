/* hv_metadata.c: see hv_metadata.h. */
#include "hv_metadata.h"

#include <stdlib.h>

#include "jpeg2000/hv_codes.h"
#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_rules.h"

enum { NLST_CODESTREAM = 1 << 24, NLST_LAYER = 2 << 24 };

static uint64_t big_endian(const uint8_t *bytes, int count) {
    uint64_t value = 0;
    while (count-- > 0)
        value = value << 8 | *bytes++;
    return value;
}

/* The next bounded box: 1, 0 at its end, or -1 with a message. */
static int next_box(hv_boxes *boxes, hv_box *box, char *error, size_t error_size) {
    const char *reason;
    size_t at;
    int status = hv_boxes_next(boxes, box, &reason, &at);
    if (status < 0)
        hv_fail(error, error_size, "metadata: %s", reason);
    return status;
}

typedef struct {
    const uint8_t *pclr, *cmap;
    size_t pclr_size, cmap_size;
} palette_boxes;

struct hv_metadata_document {
    const uint8_t *data;
    size_t size, order;
};

struct hv_metadata_frame {
    hv_metadata_document xml;
    palette_boxes palette;
};

/* A leading number list applies to descendants of its association. JPCH and
 * JPLH provide an implicit entity scope. Later lists do not create scopes. */
typedef struct xml_scope {
    const uint8_t *names;
    size_t size;
    struct xml_scope *parent;
    uint32_t kind;
    size_t index;
    int associated; /* This scope already encountered its first XML. */
} xml_scope;

static void associate_xml(hv_metadata *metadata, uint32_t kind, size_t index,
                           const hv_metadata_document *document) {
    hv_metadata_document *found = NULL;
    if (kind == NLST_CODESTREAM && index < metadata->codestream_count)
        found = &metadata->frames[index].xml;
    else if (kind == NLST_LAYER && index < metadata->layer_count)
        found = &metadata->layers[index];
    if (found != NULL && found->data == NULL)
        *found = *document;
}

static int xml_boxes(hv_metadata_resolve resolve, const void *context, const uint8_t *data, size_t size,
                     uint32_t container, xml_scope *scope, int file_level, int depth,
                     hv_metadata *metadata, size_t *order,
                     char *error, size_t error_size) {
    hv_boxes boxes;
    hv_box box;
    xml_scope local;
    size_t children = 0;
    int status;
    if (depth > HV_BOX_DEPTH_MAX)
        return hv_fail(error, error_size, "metadata: box.depth-limit");
    hv_boxes_file(&boxes, data, size);
    while ((status = next_box(&boxes, &box, error, error_size)) == 1) {
        const uint8_t *payload = data + box.payload;
        size_t length = box.end - box.payload;
        uint32_t type = box.type;
        if (resolve != NULL) {
            int resolved = resolve(context, &type, &payload, &length, error, error_size);
            if (resolved < 0) return -1;
            if (resolved == 0) { children++; continue; }
        }
        if (container == HV_BOX_ASOC && children == 0) {
            if (type == HV_BOX_GRP)
                return hv_fail(error, error_size, "metadata: grp.asoc-first");
            if (type == HV_BOX_NLST) {
                if (length % 4 != 0)
                    return hv_fail(error, error_size, "metadata: nlst.length");
                local = (xml_scope){payload, length, scope, 0, 0, 0};
                scope = &local;
            }
        }
        if (type == HV_BOX_ASOC || type == HV_BOX_GRP) {
            if (xml_boxes(resolve, context, payload, length, type, scope,
                          file_level && type == HV_BOX_GRP, depth + 1, metadata, order, error, error_size) != 0)
                return -1;
        } else if (type == HV_BOX_XML) {
            hv_metadata_document document = {payload, length, (*order)++};
            if (scope == NULL && file_level && metadata->file_xml == NULL) {
                metadata->file_xml = payload;
                metadata->file_xml_size = length;
            }
            for (xml_scope *names = scope; names != NULL; names = names->parent) {
                if (names->associated) continue;
                names->associated = 1;
                if (names->kind != 0)
                    associate_xml(metadata, names->kind, names->index, &document);
                else for (size_t at = 0; at < names->size; at += 4) {
                    uint32_t name = (uint32_t)big_endian(names->names + at, 4);
                    associate_xml(metadata, name & 0xFF000000u, name & 0xFFFFFFu, &document);
                }
            }
        }
        children++;
    }
    if (status < 0) return -1;
    if (container == HV_BOX_ASOC && children < 2)
        return hv_fail(error, error_size, "metadata: asoc.children");
    return 0;
}

static int header_palette(const uint8_t *data, const hv_box *header, palette_boxes *found,
                          char *error, size_t error_size) {
    hv_boxes boxes;
    hv_box box;
    int status;

    hv_boxes_children(&boxes, data, header);
    while ((status = next_box(&boxes, &box, error, error_size)) == 1) {
        if (box.type == HV_BOX_PCLR) {
            found->pclr = data + box.payload;
            found->pclr_size = box.end - box.payload;
        } else if (box.type == HV_BOX_CMAP) {
            found->cmap = data + box.payload;
            found->cmap_size = box.end - box.payload;
        }
    }
    return status;
}

void hv_metadata_close(hv_metadata *metadata) {
    free(metadata->frames);
    free(metadata->layers);
    *metadata = (hv_metadata){0};
}

int hv_metadata_read(const uint8_t *data, size_t size, size_t codestream_count, size_t layer_count,
                     hv_metadata_resolve resolve, const void *context,
                     hv_metadata *metadata, char *error, size_t error_size) {
    palette_boxes defaults = {0};
    size_t header = 0, layer = 0, order = 0;
    hv_boxes boxes;
    hv_box box;
    int status;

    *metadata = (hv_metadata){0};
    if (codestream_count == 0) return hv_fail(error, error_size, "metadata: no codestream");
    if (codestream_count > SIZE_MAX / sizeof *metadata->frames ||
        layer_count > SIZE_MAX / sizeof *metadata->layers)
        return hv_fail(error, error_size, "out of memory for metadata index");
    metadata->frames = calloc(codestream_count, sizeof *metadata->frames);
    if (metadata->frames != NULL && layer_count != 0)
        metadata->layers = calloc(layer_count, sizeof *metadata->layers);
    if (metadata->frames == NULL || (layer_count != 0 && metadata->layers == NULL)) {
        hv_fail(error, error_size, "out of memory for metadata index");
        goto fail;
    }
    metadata->codestream_count = codestream_count;
    metadata->layer_count = layer_count;
    hv_boxes_file(&boxes, data, size);
    while ((status = next_box(&boxes, &box, error, error_size)) == 1) {
        xml_scope implicit = {0};
        if (box.type == HV_BOX_JP2H) {
            if (header_palette(data, &box, &defaults, error, error_size) != 0)
                goto fail;
            continue;
        }
        if (box.type == HV_BOX_JPCH) {
            if (header < codestream_count && header_palette(data, &box, &metadata->frames[header].palette,
                                                error, error_size) != 0)
                goto fail;
            implicit.kind = NLST_CODESTREAM;
            implicit.index = header++;
        } else if (box.type == HV_BOX_JPLH) {
            implicit.kind = NLST_LAYER;
            implicit.index = layer++;
        }
        if (implicit.kind != 0) {
            if (xml_boxes(resolve, context, data + box.payload, box.end - box.payload, box.type,
                          &implicit, 0, 1, metadata, &order, error, error_size) != 0)
                goto fail;
        } else if (xml_boxes(resolve, context, data + box.start, box.end - box.start, 0, NULL,
                             1, 0, metadata, &order, error, error_size) != 0)
            goto fail;
    }
    if (status < 0) goto fail;
    /* Apply absent palette fields after reading all headers, so late JP2H
     * defaults cannot replace a JPCH override. */
    for (size_t i = 0; i < codestream_count; i++) {
        palette_boxes *palette = &metadata->frames[i].palette;
        if (palette->pclr == NULL) {
            palette->pclr = defaults.pclr;
            palette->pclr_size = defaults.pclr_size;
        }
        if (palette->cmap == NULL) {
            palette->cmap = defaults.cmap;
            palette->cmap_size = defaults.cmap_size;
        }
    }
    return 0;
fail:
    hv_metadata_close(metadata);
    return -1;
}

static void first_document(hv_metadata_document *found, const hv_metadata_document *candidate) {
    if (candidate->data != NULL && (found->data == NULL || candidate->order < found->order))
        *found = *candidate;
}

static void document_result(const hv_metadata *metadata, const hv_metadata_document *document,
                             const uint8_t **xml, size_t *size) {
    *xml = document->data != NULL ? document->data : metadata->file_xml;
    *size = document->data != NULL ? document->size : metadata->file_xml_size;
}

int hv_metadata_xml(const hv_metadata *metadata, uint64_t codestream,
                    const uint8_t **xml, size_t *size, char *error, size_t error_size) {
    hv_metadata_document document = {0};
    *xml = NULL;
    *size = 0;
    if (codestream >= metadata->codestream_count)
        return hv_fail(error, error_size, "no frame %llu", (unsigned long long)codestream);
    first_document(&document, &metadata->frames[codestream].xml);
    if (codestream < metadata->layer_count)
        first_document(&document, &metadata->layers[codestream]);
    document_result(metadata, &document, xml, size);
    return 0;
}

int hv_metadata_codestream_xml(const hv_metadata *metadata, uint64_t codestream,
                               const uint8_t **xml, size_t *size, char *error, size_t error_size) {
    *xml = NULL;
    *size = 0;
    if (codestream >= metadata->codestream_count)
        return hv_fail(error, error_size, "no codestream %llu", (unsigned long long)codestream);
    document_result(metadata, &metadata->frames[codestream].xml, xml, size);
    return 0;
}

int hv_metadata_layer_xml(const hv_metadata *metadata, size_t layer,
                          const hv_registration *registration, const uint8_t **xml,
                          size_t *size, char *error, size_t error_size) {
    hv_metadata_document document = {0};
    *xml = NULL;
    *size = 0;
    if (layer >= metadata->layer_count)
        return hv_fail(error, error_size, "no layer %zu", layer);
    first_document(&document, &metadata->layers[layer]);
    for (size_t i = 0; registration != NULL && i < registration->count; i++) {
        hv_registration_entry entry;
        const char *reason = hv_registration_read(registration, i, &entry);
        if (reason != NULL)
            return hv_fail(error, error_size, "%s", reason);
        if (entry.codestream >= metadata->codestream_count)
            return hv_fail(error, error_size, "no codestream %zu", entry.codestream);
        first_document(&document, &metadata->frames[entry.codestream].xml);
    }
    document_result(metadata, &document, xml, size);
    return 0;
}

int hv_metadata_palette(const hv_metadata *metadata, uint64_t codestream,
                        int *channels, uint8_t *table, size_t capacity,
                        char *error, size_t error_size) {
    enum { CMAP_ENTRY = 4, MAX_COLUMNS = 255 };
    palette_boxes found;
    uint8_t column[MAX_COLUMNS];
    size_t at, i;
    hv_palette palette;
    int component = -1, count = 0, channel;

    *channels = 0;
    if (codestream >= metadata->codestream_count)
        return hv_fail(error, error_size, "no frame %llu", (unsigned long long)codestream);
    found = metadata->frames[codestream].palette;
    if (found.pclr == NULL || found.cmap == NULL)
        return 0;

    /* The channels the mapping makes with the palette, of one component. */
    if (found.cmap_size % CMAP_ENTRY != 0)
        return hv_fail(error, error_size, "cmap: not a whole number of entries");
    for (at = 0; at < found.cmap_size; at += CMAP_ENTRY) {
        if (found.cmap[at + 2] != 1)
            continue;
        if (component >= 0 && component != (int)big_endian(found.cmap + at, 2))
            return hv_fail(error, error_size, "cmap: a palette on several components");
        component = (int)big_endian(found.cmap + at, 2);
        if (count == MAX_COLUMNS)
            return hv_fail(error, error_size, "cmap: too many palette channels");
        column[count++] = found.cmap[at + 3];
    }
    if (count == 0)
        return 0;

    hv_box box = {0};
    box.type = HV_BOX_PCLR;
    box.end = found.pclr_size;
    size_t error_at;
    const char *reason = hv_palette_open(found.pclr, &box, &palette, &error_at);
    if (reason)
        return hv_fail(error, error_size, "%s", reason);
    for (channel = 0; channel < count; channel++)
        if (column[channel] >= palette.column_count)
            return hv_fail(error, error_size, "cmap: no palette column %d", column[channel]);

    *channels = count;
    if (capacity < palette.entry_count * (size_t)count)
        return (int)palette.entry_count;
    for (i = 0; i < palette.entry_count; i++) {
        for (channel = 0; channel < count; channel++) {
            hv_palette_sample sample;
            hv_palette_read(&palette, i, column[channel], &sample);
            *table++ = hv_palette_byte(&sample);
        }
    }
    return (int)palette.entry_count;
}
