/* Persistent decoder resources, independent of the client display adapter. */
#define OPJ_SKIP_POISON
#include "opj_includes.h"
#include "decoder_workspace.h"
#include "t1_cache.h"
struct opj_decoder_workspace {
    opj_thread_pool_t *pool, *original_pool;
    opj_t1_cache *codeblocks;
    struct {void *data; size_t capacity; int busy;} coefficients[3];
    size_t coefficient_bytes, coefficient_allocations, coefficient_hits;
};
/* Only the sequential coordinator lends this cache to OpenJPEG. Tier-1/DWT
 * worker scratch uses OpenJPEG's separate aligned allocator, not these hooks. */
static _Thread_local opj_decoder_workspace *active_workspace;
opj_t1_cache *opj_active_t1_cache(void) { return active_workspace ? active_workspace->codeblocks : NULL; }
size_t opj_decoder_workspace_codeblock_stat(const opj_decoder_workspace *d, unsigned which) { return opj_t1_cache_stat(d ? d->codeblocks : NULL, which); }
#define COEFFICIENT_BUDGET ((size_t)64 * 1024 * 1024)
void *opj_workspace_image_data_alloc(size_t size) {
    if (!size) return NULL;
    opj_decoder_workspace *d = active_workspace;
    if (!d || size > COEFFICIENT_BUDGET) return opj_aligned_malloc(size);
    int best = -1;
    for (int i = 0; i < 3; ++i) {
        if (!d->coefficients[i].busy && d->coefficients[i].capacity >= size &&
            (best < 0 || d->coefficients[i].capacity < d->coefficients[best].capacity)) best = i;
    }
    if (best >= 0) {
        d->coefficients[best].busy = 1; ++d->coefficient_hits;
        return d->coefficients[best].data;
    }
    /* Evict idle smaller blocks before growing; never move a live tile buffer. */
    for (int i = 0; i < 3; ++i) if (!d->coefficients[i].busy) {
        d->coefficient_bytes -= d->coefficients[i].capacity;
        opj_aligned_free(d->coefficients[i].data);
        memset(&d->coefficients[i], 0, sizeof d->coefficients[i]);
        best = i;
    }
    if (best < 0 || size > COEFFICIENT_BUDGET - d->coefficient_bytes) return opj_aligned_malloc(size);
    void *data = opj_aligned_malloc(size);
    if (!data) return NULL;
    d->coefficients[best].data = data; d->coefficients[best].capacity = size;
    d->coefficients[best].busy = 1; d->coefficient_bytes += size; ++d->coefficient_allocations;
    return data;
}
void opj_workspace_image_data_free(void *data) {
    if (!data) return;
    opj_decoder_workspace *d = active_workspace;
    if (d) for (int i = 0; i < 3; ++i) if (d->coefficients[i].data == data) {
        d->coefficients[i].busy = 0; return;
    }
    opj_aligned_free(data);
}

opj_decoder_workspace *opj_decoder_workspace_create(int threads) {
    if (threads < 0 || threads > 8 || (threads && !opj_has_thread_support())) return NULL;
    opj_decoder_workspace *workspace = opj_calloc(1, sizeof *workspace);
    if (!workspace) return NULL;
    workspace->pool = opj_thread_pool_create(threads);
    if (!workspace->pool) { opj_free(workspace); return NULL; }
    workspace->codeblocks = opj_t1_cache_create();
    return workspace;
}
void opj_decoder_workspace_destroy(opj_decoder_workspace *workspace) {
    if (!workspace) return;
    opj_thread_pool_destroy(workspace->pool);
    opj_t1_cache_destroy(workspace->codeblocks);
    for (int i = 0; i < 3; ++i) opj_aligned_free(workspace->coefficients[i].data);
    opj_free(workspace);
}
void opj_decoder_workspace_activate(opj_decoder_workspace *workspace) { active_workspace = workspace; }
void opj_decoder_workspace_bind(opj_decoder_workspace *workspace, opj_codec_t *codec) {
    opj_j2k_t *j2k = (opj_j2k_t *)((opj_codec_private_t *)codec)->m_codec;
    workspace->original_pool = j2k->m_tp;
    j2k->m_tp = workspace->pool;
}
void opj_decoder_workspace_unbind(opj_decoder_workspace *workspace, opj_codec_t *codec) {
    opj_decoder_workspace_wait(workspace);
    ((opj_j2k_t *)((opj_codec_private_t *)codec)->m_codec)->m_tp = workspace->original_pool;
}
size_t opj_decoder_workspace_retained(const opj_decoder_workspace *workspace) {
    return workspace ? workspace->coefficient_bytes + opj_t1_cache_stat(workspace->codeblocks, 0) : 0;
}
size_t opj_decoder_workspace_coefficient_allocations(const opj_decoder_workspace *workspace) { return workspace ? workspace->coefficient_allocations : 0; }
size_t opj_decoder_workspace_coefficient_hits(const opj_decoder_workspace *workspace) { return workspace ? workspace->coefficient_hits : 0; }
int opj_decoder_workspace_threads(const opj_decoder_workspace *workspace) { return opj_thread_pool_get_thread_count(workspace->pool); }
OPJ_BOOL opj_decoder_workspace_submit(opj_decoder_workspace *workspace, opj_job_fn function, void *user) {
    return opj_thread_pool_submit_job(workspace->pool, function, user);
}
void opj_decoder_workspace_wait(opj_decoder_workspace *workspace) { opj_thread_pool_wait_completion(workspace->pool, 0); }
size_t opj_decoder_gray8_size(opj_codec_t *codec, const opj_image_t *image, OPJ_UINT32 component, OPJ_UINT32 reduce) {
    const opj_j2k_t *j2k = (opj_j2k_t *)((opj_codec_private_t *)codec)->m_codec;
    const opj_cp_t *cp = &j2k->m_cp;
    const opj_image_comp_t *gray = &image->comps[component];
    if (!j2k->m_tcd || gray->prec > 16 || gray->dx != 1 || gray->dy != 1 ||
        cp->tw != 1 || cp->th != 1 || cp->tx0 != 0 || cp->ty0 != 0 || image->x0 != 0 || image->y0 != 0 ||
        image->x1 != cp->tdx || image->y1 != cp->tdy) return 0;
    return (size_t)opj_uint_ceildivpow2(image->x1, reduce) * opj_uint_ceildivpow2(image->y1, reduce);
}
void opj_decoder_gray8_output(opj_codec_t *codec, OPJ_UINT32 component, OPJ_BYTE *pixels) {
    opj_tcd_t *tcd = ((opj_j2k_t *)((opj_codec_private_t *)codec)->m_codec)->m_tcd;
    tcd->gray8 = pixels; tcd->gray8_compno = component;
}
