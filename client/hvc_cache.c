/* hvc_cache.c: see hvc_cache.h. */
#include "hvc_cache.h"
#include "hvc_frame.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>

void hvc_cache_begin(hvc_cache *cache) {
    *cache = (hvc_cache){0};
}

/* Unsigned arithmetic that wraps on purpose, which Clang's
 * -fsanitize=integer would report. */
#if defined(__clang__)
#define WRAPS __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base")))
#else
#define WRAPS
#endif

/* The table is open-addressed: a bin sits at the first free slot from the one
 * its identity hashes to, and the table is at most half full. */
WRAPS static size_t Slot(const hvc_bin *bins, size_t capacity, int bin_class,
                         uint64_t codestream, uint64_t bin_id) {
    uint64_t hash = (codestream * 8 + (uint64_t) bin_class) * UINT64_C(0x9E3779B97F4A7C15) ^
                    bin_id;
    size_t index;
    hash ^= hash >> 31;
    hash *= UINT64_C(0xBF58476D1CE4E5B9);
    hash ^= hash >> 29;
    for (index = (size_t) hash & (capacity - 1); bins[index].used;
         index = (index + 1) & (capacity - 1)) {
        if (bins[index].bin_class == bin_class && bins[index].codestream == codestream &&
            bins[index].bin_id == bin_id)
            break;
    }
    return index;
}

const hvc_bin *hvc_cache_find(const hvc_cache *cache, int bin_class,
                               uint64_t codestream, uint64_t bin_id) {
    const hvc_bin *bin;
    if (cache->capacity == 0) return NULL;
    bin = &cache->bins[Slot(cache->bins, cache->capacity, bin_class, codestream, bin_id)];
    return bin->used ? bin : NULL;
}

/* Makes room for one more bin. A failed growth leaves the table as it was. */
static int Grow(hvc_cache *cache) {
    size_t capacity = cache->capacity ? cache->capacity * 2 : 64;
    size_t index;
    hvc_bin *bins;
    if (2 * (cache->count + 1) <= cache->capacity) return 0;
    if (capacity > (size_t) -1 / sizeof *bins) return -1;
    bins = (hvc_bin *) calloc(capacity, sizeof *bins);
    if (bins == NULL) return -1;
    for (index = 0; index < cache->capacity; index++) {
        const hvc_bin *bin = &cache->bins[index];
        if (bin->used)
            bins[Slot(bins, capacity, bin->bin_class, bin->codestream, bin->bin_id)] = *bin;
    }
    free(cache->bins);
    cache->bins = bins;
    cache->capacity = capacity;
    return 0;
}

/* Grow unfinished bins geometrically; completed bins need only their final size.
 * Failed shrinking retains the buffer, while failed growth rejects the message. */
static int Reserve(hvc_bin *bin, size_t needed, int complete) {
    size_t capacity;
    uint8_t *grown;
    if (needed == bin->capacity || (!complete && needed < bin->capacity)) return 0;
    capacity = complete ? needed : bin->capacity ? bin->capacity : 256;
    while (capacity < needed) {
        if (capacity > (size_t) -1 / 2) return -1;
        capacity *= 2;
    }
    grown = (uint8_t *) realloc(bin->data, capacity);
    if (grown == NULL) return needed <= bin->capacity ? 0 : -1;
    bin->data = grown;
    bin->capacity = capacity;
    return 0;
}

int hvc_cache_apply(hvc_cache *cache, const hvc_jpp_message *message) {
    hvc_bin fresh = {0};
    hvc_bin *bin = (hvc_bin *) hvc_cache_find(cache, message->bin_class, message->codestream,
                                            message->bin_id);
    size_t needed, held = bin != NULL ? bin->length : 0, overlap;

    cache->error = NULL;

    if (message->offset > SIZE_MAX || message->length > SIZE_MAX - message->offset) {
        cache->error = "Data-bin range overflows";
        return 0;
    }
    needed = (size_t) message->offset + (size_t) message->length;

    if (message->offset > held) {
        cache->error = "Message leaves a gap in the data-bin";
        return 0;
    }
    if ((bin != NULL && bin->complete && needed > held) ||
        (message->last_byte && needed < held)) {
        cache->error = "Message disagrees with the data-bin's final size";
        return 0;
    }
    overlap = held - (size_t)message->offset;
    if (overlap > message->length) overlap = (size_t)message->length;
    if (overlap != 0 && memcmp(bin->data + (size_t)message->offset,
                                message->data, overlap) != 0) {
        cache->error = "Message conflicts with cached data-bin bytes";
        return 0;
    }
    if (bin != NULL && needed <= held) {
        if (message->last_byte && !bin->complete) {
            Reserve(bin, held, 1);
            bin->complete = 1;
        }
        return 1;
    }
    if (bin == NULL) {
        if (Grow(cache) != 0) {
            cache->error = "Out of memory for the data-bin table";
            return 0;
        }
        fresh.used = 1;
        fresh.bin_class = message->bin_class;
        fresh.codestream = message->codestream;
        fresh.bin_id = message->bin_id;
    }
    if (Reserve(bin != NULL ? bin : &fresh, needed, message->last_byte) != 0) {
        cache->error = "Out of memory for a data-bin";
        return 0;
    }
    if (bin == NULL) {
        bin = &cache->bins[Slot(cache->bins, cache->capacity, fresh.bin_class,
                                fresh.codestream, fresh.bin_id)];
        *bin = fresh;
        cache->count++;
    }
    if (needed > held) {
        memcpy(bin->data + held, message->data + overlap, needed - held);
        bin->length = needed;
        cache->bytes += needed - held;
    }
    if (message->last_byte) bin->complete = 1;
    return 1;
}

int hvc_cache_complete(const hvc_cache *cache, int bin_class, uint64_t codestream,
                       uint64_t bin_id) {
    const hvc_bin *bin = hvc_cache_find(cache, bin_class, codestream, bin_id);
    return bin != NULL && bin->complete;
}

size_t hvc_cache_length(const hvc_cache *cache, int bin_class, uint64_t codestream,
                        uint64_t bin_id) {
    const hvc_bin *bin = hvc_cache_find(cache, bin_class, codestream, bin_id);
    return bin != NULL ? bin->length : 0;
}

size_t hvc_cache_bin_count(const hvc_cache *cache) {
    return cache->count;
}

size_t hvc_cache_total_bytes(const hvc_cache *cache) {
    return cache->bytes;
}

size_t hvc_cache_complete_count(const hvc_cache *cache) {
    size_t index;
    size_t complete = 0;
    for (index = 0; index < cache->capacity; index++) {
        if (cache->bins[index].used && cache->bins[index].complete) complete++;
    }
    return complete;
}

const char *hvc_cache_error(const hvc_cache *cache) {
    return cache->error != NULL ? cache->error : "";
}

int hvc_cache_model(const hvc_cache *cache, size_t *cursor, char *text, size_t size) {
    size_t used = 0;
    if (size == 0) return -1;
    text[0] = '\0';
    while (*cursor < cache->capacity) {
        const hvc_bin *bin = &cache->bins[*cursor];
        char descriptor[128];
        int length;
        const char *kind;
        if (!bin->used || (!bin->complete && bin->length == 0)) {
            ++*cursor;
            continue;
        }
        switch (bin->bin_class) {
        case HVC_BIN_META_DATA: kind = "M"; break;
        case HVC_BIN_MAIN_HEADER: kind = "Hm"; break;
        case HVC_BIN_TILE_HEADER: kind = "H"; break;
        case HVC_BIN_PRECINCT: kind = "P"; break;
        default: return -1;
        }
        if ((bin->bin_class == HVC_BIN_META_DATA && !bin->complete) ||
            (!bin->complete && bin->length >= INT_MAX) || bin->bin_id > INT_MAX ||
            ((bin->bin_class == HVC_BIN_MAIN_HEADER || bin->bin_class == HVC_BIN_TILE_HEADER) &&
             bin->bin_id != 0)) return -1;
        if (bin->bin_class == HVC_BIN_META_DATA)
            length = snprintf(descriptor, sizeof descriptor, "M%" PRIu64, bin->bin_id);
        else if (bin->bin_class == HVC_BIN_MAIN_HEADER)
            length = snprintf(descriptor, sizeof descriptor, "[%" PRIu64 "]Hm", bin->codestream);
        else
            length = snprintf(descriptor, sizeof descriptor, "[%" PRIu64 "]%s%" PRIu64,
                              bin->codestream, kind, bin->bin_id);
        if (!bin->complete)
            length += snprintf(descriptor + length, sizeof descriptor - (size_t)length,
                               ":%zu", bin->length);
        if (length < 0 || (size_t)length >= sizeof descriptor) return -1;
        if (used + (used != 0) + (size_t)length >= size)
            return used ? (int)used : -1;
        if (used) text[used++] = ',';
        memcpy(text + used, descriptor, (size_t)length + 1);
        used += (size_t)length;
        ++*cursor;
    }
    return (int)used;
}

int hvc_cache_match_metadata(const hvc_cache *cache, const hvc_jpp_message *message) {
    const hvc_bin *bin;
    if (message->bin_class != HVC_BIN_META_DATA) return 0;
    bin = hvc_cache_find(cache, HVC_BIN_META_DATA, message->codestream, message->bin_id);
    if (bin == NULL || !bin->complete || message->offset > bin->length ||
        message->length > bin->length - (size_t)message->offset ||
        (message->last_byte && message->offset + message->length != bin->length)) return 0;
    return message->length == 0 ||
           memcmp(bin->data + (size_t)message->offset, message->data,
                  (size_t)message->length) == 0;
}

void hvc_cache_release(hvc_cache *cache) {
    size_t index;
    for (index = 0; index < cache->capacity; index++) {
        hvc_frame_free(cache->bins[index].frame);
        free(cache->bins[index].data);
    }
    free(cache->bins);
    *cache = (hvc_cache){0};
}
