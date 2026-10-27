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
/* One job's bounded scratch, retained across decoding on a miss. Coding
 * metadata and compressed bytes must remain unchanged through store; MQ's
 * temporary markers are restored before decoding returns. No slot is retained
 * or borrowed across the decode. A zero size means the block is ineligible. */
typedef struct {
    OPJ_UINT32 hash, size, pixels_size;
    unsigned char data[12288];
} opj_t1_cache_key;
OPJ_BOOL opj_t1_cache_lookup(opj_t1_cache *cache, const opj_tcd_cblk_dec_t *block,
    OPJ_UINT32 orient, OPJ_UINT32 roishift, OPJ_UINT32 style, OPJ_INT32 *pixels, opj_t1_cache_key *key);
/* Only after successful decoding, before ROI shift/dequantization. Rechecks
 * corruption and locks the current slot: other jobs may have replaced it. */
OPJ_BOOL opj_t1_cache_store(opj_t1_cache *cache, const opj_tcd_cblk_dec_t *block,
    const opj_t1_cache_key *key, const OPJ_INT32 *pixels);
#endif
