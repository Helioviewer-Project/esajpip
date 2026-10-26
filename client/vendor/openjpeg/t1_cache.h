/* Bounded raw Tier-1 reuse. Include after opj_includes.h. */
#ifndef OPJ_T1_CACHE_H
#define OPJ_T1_CACHE_H
typedef struct opj_t1_cache opj_t1_cache;
/* Bytes of entries, coefficients and keys, that a cache holds at most. */
#define OPJ_T1_CACHE_BUDGET (8u * 1024u * 1024u)
opj_t1_cache *opj_t1_cache_create(void);
void opj_t1_cache_destroy(opj_t1_cache *cache);
/* Coordinator-only lookup; jobs capture the pointer before queue submission. */
opj_t1_cache *opj_active_t1_cache(void);
size_t opj_t1_cache_stat(const opj_t1_cache *cache, unsigned which);
OPJ_BOOL opj_t1_cache_transfer(opj_t1_cache *cache, const opj_tcd_cblk_dec_t *block,
    OPJ_UINT32 orient, OPJ_UINT32 roishift, OPJ_UINT32 style, OPJ_INT32 *pixels, OPJ_BOOL store);
#endif
