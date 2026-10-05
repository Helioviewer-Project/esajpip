/* hvc_metadata.c: see hvc_metadata.h. */
#include "hvc_metadata.h"

#include "jpeg2000/hv_codes.h"
#include "jpeg2000/hv_error.h"
#include "jpeg2000/hv_rules.h"

/* A placeholder's payload: Flags, OrigID, then the header of the box it
 * stands for. */
enum {
    PHLD = 0x70686C64,
    PHLD_ORIGINAL = 1,      /* Flags: OrigID is the metadata bin of the box's contents */
    PHLD_CODESTREAM = 4,    /* Flags: the box is a codestream */
    PHLD_HEADER = 4 + 8    /* offset of the original box's header */
};

static uint64_t big_endian(const uint8_t *bytes, int count) {
    uint64_t value = 0;
    while (count-- > 0)
        value = value << 8 | *bytes++;
    return value;
}

/* A complete metadata bin, or NULL with a message. */
static const hvc_bin *metadata_bin(const hvc_cache *cache, uint64_t id, char *error,
                                  size_t error_size) {
    const hvc_bin *bin = hvc_cache_find(cache, HVC_BIN_META_DATA, 0, id);
    if (bin == NULL || !bin->complete) {
        hv_fail(error, error_size, "metadata data-bin %llu is %s", (unsigned long long)id,
                bin == NULL ? "missing" : "incomplete");
        return NULL;
    }
    return bin;
}

static uint64_t codestreams(const hvc_cache *cache, size_t *layers, char *error, size_t error_size) {
    const hvc_bin *bin = metadata_bin(cache, 0, error, error_size);
    uint64_t count = 0;
    hv_boxes boxes;
    hv_box box;
    int status;
    const char *reason;
    size_t at;

    if (bin == NULL)
        return 0;
    hv_boxes_file(&boxes, bin->data, bin->length);
    *layers = 0;
    while ((status = hv_boxes_next(&boxes, &box, &reason, &at)) == 1) {
        if (box.type == HV_BOX_JPLH) (*layers)++;
        if (box.type == PHLD && box.end - box.payload >= 4 &&
            (big_endian(bin->data + box.payload, 4) & PHLD_CODESTREAM))
            count++;
    }
    if (!*layers) *layers = (size_t)count;
    if (status < 0) hv_fail(error, error_size, "metadata: %s", reason);
    if (status == 0 && count == 0)
        hv_fail(error, error_size, "metadata: no codestream");
    return status < 0 ? 0 : count;
}

static int resolve_box(const void *context, uint32_t *type, const uint8_t **payload,
                       size_t *length, char *error, size_t error_size) {
    if (*type != PHLD) return 1;
    if (*length < 4) return hv_fail(error, error_size, "metadata: phld.header");
    unsigned flags = (unsigned)big_endian(*payload, 4);
    if (!(flags & PHLD_ORIGINAL) || (flags & PHLD_CODESTREAM)) return 0;
    if (*length < PHLD_HEADER + 8) return hv_fail(error, error_size, "metadata: phld.header");
    *type = (uint32_t)big_endian(*payload + PHLD_HEADER + 4, 4);
    if (*type == HV_BOX_ASOC || *type == HV_BOX_GRP || *type == HV_BOX_NLST || *type == HV_BOX_XML) {
        const hvc_bin *bin = metadata_bin(context, big_endian(*payload + 4, 8), error, error_size);
        if (bin == NULL) return -1;
        *payload = bin->data;
        *length = bin->length;
    }
    return 1;
}

int hvc_metadata_open(const hvc_cache *cache, hv_metadata *metadata, char *error, size_t error_size) {
    size_t layers;
    uint64_t codestream_count = codestreams(cache, &layers, error, error_size);
    const hvc_bin *bin;
    *metadata = (hv_metadata){0};
    if (codestream_count == 0) return -1;
    if (codestream_count > SIZE_MAX) return hv_fail(error, error_size, "metadata: too many codestreams");
    bin = metadata_bin(cache, 0, error, error_size);
    return hv_metadata_read(bin->data, bin->length, (size_t)codestream_count, layers,
                            resolve_box, cache, metadata, error, error_size);
}

int hvc_metadata_presentation(const hvc_cache *cache, const uint8_t **data,
                               hv_presentation *presentation, char *error, size_t error_size) {
    const hvc_bin *bin = metadata_bin(cache, 0, error, error_size);
    if (!bin) return -1;
    hv_boxes boxes;
    hv_box box;
    const char *reason;
    size_t at, count = 0;
    int status;
    hv_boxes_file(&boxes, bin->data, bin->length);
    while ((status = hv_boxes_next(&boxes, &box, &reason, &at)) == 1) {
        if (box.type != PHLD) continue;
        const uint8_t *p = bin->data + box.payload;
        size_t length = box.end - box.payload;
        if (length < PHLD_HEADER + 8) return hv_fail(error, error_size, "metadata: phld.header");
        unsigned flags = (unsigned)big_endian(p, 4);
        uint32_t type = (uint32_t)big_endian(p + PHLD_HEADER + 4, 4);
        if (!(flags & PHLD_CODESTREAM)) {
            if (type == HV_BOX_JP2H || type == HV_BOX_JPCH || type == HV_BOX_JPLH)
                return hv_fail(error, error_size, "partitioned presentation headers are unsupported");
            continue;
        }
        size_t header = big_endian(p + PHLD_HEADER, 4) == 1 ? 16 : 8;
        if (flags != PHLD_CODESTREAM || length != PHLD_HEADER + header + 24 ||
            (type != HV_BOX_JP2C && type != HV_BOX_FTBL) ||
            big_endian(p + PHLD_HEADER + header + 16, 8) != count)
            return hv_fail(error, error_size, "unsupported codestream placeholder");
        count++;
    }
    if (status < 0) return hv_fail(error, error_size, "metadata: %s", reason);
    reason = hv_presentation_open_headers(bin->data, bin->length, count, presentation, &at);
    if (reason) return hv_fail(error, error_size, "presentation at %zu: %s", at, reason);
    *data = bin->data;
    return 0;
}
