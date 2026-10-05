/* test_jpp_malformed.cc: checks that the JPP reader rejects damaged responses.
 *
 * Earlier versions of this test carried hand-written byte vectors. Those only
 * proved the reader agreed with whoever wrote them, and in the event the vectors
 * disagreed with the server's writer. The malformed inputs here are instead
 * derived from bytes the server's own jpip::DataBinWriter produced, by
 * truncation, single-byte corruption and extension. Structural errors must be rejected; payload changes remain opaque to the
 * parser. Each case checks the property it changes.
 */
#include "hvc_jpp.h"

#include <cstdio>
#include <cstring>

#include "jpip/index/place_holder.h"
#include "jpip/response/databin_writer.h"
#include "jpip/source/source.h"

static int failures;

/* Runs the reader over a body and reports whether it was rejected. */
static bool Rejected(const char *body, size_t size) {
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    int status;
    hvc_jpp_begin(&reader, (const uint8_t *) body, size);
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) {}
    return status == HVC_JPP_ERROR;
}

static void Check(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL %s\n", message);
        failures++;
    }
}

/* Builds a response with the server's writer: metadata, main header, an empty
 * tile header, and two precinct bins, ending in a window-done EOR. */
static ptrdiff_t BuildResponse(char *buffer, size_t capacity) {
    jpip::DataBinWriter writer;
    jpip::Source file("A-LONGER-SOURCE-STRING-FOR-PACKETS", 32);

    writer.SetBuffer(buffer, (int) capacity);
    writer.StartResponse();
    writer.Write(jpip::DataBinClass::META_DATA, 0, 0, 0, file,
                 jpip::FileSegment(0, 12));
    writer.Write(jpip::DataBinClass::MAIN_HEADER, 0, 0, 0, file,
                 jpip::FileSegment(12, 10));
    writer.Write(jpip::DataBinClass::TILE_HEADER, 0, 0, 0, file,
                 jpip::FileSegment::Null, true);
    writer.Write(jpip::DataBinClass::PRECINCT, 0, 0, 0, file,
                 jpip::FileSegment(22, 10));
    writer.Write(jpip::DataBinClass::PRECINCT, 0, 0, 10, file,
                 jpip::FileSegment(0, 10), true);
    writer.WriteEOR(jpip::EOR::WINDOW_DONE);
    return writer.Finalize();
}

/* The valid response must parse, or the mutations below prove nothing. */
static void TestValidBaseline(void) {
    char buffer[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    int status;
    int count = 0;

    hvc_jpp_begin(&reader, (const uint8_t *) buffer, (size_t) size);
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) count++;
    Check(status == HVC_JPP_EOR, "baseline response ends with an EOR");
    /* The writer coalesces the second precinct message into the first where the
     * ranges allow it, so the count is checked only for being the expected
     * number of data-bins rather than of messages. */
    Check(count == 4, "baseline response message count");
}

/* Every prefix of a valid response is incomplete and must be rejected. */
static void TestEveryTruncation(void) {
    char buffer[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    ptrdiff_t cut;
    int accepted = 0;

    for (cut = 1; cut < size; cut++) {
        if (!Rejected(buffer, (size_t) cut)) {
            std::printf("FAIL truncation to %td bytes was accepted\n", (long) cut);
            failures++;
            accepted = 1;
            break;
        }
    }
    if (!accepted)
        std::printf("  all %td truncations rejected\n", (long) (size - 1));
}

/* Appending bytes after the EOR leaves an incomplete-looking response, and the
 * EOR length no longer reaches the end, so it must be rejected. */
static void TestExtensionsRejected(void) {
    char buffer[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    char extended[4200];

    std::memcpy(extended, buffer, (size_t) size);
    extended[size] = 0;
    Check(Rejected(extended, (size_t) size + 1), "one trailing byte rejected");
    std::memset(extended + size + 1, 0x41, 32);
    Check(Rejected(extended, (size_t) size + 33), "trailing bytes rejected");
}

/* Payload bytes are opaque: changing one must preserve the message fields. */
static void TestPayloadChanges(void) {
    char buffer[4096], changed[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    hvc_jpp_begin(&reader, (const uint8_t *)buffer, (size_t)size);
    while (hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE) {
        for (size_t i = 0; i < message.length; i++) {
            std::memcpy(changed, buffer, (size_t)size);
            size_t at = (size_t)(message.data - (const uint8_t *)buffer) + i;
            changed[at] ^= 0x7F;
            hvc_jpp_reader original, modified;
            hvc_jpp_message a, b;
            hvc_jpp_begin(&original, (const uint8_t *)buffer, (size_t)size);
            hvc_jpp_begin(&modified, (const uint8_t *)changed, (size_t)size);
            int status;
            while ((status = hvc_jpp_next(&original, &a)) == HVC_JPP_MESSAGE) {
                Check(hvc_jpp_next(&modified, &b) == HVC_JPP_MESSAGE &&
                      a.bin_class == b.bin_class && a.codestream == b.codestream &&
                      a.bin_id == b.bin_id && a.offset == b.offset &&
                      a.length == b.length && a.last_byte == b.last_byte,
                      "payload change preserves message structure");
            }
            Check(status == HVC_JPP_EOR && hvc_jpp_next(&modified, &b) == HVC_JPP_EOR &&
                  hvc_jpp_reason(&original) == hvc_jpp_reason(&modified),
                  "payload change preserves response completion");
        }
    }
}

/* A data-bin class with its low bit set is an extended class, outside this
 * profile, and must be refused rather than treated as a standard class. */
static void TestExtendedClassRejected(void) {
    char buffer[4096];
    char mutated[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    ptrdiff_t index;

    /* The second byte of the first message header carries the class as VBAS. */
    std::memcpy(mutated, buffer, (size_t) size);
    mutated[1] = 7;
    Check(Rejected(mutated, (size_t) size), "extended data-bin class rejected");

    /* The same for a later message, where the class is the first byte after the
     * Bin-ID. Find a message whose class byte is a small value and set its low
     * bit. */
    std::memcpy(mutated, buffer, (size_t) size);
    for (index = 1; index < size; index++) {
        if ((unsigned char) mutated[index] == 6) { /* MAIN_HEADER */
            mutated[index] = 7;
            Check(Rejected(mutated, (size_t) size),
                  "extended class in a later message rejected");
            break;
        }
    }
}

static void TestUnsupportedClassRejected(void) {
    // Class 2^32 + 8 used to wrap to metadata class 8 on conversion to int.
    const uint8_t wrapped[] = {0x50, 0x90, 0x80, 0x80, 0x80, 0x08,
                               0, 1, 'x', 0, HVC_EOR_WINDOW_DONE, 0};
    Check(Rejected((const char *)wrapped, sizeof wrapped), "class wrapping to metadata rejected");
    const uint8_t unknown[] = {0x50, 10, 0, 1, 'x', 0, HVC_EOR_WINDOW_DONE, 0};
    Check(Rejected((const char *)unknown, sizeof unknown), "unknown even class rejected");
}

/* A Bin-ID larger than T.808 allows must be refused rather than accepted as a
 * very large identifier that would silently address the wrong bin. */
static void TestOversizedBinIdRejected(void) {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("", 0);
    writer.SetBuffer(buffer, sizeof buffer);
    writer.StartResponse();
    Check(writer.Write(jpip::DataBinClass::PRECINCT, 0, UINT64_C(1) << 37, 0,
                       file, jpip::FileSegment::Null, true) == jpip::DataBinWriter::Result::WRITTEN,
          "writer supplies bin-id boundary case");
    writer.WriteEOR(jpip::EOR::WINDOW_DONE);
    ptrdiff_t size = writer.Finalize();
    Check(Rejected(buffer, (size_t)size), "bin-id above the profile limit rejected");
}

/* A length field claiming more bytes than remain must not cause a read past the
 * end of the buffer. */
static void TestOverlongLengthRejected(void) {
    char buffer[4096];
    ptrdiff_t size = BuildResponse(buffer, sizeof buffer);
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    hvc_jpp_begin(&reader, (const uint8_t *)buffer, (size_t)size);
    if (hvc_jpp_next(&reader, &message) != HVC_JPP_MESSAGE || message.length != 12) {
        Check(false, "length test has its expected first message");
        return;
    }
    // The first payload is 12 bytes, so its length occupies the preceding byte.
    size_t at = (size_t)(message.data - (const uint8_t *)buffer) - 1;
    Check(buffer[at] == 12, "first payload length byte located");
    buffer[at] = 127;
    Check(Rejected(buffer, (size_t)size), "length exceeding the response rejected");
}

/* An empty response is not a valid one: it has no EOR. */
static void TestEmptyRejected(void) {
    Check(Rejected("", 0), "empty response rejected");
    /* An EOR message needs its reason byte and a length; three zero bytes carry
     * a reason of zero and no terminating length, so the boundary cannot be
     * verified. */
    char zero[3] = {0, 0, 0};
    Check(Rejected(zero, 3), "EOR without a length rejected");
    /* A lone opening byte, with nothing after it. */
    char lone[1] = {0};
    Check(Rejected(lone, 1), "truncated EOR rejected");
}

int main(void) {
    TestValidBaseline();
    TestEveryTruncation();
    TestExtensionsRejected();
    TestExtendedClassRejected();
    TestUnsupportedClassRejected();
    TestOversizedBinIdRejected();
    TestOverlongLengthRejected();
    TestEmptyRejected();
    TestPayloadChanges();
    if (failures) {
        std::printf("%d malformed-response check(s) failed\n", failures);
        return 1;
    }
    std::printf("JPP malformed-response checks passed\n");
    return 0;
}