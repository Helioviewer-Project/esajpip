/* hvc_jpp.c: see hvc_jpp.h. */
#include "hvc_jpp.h"

#include <string.h>

/* T.808 D.3 bounds a Bin-ID at 37 bits, and the header form reserves the top
 * two bits of the first byte for the presence field. Both limits are checked
 * here rather than trusted, because a client must not loop on a corrupt stream. */
#define HVC_JPP_MAX_BIN_ID ((UINT64_C(1) << 37) - 1)
#define HVC_JPP_MAX_VBAS_BITS 64

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
            reader->error = "Truncated JPP integer";
            return -1;
        }
        byte = reader->data[reader->position++];
        if (shifted >= HVC_JPP_MAX_VBAS_BITS) {
            reader->error = "Overflowing JPP integer";
            return -1;
        }
        /* Refuse a value that cannot fit in 64 bits rather than wrapping. */
        if (result > (UINT64_MAX >> 7)) {
            reader->error = "Overflowing JPP integer";
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
    /* Class and CSn start at zero for each response, as T.808 D.3 requires. */
    *reader = (hvc_jpp_reader){.data = data, .size = size};
}

int hvc_jpp_reason(const hvc_jpp_reader *reader) {
    return reader->reason;
}

const char *hvc_jpp_error(const hvc_jpp_reader *reader) {
    return reader->error != NULL ? reader->error : "";
}

int hvc_jpp_next(hvc_jpp_reader *reader, hvc_jpp_message *message) {
    uint8_t first;
    int presence;
    uint64_t bin_id, offset, length;

    reader->error = NULL;

    if (reader->position >= reader->size) {
        reader->error = "Response ended without an EOR message";
        return HVC_JPP_ERROR;
    }

    first = reader->data[reader->position++];

    /* An all-zero first byte opens the end-of-response message: reason in the
     * next byte, then the total remaining length, which must account for every
     * byte left so the end of the stream is verifiable. */
    if (first == 0) {
        uint64_t remaining;
        if (reader->position >= reader->size) {
            reader->error = "Truncated EOR message";
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
            reader->error = "Unsupported end-of-response reason";
            return HVC_JPP_ERROR;
        }
        if (ReadVbas(reader, 0, 0, 0, &remaining) < 0) return HVC_JPP_ERROR;
        /* The EOR length counts its own bytes after the reason, so the message
         * must end exactly at the end of the response. */
        if (remaining != reader->size - reader->position) {
            reader->error = "Missing or misplaced EOR boundary";
            return HVC_JPP_ERROR;
        }
        return HVC_JPP_EOR;
    }

    presence = (first >> 5) & 3;
    if (presence == 0) {
        reader->error = "Reserved JPP header form";
        return HVC_JPP_ERROR;
    }

    /* The low nibble of the first byte carries the top four Bin-ID bits, and
     * its high bit says more Bin-ID bytes follow. */
    if (ReadVbas(reader, (uint64_t) (first & 15), 1, (first & 0x80) != 0,
                 &bin_id) < 0)
        return HVC_JPP_ERROR;
    if (bin_id > HVC_JPP_MAX_BIN_ID) {
        reader->error = "JPP Bin-ID exceeds the standard limit";
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
        reader->error = "Extended data-bin class is not supported";
        return HVC_JPP_ERROR;
    }

    if (reader->cls > HVC_BIN_META_DATA) {
        reader->error = "Unsupported data-bin class";
        return HVC_JPP_ERROR;
    }

    if (length > reader->size - reader->position) {
        reader->error = "JPP message runs past the end of the response";
        return HVC_JPP_ERROR;
    }
    if (offset > UINT64_MAX - length) {
        reader->error = "JPP message range overflows";
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

    return HVC_JPP_MESSAGE;
}

/* Writes one VBAS integer and returns its length. */
static size_t WriteVbas(uint8_t *out, uint64_t value) {
    size_t length = 1, index;
    while (length < 10 && (value >> (7 * length)) != 0) length++;
    for (index = 0; index < length; index++)
        out[index] = (uint8_t) (((value >> (7 * (length - 1 - index))) & 0x7F) |
                                (index + 1 < length ? 0x80 : 0));
    return length;
}

size_t hvc_jpp_write(const hvc_jpp_message *message, uint8_t *out, size_t capacity) {
    /* 1 + 5 Bin-ID bytes, 4 integers of up to 10. */
    uint8_t header[46];
    size_t size = 1;
    unsigned shift = 0;

    while ((message->bin_id >> (shift + 4)) != 0) shift += 7;
    /* Presence 3: class and CSn explicit. */
    header[0] = (uint8_t) (0x60 | (message->last_byte ? 0x10 : 0) | (shift != 0 ? 0x80 : 0) |
                           ((message->bin_id >> shift) & 15));
    while (shift != 0) {
        shift -= 7;
        header[size++] = (uint8_t) (((message->bin_id >> shift) & 0x7F) | (shift != 0 ? 0x80 : 0));
    }
    size += WriteVbas(header + size, (uint64_t) message->bin_class);
    size += WriteVbas(header + size, message->codestream);
    size += WriteVbas(header + size, message->offset);
    size += WriteVbas(header + size, message->length);
    if (out != NULL && size <= capacity && message->length <= capacity - size) {
        memcpy(out, header, size);
        if (message->length != 0) memcpy(out + size, message->data, (size_t) message->length);
    }
    return size + (size_t) message->length;
}
