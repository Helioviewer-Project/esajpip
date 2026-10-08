/* hvc_jpp.h: JPP-stream message reading and writing (T.808 A.2 and D.3).
 *
 * A JPP stream is a sequence of messages. Each message delivers a byte range
 * of one data-bin, and a response ends with one end-of-response message. This
 * parser is the client counterpart of the server's jpip/response/databin_writer.cc
 * and follows the same field layout, but validates rather than generates.
 *
 * The parser holds no allocation beyond one caller-supplied buffer and reports
 * every failure through hvc_jpp_error(). A partially parsed response is never
 * left half-committed: hvc_jpp_next() either returns a complete message or an
 * error, and the bin store is updated only once the payload has been accepted.
 *
 * Class and CSn are inherited across messages within one response, as the
 * standard specifies; hvc_jpp_begin() resets that state.
 */
#ifndef HVC_JPP_H
#define HVC_JPP_H

#include <stddef.h>
#include <stdint.h>
#include "hvc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Data-bin classes, from jpip/jpip.h in the server tree. The client keeps its
 * own copy so the module does not link the C++ library. */
enum {
    HVC_BIN_PRECINCT = 0,
    HVC_BIN_EXTENDED_PRECINCT = 1,
    HVC_BIN_TILE_HEADER = 2,
    HVC_BIN_TILE_DATA = 4,
    HVC_BIN_EXTENDED_TILE = 5,
    HVC_BIN_MAIN_HEADER = 6,
    HVC_BIN_META_DATA = 8
};

/* One delivered data-bin range. The payload is inside the buffer the caller
 * passed to hvc_jpp_next(); it stays valid until the next call. */
typedef struct {
    int      bin_class;    /* data-bin class, after inheritance */
    uint64_t codestream;   /* CSn, after inheritance */
    uint64_t bin_id;       /* data-bin identifier */
    uint64_t offset;       /* offset within the data-bin */
    uint64_t length;       /* length of this message's payload */
    int      last_byte;    /* nonzero when this message ends the data-bin */
    const uint8_t *data;   /* payload, length bytes */
} hvc_jpp_message;

/* Result of a read. */
enum {
    HVC_JPP_MESSAGE = 0,   /* a message was produced */
    HVC_JPP_EOR,           /* end of response; check hvc_jpp_reason() */
    HVC_JPP_ERROR          /* hvc_jpp_error() describes the failure */
};

typedef struct {
    /* Input window. hvc_jpp_next() never reads past data + size, and reports a
     * truncated message as HVC_JPP_ERROR rather than returning partial data. */
    const uint8_t *data;
    size_t         size;
    size_t         position;

    /* Per-response inheritance state, reset by hvc_jpp_begin(). */
    uint64_t       cls;
    uint64_t       codestream;

    int      reason;       /* end-of-response reason once HVC_JPP_EOR is seen */
    const char *error;     /* NULL unless the last call failed */
} hvc_jpp_reader;

/* Prepares a reader over size bytes. Call once per response, or again for a
 * continuation response; class and CSn inheritance restarts each time. */
void hvc_jpp_begin(hvc_jpp_reader *reader, const uint8_t *data, size_t size);

/* Reads the next message. Returns HVC_JPP_MESSAGE, HVC_JPP_EOR at the end of the
 * response, or HVC_JPP_ERROR. An EOR message must be the last thing in the
 * response: trailing bytes after it are an error. */
int hvc_jpp_next(hvc_jpp_reader *reader, hvc_jpp_message *message);

/* The end-of-response reason after HVC_JPP_EOR. Meaningless otherwise. */
int hvc_jpp_reason(const hvc_jpp_reader *reader);

/* A nonempty diagnostic after HVC_JPP_ERROR, empty otherwise. */
const char *hvc_jpp_error(const hvc_jpp_reader *reader);

/* Writes one message with explicit class and CSn; Bin-ID within 37 bits.
 * Returns its size, written only when capacity holds it. */
size_t hvc_jpp_write(const hvc_jpp_message *message, uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif /* HVC_JPP_H */