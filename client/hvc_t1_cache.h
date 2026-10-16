/* Bounded raw Tier-1 reuse. Include after opj_includes.h. */
#ifndef HVC_T1_CACHE_H
#define HVC_T1_CACHE_H
typedef struct hvc_t1_cache hvc_t1_cache;
hvc_t1_cache *hvc_t1_cache_create(void);
void hvc_t1_cache_destroy(hvc_t1_cache *cache);
/* Coordinator-only lookup; jobs capture the pointer before queue submission. */
hvc_t1_cache *hvc_active_t1_cache(void);
size_t hvc_t1_cache_stat(const hvc_t1_cache *cache, unsigned which);
OPJ_BOOL hvc_t1_cache_transfer(hvc_t1_cache *cache, const opj_tcd_cblk_dec_t *block,
    OPJ_UINT32 orient, OPJ_UINT32 roishift, OPJ_UINT32 style, OPJ_INT32 *pixels, OPJ_BOOL store);
#endif
