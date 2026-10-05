/* hvc_jpp.c: see hvc_jpp.h. */
#include "hvc_jpp.h"


/* T.808 D.3 bounds a Bin-ID at 37 bits, and the header form reserves the top
 * two bits of the first byte for the presence field. Both limits are checked
 * here rather than trusted, because a client must not loop on a corrupt stream. */
#define HVC_JPP_MAX_BIN_ID ((UINT64_C(1) << 37) - 1)
#define HVC_JPP_MAX_VBAS_BITS 64

static void Fail(hvc_jpp_reader *reader, const char *message) {
    reader->error = message;
}

/* Reads one VBAS integer, as T.808 A.2 encodes it: seven bits per byte, most
 * significant group first, high bit set on every byte but the last.
 *
 * A Bin-ID is the one field that does not start on a byte boundary. Its top four
 * bits sit in the low nibble of the JPP header's first byte, and that byte's own
 * high bit says whether further Bin-ID bytes follow. A caller that already
 * consumed those four bits passes them as seed and sets seeded; with the header
 * byte's continuation flag clear the Bin-ID is finished and no further byte is
 * read. Every other field leaves seeded clear, which reads a whole integer
 * starting at the current position. */
static int ReadVbas(hvc_jpp_reader *reader, uint64_t seed, int seeded,
                    int continuation, uint64_t *value) {
    uint64_t result = seeded ? seed : 0;
    int shifted = seeded ? 4 : 0;
    /* An unseeded read always takes at least one byte; a seeded one continues
     * only while the header byte said it did. */
    int more = seeded ? continuation : 1;

    while (more) {
        uint8_t byte;
        if (reader->position >= reader->size) {
            Fail(reader, "Truncated JPP integer");
            return -1;
        }
        byte = reader->data[reader->position++];
        if (shifted >= HVC_JPP_MAX_VBAS_BITS) {
            Fail(reader, "Overflowing JPP integer");
            return -1;
        }
        /* Refuse a value that cannot fit in 64 bits rather than wrapping. */
        if (result > (UINT64_MAX >> 7)) {
            Fail(reader, "Overflowing JPP integer");
            return -1;
        }
        result = (result << 7) | (byte & 0x7F);
        shifted += 7;
        more = (byte & 0x80) != 0;
    }
    *value = result;
    return 0;
}

void hvc_jpp_begin(hvc_jpp_reader *reader, const uint8_t *data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->position = 0;
    /* Class and CSn start at zero for each response, as T.808 D.3 requires. */
    reader->cls = 0;
    reader->codestream = 0;
    reader->have_previous = 0;
    reader->reason = 0;
    reader->error = NULL;
}

int hvc_jpp_reason(const hvc_jpp_reader *reader) {
    return reader->reason;
}

const char *hvc_jpp_error(const hvc_jpp_reader *reader) {
    return reader->error != NULL ? reader->error : "";
}

int hvc_jpp_reason_continues(int reason) {
    /* WINDOW_DONE and the two byte-budget limits leave the channel usable and
     * the window served as far as the server is willing to go. A window change
     * means the client itself superseded this window, and a session limit ends
     * it, so neither continues. */
    return reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_BYTE_LIMIT_REACHED ||
           reason == HVC_EOR_RESPONSE_LIMIT_REACHED || reason == HVC_EOR_IMAGE_DONE;
}

int hvc_jpp_next(hvc_jpp_reader *reader, hvc_jpp_message *message) {
    uint8_t first;
    int presence;
    uint64_t bin_id, offset, length;

    reader->error = NULL;

    if (reader->position >= reader->size) {
        Fail(reader, "Response ended without an EOR message");
        return HVC_JPP_ERROR;
    }

    first = reader->data[reader->position++];

    /* An all-zero first byte opens the end-of-response message: reason in the
     * next byte, then the total remaining length, which must account for every
     * byte left so the end of the stream is verifiable. */
    if (first == 0) {
        uint64_t remaining;
        if (reader->position >= reader->size) {
            Fail(reader, "Truncated EOR message");
            return HVC_JPP_ERROR;
        }
        reader->reason = reader->data[reader->position++];
        /* T.808 D.3 defines reasons 1 to 7 and the non-specified 0xFF. Zero is
         * not among them, so a stream carrying it is not one this client can
         * interpret; treating it as "keep going" would silently accept a
         * response whose completion is unknown. */
        if (reader->reason != HVC_EOR_NON_SPECIFIED &&
            (reader->reason < HVC_EOR_IMAGE_DONE ||
             reader->reason > HVC_EOR_RESPONSE_LIMIT_REACHED)) {
            Fail(reader, "Unsupported end-of-response reason");
            return HVC_JPP_ERROR;
        }
        if (ReadVbas(reader, 0, 0, 0, &remaining) < 0) return HVC_JPP_ERROR;
        /* The EOR length counts its own bytes after the reason, so the message
         * must end exactly at the end of the response. */
        if (remaining != reader->size - reader->position) {
            Fail(reader, "Missing or misplaced EOR boundary");
            return HVC_JPP_ERROR;
        }
        return HVC_JPP_EOR;
    }

    presence = (first >> 5) & 3;
    if (presence == 0) {
        Fail(reader, "Reserved JPP header form");
        return HVC_JPP_ERROR;
    }

    /* The low nibble of the first byte carries the top four Bin-ID bits, and
     * its high bit says more Bin-ID bytes follow. */
    if (ReadVbas(reader, (uint64_t) (first & 15), 1, (first & 0x80) != 0,
                 &bin_id) < 0)
        return HVC_JPP_ERROR;
    if (bin_id > HVC_JPP_MAX_BIN_ID) {
        Fail(reader, "JPP Bin-ID exceeds the standard limit");
        return HVC_JPP_ERROR;
    }

    if (presence >= 2 && ReadVbas(reader, 0, 0, 0, &reader->cls) < 0)
        return HVC_JPP_ERROR;
    if (presence == 3 && ReadVbas(reader, 0, 0, 0, &reader->codestream) < 0)
        return HVC_JPP_ERROR;

    if (ReadVbas(reader, 0, 0, 0, &offset) < 0) return HVC_JPP_ERROR;
    if (ReadVbas(reader, 0, 0, 0, &length) < 0) return HVC_JPP_ERROR;

    /* The even classes are the standard ones; the odd ones are extended and are
     * not part of this profile. */
    if ((reader->cls & 1) != 0) {
        Fail(reader, "Extended data-bin class is not supported");
        return HVC_JPP_ERROR;
    }

    if (reader->cls > HVC_BIN_META_DATA) {
        Fail(reader, "Unsupported data-bin class");
        return HVC_JPP_ERROR;
    }

    if (length > reader->size - reader->position) {
        Fail(reader, "JPP message runs past the end of the response");
        return HVC_JPP_ERROR;
    }
    if (offset > UINT64_MAX - length) {
        Fail(reader, "JPP message range overflows");
        return HVC_JPP_ERROR;
    }

    message->bin_class = (int) reader->cls;
    message->codestream = reader->codestream;
    message->bin_id = bin_id;
    message->offset = offset;
    message->length = length;
    message->last_byte = (first & 0x10) != 0;
    message->data = reader->data + reader->position;
    reader->position += (size_t) length;

    /* Remember the pair the next message may inherit. */
    reader->have_previous = 1;
    return HVC_JPP_MESSAGE;
}
