/* hvc_cache.h: the client's store of delivered data-bins.
 *
 * One source owns one store. Each data-bin is identified by its class,
 * codestream and Bin-ID, and arrives in messages that must append without a gap,
 * because the server tracks exactly the same prefixes: if the client and server
 * disagree about a bin's length, every later request is wrong. The store
 * therefore refuses any message that does not continue a bin where it left off,
 * rather than trying to reconcile the difference.
 *
 * A bin is complete once a message sets the last-byte flag. Bins stay in
 * memory for the life of the source, complete or not: the server will not
 * resend what it has sent, and it takes no notice that a client has dropped
 * something (JPIP_PROFILE.md: no subtractive cache model).
 * A replacement channel for the same immutable target can be synchronized
 * with hvc_cache_model without discarding the store.
 *
 * The store is a hash table on a bin's identity, so it holds the many
 * codestreams of a movie; memory is its only limit.
 */
#ifndef HVC_CACHE_H
#define HVC_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "hvc_jpp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One delivered data-bin. A bin grows by reallocation as later layers arrive. */
struct hvc_frame;
typedef struct {
    int      used;         /* this slot of the table holds a bin */
    int      bin_class;
    uint64_t codestream;
    uint64_t bin_id;
    uint8_t *data;
    size_t   length;
    size_t   capacity;
    int      complete;
    int      layers;       /* whole precinct packets confirmed by the host */
    size_t   packet_bytes; /* byte boundary of those packets */
    struct hvc_frame *frame; /* Prepared information for a main-header bin. */
} hvc_bin;

typedef struct {
    hvc_bin *bins;         /* the table: `capacity` slots, a power of two */
    size_t  count;        /* bins held */
    size_t  capacity;
    size_t  bytes;        /* total payload held */
    const char *error;   /* NULL unless the last hvc_cache_apply failed */
} hvc_cache;

/* Prepares an empty store. */
void hvc_cache_begin(hvc_cache *cache);

/* Applies one message. Returns 0 and sets hvc_cache_error() when the message
 * does not continue the bin the server believes it does, when memory runs out,
 * or when a message arrives for an already completed bin. A rejected message
 * leaves the store unchanged. */
int hvc_cache_apply(hvc_cache *cache, const hvc_jpp_message *message);

/* Looks a bin up, or NULL when the client has not received any of it. The
 * pointer is good until the next hvc_cache_apply(). */
const hvc_bin *hvc_cache_find(const hvc_cache *cache, int bin_class,
                               uint64_t codestream, uint64_t bin_id);

/* True when the named bin has been delivered in full. */
int hvc_cache_complete(const hvc_cache *cache, int bin_class, uint64_t codestream,
                       uint64_t bin_id);

/* How many bytes of a bin the client holds, which is what the server's cache
 * model should report for it. */
size_t hvc_cache_length(const hvc_cache *cache, int bin_class, uint64_t codestream,
                        uint64_t bin_id);

/* Counts of each kind held, for a status line in the host. */
size_t hvc_cache_bin_count(const hvc_cache *cache);
size_t hvc_cache_total_bytes(const hvc_cache *cache);
size_t hvc_cache_complete_count(const hvc_cache *cache);

/* Writes a comma-separated explicit JPIP cache model, in batches. Start
 * *cursor at zero and repeat until it reaches cache->capacity. Do not change
 * the cache between batches. Returns bytes written (excluding the NUL), or
 * -1 if a descriptor cannot fit or its bin class or length cannot be declared.
 * Bin identities must belong to the same supported target on the server.
 * Metadata must be complete: partial M0 declarations cannot restore the
 * server's placeholder traversal. */
int hvc_cache_model(const hvc_cache *cache, size_t *cursor, char *text, size_t size);

/* Checks a metadata range repeated while establishing a replacement channel.
 * It must match a complete cached bin exactly over that range. Never modifies
 * the cache or relaxes hvc_cache_apply's append-only contract. */
int hvc_cache_match_metadata(const hvc_cache *cache, const hvc_jpp_message *message);

/* A nonempty diagnostic after a failed hvc_cache_apply(). */
const char *hvc_cache_error(const hvc_cache *cache);

/* Releases every bin. Safe on an already-released store. */
void hvc_cache_release(hvc_cache *cache);

#ifdef __cplusplus
}
#endif
#endif /* HVC_CACHE_H */
