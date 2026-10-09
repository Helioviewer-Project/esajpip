/* Checks append, replay, completion, allocation failure and cache bounds. */
#include "hvc_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile the cache here to inject resize failures without a production hook. */
static int fail_resize;
static void *Resize(void *data, size_t size) {
    if (fail_resize) { fail_resize = 0; return NULL; }
    return realloc(data, size);
}
#define realloc Resize
#include "../../client/hvc_cache.c"
#undef realloc

static int failures;

static void Check(int condition, const char *message) {
    if (!condition) {
        printf("FAIL %s\n", message);
        failures++;
    }
}

/* Builds a message delivering length bytes of the given payload at an offset. */
static hvc_jpp_message Message(int cls, uint64_t codestream, uint64_t bin_id,
                              uint64_t offset, const char *payload,
                              size_t length, int last) {
    hvc_jpp_message message;
    message.bin_class = cls;
    message.codestream = codestream;
    message.bin_id = bin_id;
    message.offset = offset;
    message.length = length;
    message.last_byte = last;
    message.data = (const uint8_t *) payload;
    return message;
}

static void TestContiguousAppend(void) {
    hvc_cache cache;
    hvc_jpp_message message;

    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_PRECINCT, 0, 5, 0, "abc", 3, 0);
    Check(hvc_cache_apply(&cache, &message), "first message accepted");
    message = Message(HVC_BIN_PRECINCT, 0, 5, 3, "def", 3, 0);
    Check(hvc_cache_apply(&cache, &message), "continuing message accepted");
    message = Message(HVC_BIN_PRECINCT, 0, 5, 6, "gh", 2, 1);
    Check(hvc_cache_apply(&cache, &message), "final message accepted");

    const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 5);
    Check(cache.count == 1 && bin != NULL && bin->length == 8 && bin->complete &&
          memcmp(bin->data, "abcdefgh", 8) == 0, "complete bin payload in order");
    hvc_cache_release(&cache);
}

static void TestGapsRejected(void) {
    hvc_cache cache;
    hvc_jpp_message message;

    /* A message that skips ahead of the recorded length would leave the client
     * and server disagreeing about the bin. */
    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_PRECINCT, 0, 1, 0, "abc", 3, 0);
    Check(hvc_cache_apply(&cache, &message), "seed accepted");
    message = Message(HVC_BIN_PRECINCT, 0, 1, 5, "xyz", 3, 0);
    Check(!hvc_cache_apply(&cache, &message), "gap rejected");
    Check(hvc_cache_error(&cache)[0] != 0, "gap has a diagnostic");

    /* Identical retransmissions leave the store unchanged. */
    message = Message(HVC_BIN_PRECINCT, 0, 1, 0, "abc", 3, 0);
    Check(hvc_cache_apply(&cache, &message), "retransmission accepted");

    /* The rejected messages must not have changed the store. */
    const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 1);
    Check(bin != NULL && bin->length == 3 && !bin->complete &&
          memcmp(bin->data, "abc", 3) == 0, "store unchanged after rejection");

    /* A completed bin takes no further bytes. */
    message = Message(HVC_BIN_PRECINCT, 0, 1, 3, "d", 1, 1);
    Check(hvc_cache_apply(&cache, &message), "bin completes");
    message = Message(HVC_BIN_PRECINCT, 0, 1, 4, "e", 1, 0);
    Check(!hvc_cache_apply(&cache, &message), "message after completion rejected");
    hvc_cache_release(&cache);
}

static void TestOverlap(void) {
    hvc_cache cache;
    hvc_cache_begin(&cache);
    hvc_jpp_message m = Message(HVC_BIN_PRECINCT, 0, 0, 0, "abcd", 4, 0);
    Check(hvc_cache_apply(&cache, &m), "overlap seed");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 1, "bc", 2, 0);
    Check(hvc_cache_apply(&cache, &m), "contained replay");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 2, "XXef", 4, 1);
    Check(!hvc_cache_apply(&cache, &m), "conflicting overlap rejected");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 0, "abc", 3, 1);
    Check(!hvc_cache_apply(&cache, &m), "premature final size rejected");
    hvc_bin *bin = (hvc_bin *)hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0);
    bin->layers = 1;
    bin->packet_bytes = 4;
    Check(bin->length == 4 && !bin->complete && memcmp(bin->data, "abcd", 4) == 0,
          "rejections preserve bytes and completion");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 2, "cdef", 4, 1);
    Check(hvc_cache_apply(&cache, &m), "overlap plus final suffix");
    Check(bin->length == 6 && bin->complete && memcmp(bin->data, "abcdef", 6) == 0 &&
          cache.count == 1, "overlap appends only the new suffix");
    Check(hvc_cache_apply(&cache, &m), "completed suffix replay");
    Check(bin->layers == 1 && bin->packet_bytes == 4, "replay preserves confirmed prefix");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 0, "abcdef", 6, 1);
    Check(hvc_cache_apply(&cache, &m), "completed full replay");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 1, "bc", 2, 0);
    Check(hvc_cache_apply(&cache, &m) && bin->complete, "replay preserves final flag");
    m = Message(HVC_BIN_PRECINCT, 0, 0, 4, "efg", 3, 0);
    Check(!hvc_cache_apply(&cache, &m), "completed bin extension rejected");
    m = Message(HVC_BIN_PRECINCT, 0, 0, UINT64_MAX, "x", 1, 0);
    Check(!hvc_cache_apply(&cache, &m), "overflow rejected");
    hvc_cache_release(&cache);
}

static void TestDistinctBins(void) {
    hvc_cache cache;
    hvc_jpp_message message;

    /* Identity is class, codestream and Bin-ID together, so the same id under a
     * different codestream is a different bin. */
    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_MAIN_HEADER, 0, 0, 0, "hdr", 3, 1);
    Check(hvc_cache_apply(&cache, &message), "main header accepted");
    message = Message(HVC_BIN_MAIN_HEADER, 1, 0, 0, "hdr2", 4, 1);
    Check(hvc_cache_apply(&cache, &message), "second codestream main header");
    message = Message(HVC_BIN_TILE_HEADER, 0, 0, 0, "", 0, 1);
    Check(hvc_cache_apply(&cache, &message), "empty tile header accepted");

    Check(cache.count == 3, "three distinct bins");
    const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, 0, 0);
    Check(bin != NULL && bin->length == 3 && bin->complete, "codestream 0 header");
    bin = hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, 1, 0);
    Check(bin != NULL && bin->length == 4 && bin->complete, "codestream 1 header");
    bin = hvc_cache_find(&cache, HVC_BIN_TILE_HEADER, 0, 0);
    Check(bin != NULL && bin->length == 0 && bin->complete, "empty tile header is complete");
    hvc_cache_release(&cache);
}

static void TestUnknownBin(void) {
    hvc_cache cache;

    hvc_cache_begin(&cache);
    Check(hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 99) == NULL, "unknown bin absent");
    Check(hvc_cache_error(&cache)[0] == 0, "no diagnostic without a failure");
    hvc_cache_release(&cache);
}

/* A precinct bin can arrive over many quality layers, so the store must grow
 * without losing earlier layers. */
static void TestGrowthKeepsContents(void) {
    hvc_cache cache;
    hvc_jpp_message message;
    char payload[512];
    size_t layer;
    size_t total = 0;

    hvc_cache_begin(&cache);
    for (layer = 0; layer < 40; layer++) {
        memset(payload, (int) ('a' + (layer % 26)), sizeof payload);
        message = Message(HVC_BIN_PRECINCT, 0, 0, total, payload, 300,
                          layer == 39);
        Check(hvc_cache_apply(&cache, &message), "layer accepted");
        total += 300;
    }
    {
        const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0);
        int intact = bin != NULL && bin->length == total;
        size_t index;
        for (index = 0; intact && index < total; index++) {
            if (bin->data[index] != (uint8_t) ('a' + ((index / 300) % 26)))
                intact = 0;
        }
        Check(intact, "every layer survived growth in order");
    }
    hvc_cache_release(&cache);
}

/* A length that cannot be satisfied must be reported rather than attempted. */
static void TestHostileLength(void) {
    hvc_cache cache;
    hvc_jpp_message message;

    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_PRECINCT, 0, 0, (uint64_t) -1 - 4, "ab", 8, 0);
    Check(!hvc_cache_apply(&cache, &message), "overflowing range rejected");
    Check(hvc_cache_error(&cache)[0] != 0, "overflow has a diagnostic");
    hvc_cache_release(&cache);
}

/* A movie's worth of bins: every one stays findable, with its own bytes, as
 * the table grows. */
static void TestManyBins(void) {
    enum { CODESTREAMS = 40, BINS = 1500 };
    hvc_cache cache;
    hvc_jpp_message message;
    uint64_t codestream, bin_id;
    int wrong = 0;

    hvc_cache_begin(&cache);
    for (codestream = 0; codestream < CODESTREAMS; codestream++) {
        for (bin_id = 0; bin_id < BINS; bin_id++) {
            uint8_t payload[2] = {(uint8_t) codestream, (uint8_t) bin_id};
            message = Message(HVC_BIN_PRECINCT, codestream, bin_id, 0, (const char *) payload, 2,
                              bin_id % 2);
            wrong += !hvc_cache_apply(&cache, &message);
        }
    }
    Check(!wrong, "many bins accepted");
    Check(cache.count == CODESTREAMS * BINS, "many bins counted");
    for (codestream = 0; codestream < CODESTREAMS; codestream++) {
        for (bin_id = 0; bin_id < BINS; bin_id++) {
            const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, codestream, bin_id);
            wrong += bin == NULL || bin->length != 2 || bin->data[0] != (uint8_t) codestream ||
                     bin->data[1] != (uint8_t) bin_id || bin->complete != (int) (bin_id % 2);
        }
    }
    Check(!wrong, "many bins found with their bytes");
    Check(hvc_cache_find(&cache, HVC_BIN_PRECINCT, CODESTREAMS, 0) == NULL &&
          hvc_cache_find(&cache, HVC_BIN_TILE_HEADER, 0, 0) == NULL, "other bins are unknown");
    hvc_cache_release(&cache);
    Check(cache.count == 0 && hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0) == NULL,
          "a released store is empty");
}

/* A message that is refused leaves no trace, not even an empty bin. */
static void TestRejectedLeavesNothing(void) {
    hvc_cache cache;
    hvc_jpp_message message;

    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_PRECINCT, 0, 7, 3, "abc", 3, 0);
    Check(!hvc_cache_apply(&cache, &message), "a bin cannot start past its first byte");
    Check(cache.count == 0 && hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 7) == NULL,
          "a refused message added no bin");
    hvc_cache_release(&cache);
}

static void TestRecoveryModel(void) {
    hvc_cache cache;
    hvc_jpp_message message;
    char model[20], all[256] = "";
    size_t cursor = 0;
    int batches = 0;
    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_META_DATA, 0, 0, 0, "metadata", 8, 1);
    Check(hvc_cache_apply(&cache, &message), "complete recovery metadata");
    Check(hvc_cache_match_metadata(&cache, &message), "identical metadata replay");
    Check(hvc_cache_apply(&cache, &message), "normal response accepts identical replay");
    message = Message(HVC_BIN_META_DATA, 0, 0, 2, "tada", 4, 0);
    Check(hvc_cache_match_metadata(&cache, &message), "identical metadata subrange");
    message.data = (const uint8_t *)"xxxx";
    Check(!hvc_cache_match_metadata(&cache, &message), "changed metadata rejected");
    message = Message(HVC_BIN_META_DATA, 0, 0, 2, "tadata", 6, 1);
    Check(hvc_cache_match_metadata(&cache, &message), "metadata final subrange");
    message.offset = 3;
    Check(!hvc_cache_match_metadata(&cache, &message), "metadata range beyond cache rejected");
    message = Message(HVC_BIN_META_DATA, 0, 0, 0, "meta", 4, 1);
    Check(!hvc_cache_match_metadata(&cache, &message), "early metadata completion rejected");
    message.bin_id = 1;
    Check(!hvc_cache_match_metadata(&cache, &message), "unknown metadata rejected");
    message = Message(HVC_BIN_MAIN_HEADER, 3, 0, 0, "hdr", 3, 1);
    Check(hvc_cache_apply(&cache, &message), "recovery main header");
    Check(!hvc_cache_match_metadata(&cache, &message), "restoration forbids image data");
    message = Message(HVC_BIN_TILE_HEADER, 3, 0, 0, "", 0, 1);
    Check(hvc_cache_apply(&cache, &message), "recovery empty complete header");
    message = Message(HVC_BIN_PRECINCT, 3, 42, 0, "abc", 3, 0);
    Check(hvc_cache_apply(&cache, &message), "recovery partial precinct");
    message = Message(HVC_BIN_PRECINCT, 3, 43, 0, "abc", 3, 1);
    Check(hvc_cache_apply(&cache, &message), "recovery complete precinct");
    while (cursor < cache.capacity) {
        int length = hvc_cache_model(&cache, &cursor, model, sizeof model);
        Check(length >= 0 && length == (int)strlen(model), "model batch length");
        if (length < 0) break;
        if (length == 0) continue;
        if (all[0]) strcat(all, ",");
        strcat(all, model);
        batches++;
    }
    Check(batches > 1, "model is split into bounded batches");
    Check(strstr(all, "M0") && strstr(all, "[3]Hm") && strstr(all, "[3]H0") &&
          strstr(all, "[3]P42:3") && strstr(all, "[3]P43"), "model preserves bin identities and prefixes");
    Check(!strstr(all, "M0:") && !strstr(all, "P43:"), "complete bins declare no byte count");
    cursor = 0;
    Check(hvc_cache_model(&cache, &cursor, model, 1) == -1, "undersized model buffer refused");
    message = Message(HVC_BIN_META_DATA, 0, 1, 0, "partial", 7, 0);
    Check(hvc_cache_apply(&cache, &message), "partial metadata seed");
    Check(!hvc_cache_match_metadata(&cache, &message), "incomplete metadata cannot be replayed");
    cursor = 0;
    Check(hvc_cache_model(&cache, &cursor, all, sizeof all) == -1, "partial metadata not advertised");
    const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_META_DATA, 0, 0);
    Check(bin != NULL && bin->complete && bin->length == 8 &&
          memcmp(bin->data, "metadata", 8) == 0, "recovery checks preserve cached metadata");
    hvc_cache_release(&cache);
    cursor = 0;
    Check(hvc_cache_model(&cache, &cursor, model, sizeof model) == 0 && model[0] == 0,
          "empty model terminates");
}

static void TestCompletedStorage(void) {
    hvc_cache cache;
    uint8_t payload[300] = {1, 2, 3};
    hvc_jpp_message message;
    const hvc_bin *bin;
    hvc_cache_begin(&cache);
    message = Message(HVC_BIN_PRECINCT, 0, 0, 0, (const char *)payload, 3, 0);
    Check(hvc_cache_apply(&cache, &message), "partial bin accepted");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0);
    size_t capacity = bin->capacity;
    fail_resize = 1;
    message = Message(HVC_BIN_PRECINCT, 0, 0, 3, NULL, 0, 1);
    Check(hvc_cache_apply(&cache, &message), "failed shrink still completes bin");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0);
    Check(bin->complete && bin->length == 3 && bin->capacity == capacity &&
          memcmp(bin->data, payload, 3) == 0, "failed shrink preserves bytes");

    message = Message(HVC_BIN_PRECINCT, 0, 1, 0, (const char *)payload, sizeof payload, 1);
    fail_resize = 1;
    Check(!hvc_cache_apply(&cache, &message) &&
          hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 1) == NULL,
          "failed allocation leaves new bin absent");
    Check(hvc_cache_apply(&cache, &message), "completed bin allocation retry succeeds");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 1);
    Check(bin->complete && bin->capacity == sizeof payload &&
          memcmp(bin->data, payload, sizeof payload) == 0, "completed bin is exactly sized");

    message = Message(HVC_BIN_PRECINCT, 0, 2, 0, (const char *)payload, 3, 0);
    Check(hvc_cache_apply(&cache, &message), "growth failure seed accepted");
    message = Message(HVC_BIN_PRECINCT, 0, 2, 3, (const char *)payload + 3, sizeof payload - 3, 1);
    fail_resize = 1;
    Check(!hvc_cache_apply(&cache, &message), "failed completed-bin growth rejected");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 2);
    Check(!bin->complete && bin->length == 3 && memcmp(bin->data, payload, 3) == 0,
          "failed growth preserves partial bytes");
    Check(hvc_cache_apply(&cache, &message), "completed-bin growth retry succeeds");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 2);
    Check(bin->complete && bin->capacity == sizeof payload &&
          memcmp(bin->data, payload, sizeof payload) == 0, "completion preserves appended bytes");
    message = Message(HVC_BIN_PRECINCT, 0, 3, 0, (const char *)payload, 3, 0);
    Check(hvc_cache_apply(&cache, &message), "shrink seed accepted");
    message = Message(HVC_BIN_PRECINCT, 0, 3, 3, NULL, 0, 1);
    Check(hvc_cache_apply(&cache, &message), "zero-length final message accepted");
    bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 3);
    Check(bin->complete && bin->capacity == 3 && memcmp(bin->data, payload, 3) == 0,
          "completed buffer shrinks without losing bytes");
    hvc_cache_release(&cache);
}

int main(void) {
    TestCompletedStorage();
    TestRecoveryModel();
    TestOverlap();
    TestContiguousAppend();
    TestGapsRejected();
    TestDistinctBins();
    TestUnknownBin();
    TestGrowthKeepsContents();
    TestHostileLength();
    TestManyBins();
    TestRejectedLeavesNothing();
    if (failures) {
        printf("%d data-bin store check(s) failed\n", failures);
        return 1;
    }
    printf("Data-bin store checks passed\n");
    return 0;
}
