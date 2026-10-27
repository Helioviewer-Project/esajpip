/* Exact byte-verified reuse, before ROI shift/dequantization. No MQ state is
 * retained. Cache allocation failure is a miss, never a decoding failure. */
#define OPJ_SKIP_POISON
#include "opj_includes.h"
#include "t1_cache.h"
/* Direct-mapped, so keep the load low: a 4096x4096 frame caches about 190
 * blocks, those up to its 1024x1024 resolution. */
#define SLOTS 1024
typedef struct {
    OPJ_UINT32 hash, key_size, pixels_size;
    size_t capacity;
    unsigned char data[]; /* raw int32 coefficients, then exact serialized key */
} entry;
#define PIXELS_MAX 16384
struct opj_t1_cache {
    opj_mutex_t *mutex;
    entry *slots[SLOTS];
    size_t bytes, hits, misses;
};
opj_t1_cache *opj_t1_cache_create(void) {
    opj_t1_cache *c = opj_calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->mutex = opj_mutex_create();
    if (!c->mutex && opj_has_thread_support()) { opj_free(c); return NULL; }
    c->bytes = sizeof(*c);
    return c;
}
void opj_t1_cache_destroy(opj_t1_cache *c) {
    if (!c) return;
    for (unsigned i=0; i<SLOTS; ++i) opj_free(c->slots[i]);
    opj_mutex_destroy(c->mutex); opj_free(c);
}
size_t opj_t1_cache_stat(const opj_t1_cache *c, unsigned which) {
    /* Called by coordinator only after the pool has drained. */
    return !c ? 0 : which == 0 ? c->bytes : which == 1 ? c->hits : c->misses;
}
static void append(unsigned char **p, const void *data, size_t n) {
    memcpy(*p, data, n); *p += n;
}
OPJ_BOOL opj_t1_cache_lookup(opj_t1_cache *c, const opj_tcd_cblk_dec_t *b,
    OPJ_UINT32 orient, OPJ_UINT32 roishift, OPJ_UINT32 style, OPJ_INT32 *pixels, opj_t1_cache_key *key) {
    /* Small low-resolution blocks only. Stack key is bounded independently of
     * untrusted chunk counts/lengths, and never survives this job. */
    unsigned char *p = key->data;
    OPJ_UINT32 header[] = {(OPJ_UINT32)(b->x1-b->x0), (OPJ_UINT32)(b->y1-b->y0),
        orient, roishift, style, b->numbps, b->Mb, b->real_num_segs, b->numchunks};
    size_t size = sizeof(header), pixel_size = (size_t)header[0]*header[1]*sizeof(*pixels);
    key->size = 0;
    if (!c || b->corrupted || !b->numchunks || header[0]>1024 || header[1]>1024 || !pixel_size || pixel_size>PIXELS_MAX ||
        b->real_num_segs > (sizeof(key->data)-size)/8) return OPJ_FALSE;
    size += (size_t)b->real_num_segs*8;
    if (b->numchunks > (sizeof(key->data)-size)/4) return OPJ_FALSE;
    size += (size_t)b->numchunks*4;
    for (OPJ_UINT32 i=0; i<b->numchunks; ++i) {
        if (b->chunks[i].len > sizeof(key->data)-size) return OPJ_FALSE;
        size += b->chunks[i].len;
    }
    append(&p, header, sizeof(header));
    for (OPJ_UINT32 i=0; i<b->real_num_segs; ++i) {
        append(&p, &b->segs[i].len, 4); append(&p, &b->segs[i].real_num_passes, 4);
    }
    for (OPJ_UINT32 i=0; i<b->numchunks; ++i) {
        append(&p, &b->chunks[i].len, 4); append(&p, b->chunks[i].data, b->chunks[i].len);
    }
    /* Eight bytes per multiply: hashing all compressed bytes of every cached
     * block one byte at a time cost 3-4% of a decode without hits. Matches
     * are compared in full, so only the spread across slots matters. */
    OPJ_UINT64 h=size, w;
    size_t i=0;
    for (; i+8<=size; i+=8) { memcpy(&w,key->data+i,8); h=(h^w)*0x9e3779b97f4a7c15u; h^=h>>29; }
    for (; i<size; ++i) h=(h^key->data[i])*0x100000001b3u;
    OPJ_UINT32 hash=(OPJ_UINT32)(h^(h>>32));
    key->hash = hash; key->size = (OPJ_UINT32)size; key->pixels_size = (OPJ_UINT32)pixel_size;
    const unsigned slot=hash%SLOTS;
    opj_mutex_lock(c->mutex);
    entry *e=c->slots[slot];
    OPJ_BOOL hit=e && e->hash==hash && e->key_size==size && e->pixels_size==pixel_size &&
        memcmp(e->data+pixel_size,key->data,size)==0;
    if (hit) { memcpy(pixels,e->data,pixel_size); ++c->hits; }
    else ++c->misses;
    opj_mutex_unlock(c->mutex); return hit;
}
OPJ_BOOL opj_t1_cache_store(opj_t1_cache *c, const opj_tcd_cblk_dec_t *b,
    const opj_t1_cache_key *key, const OPJ_INT32 *pixels) {
    if (!c || b->corrupted || !key->size) return OPJ_FALSE;
    size_t size=key->size, pixel_size=key->pixels_size;
    const unsigned slot=key->hash%SLOTS;
    opj_mutex_lock(c->mutex);
    entry *e=c->slots[slot];
    size_t need=sizeof(*e)+pixel_size+size;
    if (!e || e->capacity<need) {
        /* A full cache keeps its entries rather than replace them. */
        if (c->bytes-(e ? e->capacity : 0)+need > OPJ_T1_CACHE_BUDGET) { opj_mutex_unlock(c->mutex); return OPJ_FALSE; }
        if (e) { c->bytes-=e->capacity; opj_free(e); }
        c->slots[slot]=e=opj_malloc(need);
        if (!e) { opj_mutex_unlock(c->mutex); return OPJ_FALSE; }
        e->capacity=need; c->bytes+=need;
    }
    e->hash=key->hash; e->key_size=(OPJ_UINT32)size; e->pixels_size=(OPJ_UINT32)pixel_size;
    memcpy(e->data,pixels,pixel_size); memcpy(e->data+pixel_size,key->data,size);
    opj_mutex_unlock(c->mutex); return OPJ_TRUE;
}
