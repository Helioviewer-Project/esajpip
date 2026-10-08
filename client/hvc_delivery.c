#include "hvc_delivery.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t first, end; } range;
typedef struct {
    int used, bin_class, complete;
    uint64_t codestream, id, prefix;
    range *ranges;
    size_t count, capacity;
} bin;
struct hvc_delivery {
    uint64_t window;
    bin *bins;
    size_t count, capacity;
    hvc_delivery *next;
};

static size_t slot(const bin *bins, size_t capacity, int bin_class, uint64_t codestream, uint64_t id) {
    uint64_t hash = (codestream * 8 + (uint64_t)bin_class) * UINT64_C(0x9E3779B97F4A7C15) ^ id;
    hash ^= hash >> 31;
    hash *= UINT64_C(0xBF58476D1CE4E5B9);
    hash ^= hash >> 29;
    size_t index = (size_t)hash & (capacity - 1);
    while (bins[index].used) {
        if (bins[index].bin_class == bin_class && bins[index].codestream == codestream && bins[index].id == id)
            break;
        index = (index + 1) & (capacity - 1);
    }
    return index;
}

hvc_delivery *hvc_delivery_window(hvc_delivery **head, uint64_t window) {
    for (hvc_delivery *item = *head; item; item = item->next)
        if (item->window == window) return item;
    hvc_delivery *item = calloc(1, sizeof *item);
    if (!item) return NULL;
    item->window = window;
    item->next = *head;
    *head = item;
    return item;
}

static bin *find(hvc_delivery *delivery, const hvc_jpp_message *message) {
    if (delivery->capacity) {
        bin *found = &delivery->bins[slot(delivery->bins, delivery->capacity,
                                         message->bin_class, message->codestream, message->bin_id)];
        if (found->used) return found;
    }
    if (delivery->count >= delivery->capacity / 2) {
        if (delivery->capacity > SIZE_MAX / 2 / sizeof(bin)) return NULL;
        size_t capacity = delivery->capacity ? delivery->capacity * 2 : 64;
        bin *bins = calloc(capacity, sizeof *bins);
        if (!bins) return NULL;
        for (size_t i = 0; i < delivery->capacity; i++) {
            bin *old = &delivery->bins[i];
            if (old->used) bins[slot(bins, capacity, old->bin_class, old->codestream, old->id)] = *old;
        }
        free(delivery->bins);
        delivery->bins = bins;
        delivery->capacity = capacity;
    }
    bin *fresh = &delivery->bins[slot(delivery->bins, delivery->capacity,
                                     message->bin_class, message->codestream, message->bin_id)];
    fresh->used = 1;
    fresh->bin_class = message->bin_class;
    fresh->codestream = message->codestream;
    fresh->id = message->bin_id;
    delivery->count++;
    return fresh;
}

int hvc_delivery_receive(hvc_delivery *delivery, const hvc_jpp_message *message) {
    bin *value = find(delivery, message);
    if (!value) return -1;
    int progress = message->last_byte && !value->complete;
    value->complete |= message->last_byte;
    if (!message->length) return progress;
    uint64_t first = message->offset, end = first + message->length;
    if (end <= value->prefix) return progress;
    /* Normal delivery is contiguous: keep its prefix without allocating ranges. */
    if (first <= value->prefix) {
        value->prefix = end;
        size_t used = 0;
        while (used < value->count && value->ranges[used].first <= value->prefix) {
            if (value->ranges[used].end > value->prefix) value->prefix = value->ranges[used].end;
            used++;
        }
        if (used) {
            memmove(value->ranges, value->ranges + used, (value->count - used) * sizeof(range));
            value->count -= used;
        }
        return 1;
    }
    /* A replacement channel may replay ranges beyond a prefix not yet replayed. */
    size_t begin = 0;
    while (begin < value->count && value->ranges[begin].end < first) begin++;
    if (begin < value->count && value->ranges[begin].first <= first && value->ranges[begin].end >= end)
        return progress;
    size_t after = begin;
    while (after < value->count && value->ranges[after].first <= end) {
        if (value->ranges[after].first < first) first = value->ranges[after].first;
        if (value->ranges[after].end > end) end = value->ranges[after].end;
        after++;
    }
    if (begin == after && value->count == value->capacity) {
        if (value->capacity > SIZE_MAX / 2 / sizeof(range)) return -1;
        size_t capacity = value->capacity ? value->capacity * 2 : 4;
        range *ranges = realloc(value->ranges, capacity * sizeof *ranges);
        if (!ranges) return -1;
        value->ranges = ranges;
        value->capacity = capacity;
    }
    memmove(value->ranges + begin + 1, value->ranges + after, (value->count - after) * sizeof(range));
    value->ranges[begin] = (range){first, end};
    value->count = value->count - (after - begin) + 1;
    return 1;
}

static void release(hvc_delivery *delivery) {
    for (size_t i = 0; i < delivery->capacity; i++) free(delivery->bins[i].ranges);
    free(delivery->bins);
    free(delivery);
}
void hvc_delivery_end(hvc_delivery **head, uint64_t window) {
    while (*head) {
        hvc_delivery *item = *head;
        if (item->window == window) { *head = item->next; release(item); return; }
        head = &item->next;
    }
}
void hvc_delivery_free(hvc_delivery *head) {
    while (head) { hvc_delivery *next = head->next; release(head); head = next; }
}
