/* hv_metadata.c: see hv_metadata.h. */
#include "hv_metadata.h"

#include <stdlib.h>

#include "jpeg2000/hv_codes.h"
#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_rules.h"

/* A placeholder's payload: Flags, OrigID, then the header of the box it
 * stands for. */
enum {
    PHLD = 0x70686C64,
    PHLD_ORIGINAL = 1,      /* Flags: OrigID is the metadata bin of the box's contents */
    PHLD_CODESTREAM = 4,    /* Flags: the box is a codestream */
    PHLD_HEADER = 4 + 8,    /* offset of the original box's header */
    NLST_CODESTREAM = 1 << 24, NLST_LAYER = 2 << 24
};

static uint64_t big_endian(const uint8_t *bytes, int count) {
    uint64_t value = 0;
    while (count-- > 0)
        value = value << 8 | *bytes++;
    return value;
}

/* A complete metadata bin, or NULL with a message. */
static const hv_bin *metadata_bin(const hv_cache *cache, uint64_t id, char *error,
                                  size_t error_size) {
    const hv_bin *bin = hv_cache_find(cache, HV_BIN_META_DATA, 0, id);
    if (bin == NULL || !bin->complete) {
        hv_fail(error, error_size, "metadata data-bin %llu is %s", (unsigned long long)id,
                bin == NULL ? "missing" : "incomplete");
        return NULL;
    }
    return bin;
}

/* The next box of a bin: 1, 0 at its end, or -1 with a message. */
static int next_box(hv_boxes *boxes, hv_box *box, char *error, size_t error_size) {
    const char *reason;
    size_t at;
    int status = hv_boxes_next(boxes, box, &reason, &at);
    if (status < 0)
        hv_fail(error, error_size, "metadata: %s", reason);
    return status;
}

static uint64_t codestreams(const hv_cache *cache, char *error, size_t error_size) {
    const hv_bin *bin = metadata_bin(cache, 0, error, error_size);
    uint64_t count = 0;
    hv_boxes boxes;
    hv_box box;
    int status;

    if (bin == NULL)
        return 0;
    hv_boxes_file(&boxes, bin->data, bin->length);
    while ((status = next_box(&boxes, &box, error, error_size)) == 1)
        if (box.type == PHLD && box.end - box.payload >= 4 &&
            (big_endian(bin->data + box.payload, 4) & PHLD_CODESTREAM))
            count++;
    if (status == 0 && count == 0)
        hv_fail(error, error_size, "metadata: no codestream");
    return status < 0 ? 0 : count;
}

typedef struct {
    const uint8_t *pclr, *cmap;
    size_t pclr_size, cmap_size;
} palette_boxes;

struct hv_metadata_frame {
    const uint8_t *xml;
    size_t xml_size;
    palette_boxes palette;
};

/* A leading number list applies to descendants of its association. Later
 * number lists do not create scopes. Grouping boxes are transparent. */
typedef struct xml_scope {
    const uint8_t *names;
    size_t size;
    const struct xml_scope *parent;
} xml_scope;

static int xml_boxes(const hv_cache *cache, const uint8_t *data, size_t size,
                     uint32_t container, const xml_scope *scope, int file_level, int depth,
                     hv_metadata *metadata, struct hv_metadata_frame *file_xml,
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
        if (type == PHLD) {
            if (length < 4)
                return hv_fail(error, error_size, "metadata: phld.header");
            unsigned flags = (unsigned)big_endian(payload, 4);
            if (!(flags & PHLD_ORIGINAL) || (flags & PHLD_CODESTREAM)) {
                children++;
                continue;
            }
            if (length < PHLD_HEADER + 8)
                return hv_fail(error, error_size, "metadata: phld.header");
            type = (uint32_t)big_endian(payload + PHLD_HEADER + 4, 4);
            if (type == HV_BOX_ASOC || type == HV_BOX_GRP ||
                type == HV_BOX_NLST || type == HV_BOX_XML) {
                const hv_bin *bin = metadata_bin(cache, big_endian(payload + 4, 8),
                                                  error, error_size);
                if (bin == NULL) return -1;
                payload = bin->data;
                length = bin->length;
            }
        }
        if (container == HV_BOX_ASOC && children == 0) {
            if (type == HV_BOX_GRP)
                return hv_fail(error, error_size, "metadata: grp.asoc-first");
            if (type == HV_BOX_NLST) {
                if (length % 4 != 0)
                    return hv_fail(error, error_size, "metadata: nlst.length");
                local = (xml_scope){payload, length, scope};
                scope = &local;
            }
        }
        if (type == HV_BOX_ASOC || type == HV_BOX_GRP) {
            if (xml_boxes(cache, payload, length, type, scope,
                          file_level && type == HV_BOX_GRP, depth + 1, metadata, file_xml, error, error_size) != 0)
                return -1;
        } else if (type == HV_BOX_XML) {
            if (scope == NULL && file_level && file_xml->xml == NULL) {
                file_xml->xml = payload;
                file_xml->xml_size = length;
            }
            for (const xml_scope *names = scope; names != NULL; names = names->parent) {
                for (size_t at = 0; at < names->size; at += 4) {
                    uint32_t name = (uint32_t)big_endian(names->names + at, 4);
                    uint32_t kind = name & 0xFF000000u, index = name & 0xFFFFFFu;
                    if ((kind == NLST_CODESTREAM || kind == NLST_LAYER) &&
                        index < metadata->count && metadata->frames[index].xml == NULL) {
                        metadata->frames[index].xml = payload;
                        metadata->frames[index].xml_size = length;
                    }
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
    metadata->frames = NULL;
    metadata->count = 0;
}

int hv_metadata_open(const hv_cache *cache, hv_metadata *metadata,
                     char *error, size_t error_size) {
    const hv_bin *bin;
    uint64_t count = codestreams(cache, error, error_size);
    palette_boxes defaults = {0};
    struct hv_metadata_frame file_xml = {0};
    size_t header = 0, i;
    hv_boxes boxes;
    hv_box box;
    int status;

    *metadata = (hv_metadata){0};
    if (count == 0) return -1;
    if (count > SIZE_MAX / sizeof *metadata->frames ||
        (metadata->frames = calloc((size_t)count, sizeof *metadata->frames)) == NULL)
        return hv_fail(error, error_size, "out of memory for metadata index");
    metadata->count = (size_t)count;
    bin = metadata_bin(cache, 0, error, error_size);
    hv_boxes_file(&boxes, bin->data, bin->length);
    while ((status = next_box(&boxes, &box, error, error_size)) == 1) {
        const uint8_t *payload = bin->data + box.payload;
        if (box.type == HV_BOX_JP2H) {
            if (header_palette(bin->data, &box, &defaults, error, error_size) != 0)
                goto fail;
            for (i = 0; i < metadata->count; i++) {
                palette_boxes *palette = &metadata->frames[i].palette;
                if (defaults.pclr != NULL) {
                    palette->pclr = defaults.pclr;
                    palette->pclr_size = defaults.pclr_size;
                }
                if (defaults.cmap != NULL) {
                    palette->cmap = defaults.cmap;
                    palette->cmap_size = defaults.cmap_size;
                }
            }
        } else if (box.type == HV_BOX_JPCH && header < metadata->count) {
            if (header_palette(bin->data, &box, &metadata->frames[header++].palette,
                               error, error_size) != 0)
                goto fail;
        } else if (box.type == HV_BOX_XML && file_xml.xml == NULL) {
            file_xml.xml = payload;
            file_xml.xml_size = box.end - box.payload;
        } else if (box.type == HV_BOX_ASOC || box.type == HV_BOX_GRP) {
            if (xml_boxes(cache, payload, box.end - box.payload, box.type, NULL,
                          box.type == HV_BOX_GRP, 1, metadata, &file_xml,
                          error, error_size) != 0)
                goto fail;
        } else if (box.type == PHLD) {
            if (xml_boxes(cache, bin->data + box.start, box.end - box.start, 0, NULL,
                          1, 0, metadata, &file_xml, error, error_size) != 0)
                goto fail;
        }
    }
    if (status < 0) goto fail;
    for (i = 0; i < metadata->count; i++)
        if (metadata->frames[i].xml == NULL) {
            metadata->frames[i].xml = file_xml.xml;
            metadata->frames[i].xml_size = file_xml.xml_size;
        }
    return 0;
fail:
    hv_metadata_close(metadata);
    return -1;
}

int hv_metadata_xml(const hv_metadata *metadata, uint64_t codestream,
                    const uint8_t **xml, size_t *size, char *error, size_t error_size) {
    *xml = NULL;
    *size = 0;
    if (codestream >= metadata->count)
        return hv_fail(error, error_size, "no frame %llu", (unsigned long long)codestream);
    *xml = metadata->frames[codestream].xml;
    *size = metadata->frames[codestream].xml_size;
    return 0;
}

int hv_metadata_palette(const hv_metadata *metadata, uint64_t codestream,
                        int *channels, uint8_t *table, size_t capacity,
                        char *error, size_t error_size) {
    enum { PCLR_COUNTS = 3, CMAP_ENTRY = 4, MAX_ENTRIES = 1024, MAX_COLUMNS = 255, MAX_DEPTH = 38 };
    palette_boxes found;
    uint8_t column[MAX_COLUMNS];
    size_t offset[MAX_COLUMNS], entry = 0, entries, columns, at, i;
    const uint8_t *depths, *values;
    int component = -1, count = 0, channel;

    *channels = 0;
    if (codestream >= metadata->count)
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

    /* NE, NPC, a bit depth for each column, then the entries: each column's
     * value in whole bytes. */
    if (found.pclr_size < PCLR_COUNTS)
        return hv_fail(error, error_size, "pclr: no NE and NPC");
    entries = (size_t)big_endian(found.pclr, 2);
    columns = found.pclr[2];
    if (entries < 1 || entries > MAX_ENTRIES || columns < 1 ||
        found.pclr_size - PCLR_COUNTS < columns)
        return hv_fail(error, error_size, "pclr: NE or NPC out of range");
    depths = found.pclr + PCLR_COUNTS;
    values = depths + columns;
    for (i = 0; i < columns; i++) {
        if ((depths[i] & 0x7Fu) + 1 > MAX_DEPTH)
            return hv_fail(error, error_size, "pclr: bit depth out of range");
        offset[i] = entry;
        entry += ((depths[i] & 0x7Fu) + 8) / 8;
    }
    if (found.pclr_size - PCLR_COUNTS - columns != entries * entry)
        return hv_fail(error, error_size, "pclr.entries-length");
    for (channel = 0; channel < count; channel++)
        if (column[channel] >= columns)
            return hv_fail(error, error_size, "cmap: no palette column %d", column[channel]);

    *channels = count;
    if (capacity < entries * (size_t)count)
        return (int)entries;
    for (i = 0; i < entries; i++) {
        for (channel = 0; channel < count; channel++) {
            uint8_t depth = depths[column[channel]];
            unsigned bits = (depth & 0x7Fu) + 1;
            uint64_t value = big_endian(values + i * entry + offset[column[channel]],
                                        (int)((bits + 7) / 8));
            value &= ((uint64_t)1 << bits) - 1;
            if (depth & 0x80u)                  /* signed: from -2^(bits-1) */
                value ^= (uint64_t)1 << (bits - 1);
            *table++ = (uint8_t)(bits >= 8 ? value >> (bits - 8)
                                           : value * 255 / (((uint64_t)1 << bits) - 1));
        }
    }
    return (int)entries;
}
