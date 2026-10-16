/* Exact byte-verified reuse, before ROI shift/dequantization. No MQ state is
 * retained. Cache allocation failure is a miss, never a decoding failure. */
#define OPJ_SKIP_POISON
#include "opj_includes.h"
#include "hvc_t1_cache.h"
#define SLOTS 256
#define BUDGET (8u * 1024u * 1024u)
typedef struct {
    OPJ_UINT32 hash, key_size, pixels_size;
    size_t capacity;
    unsigned char data[]; /* raw int32 coefficients, then exact serialized key */
} entry;
struct hvc_t1_cache {
    opj_mutex_t *mutex;
    entry *slots[SLOTS];
    size_t bytes, hits, misses;
};
hvc_t1_cache *hvc_t1_cache_create(void) {
    hvc_t1_cache *c = opj_calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->mutex = opj_mutex_create();
    if (!c->mutex && opj_has_thread_support()) { opj_free(c); return NULL; }
    c->bytes = sizeof(*c);
    return c;
}
void hvc_t1_cache_destroy(hvc_t1_cache *c) {
    if (!c) return;
    for (unsigned i=0; i<SLOTS; ++i) opj_free(c->slots[i]);
    opj_mutex_destroy(c->mutex); opj_free(c);
}
size_t hvc_t1_cache_stat(const hvc_t1_cache *c, unsigned which) {
    /* Called by coordinator only after the pool has drained. */
    return !c ? 0 : which == 0 ? c->bytes : which == 1 ? c->hits : c->misses;
}
static void append(unsigned char **p, const void *data, size_t n) {
    memcpy(*p, data, n); *p += n;
}
OPJ_BOOL hvc_t1_cache_transfer(hvc_t1_cache *c, const opj_tcd_cblk_dec_t *b,
    OPJ_UINT32 orient, OPJ_UINT32 roishift, OPJ_UINT32 style, OPJ_INT32 *pixels, OPJ_BOOL store) {
    /* Small low-resolution blocks only. Stack key is bounded independently of
     * untrusted chunk counts/lengths, and never survives this call. */
    unsigned char key[12288], *p = key;
    OPJ_UINT32 header[] = {(OPJ_UINT32)(b->x1-b->x0), (OPJ_UINT32)(b->y1-b->y0),
        orient, roishift, style, b->numbps, b->real_num_segs, b->numchunks};
    size_t size = sizeof(header), pixel_size = (size_t)header[0]*header[1]*sizeof(*pixels);
    if (!c || b->corrupted || !b->numchunks || header[0]>1024 || header[1]>1024 || !pixel_size || pixel_size>16384 ||
        b->real_num_segs > (sizeof(key)-size)/8) return OPJ_FALSE;
    size += (size_t)b->real_num_segs*8;
    if (b->numchunks > (sizeof(key)-size)/4) return OPJ_FALSE;
    size += (size_t)b->numchunks*4;
    for (OPJ_UINT32 i=0; i<b->numchunks; ++i) {
        if (b->chunks[i].len > sizeof(key)-size) return OPJ_FALSE;
        size += b->chunks[i].len;
    }
    append(&p, header, sizeof(header));
    for (OPJ_UINT32 i=0; i<b->real_num_segs; ++i) {
        append(&p, &b->segs[i].len, 4); append(&p, &b->segs[i].real_num_passes, 4);
    }
    for (OPJ_UINT32 i=0; i<b->numchunks; ++i) {
        append(&p, &b->chunks[i].len, 4); append(&p, b->chunks[i].data, b->chunks[i].len);
    }
    OPJ_UINT32 hash=2166136261u;
    for (size_t i=0; i<size; ++i) hash=(hash^key[i])*16777619u;
    const unsigned slot=hash%SLOTS;
    opj_mutex_lock(c->mutex);
    entry *e=c->slots[slot];
    if (!store) {
        OPJ_BOOL hit=e && e->hash==hash && e->key_size==size && e->pixels_size==pixel_size &&
            memcmp(e->data+pixel_size,key,size)==0;
        if (hit) { memcpy(pixels,e->data,pixel_size); ++c->hits; }
        else ++c->misses;
        opj_mutex_unlock(c->mutex); return hit;
    }
    size_t need=sizeof(*e)+pixel_size+size;
    if (need>(BUDGET-sizeof(*c))/SLOTS) { opj_mutex_unlock(c->mutex); return OPJ_FALSE; }
    if (!e || e->capacity<need) {
        if (e) { c->bytes-=e->capacity; opj_free(e); }
        c->slots[slot]=e=opj_malloc(need);
        if (!e) { opj_mutex_unlock(c->mutex); return OPJ_FALSE; }
        e->capacity=need; c->bytes+=need;
    }
    e->hash=hash; e->key_size=(OPJ_UINT32)size; e->pixels_size=(OPJ_UINT32)pixel_size;
    memcpy(e->data,pixels,pixel_size); memcpy(e->data+pixel_size,key,size);
    opj_mutex_unlock(c->mutex); return OPJ_TRUE;
}
