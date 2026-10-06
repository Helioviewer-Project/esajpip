/* test_jpp_roundtrip.cc: checks the JPP-stream reader against bytes produced by
 * the server's own writer, jpip::DataBinWriter.
 *
 * Hand-written JPP vectors only prove the parser agrees with the author's
 * understanding of T.808 D.3. These cases instead drive the authoritative
 * encoder in this repository and read its output back, so the client and the
 * server are known to agree on the wire format, including the header-growth
 * rewrite the writer performs when it coalesces adjacent writes.
 *
 * This test is native and links the C++ jpip library. The reader it exercises is
 * the same C code the wasm module runs.
 */
#include "hvc_jpp.h"

#include <cstdio>
#include <cstring>

#include "jpip/index/place_holder.h"
#include "jpip/response/databin_writer.h"
#include "jpip/source/source.h"

static int failures;

static void Check(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL %s\n", message);
        failures++;
    }
}

/* What the reader produced for one response. */
struct Seen {
    int count;
    struct {
        int cls;
        uint64_t codestream;
        uint64_t bin_id;
        uint64_t offset;
        uint64_t length;
        int last;
        uint8_t data[256];
    } message[64];
    int reason;
    int error;
};

static void Read(const uint8_t *body, size_t size, struct Seen *seen) {
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    int status;

    std::memset(seen, 0, sizeof *seen);
    hvc_jpp_begin(&reader, body, size);
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) {
        if (seen->count >= 64) {
            std::printf("FAIL too many messages\n");
            failures++;
            return;
        }
        int index = seen->count++;
        seen->message[index].cls = message.bin_class;
        seen->message[index].codestream = message.codestream;
        seen->message[index].bin_id = message.bin_id;
        seen->message[index].offset = message.offset;
        seen->message[index].length = message.length;
        seen->message[index].last = message.last_byte;
        std::memcpy(seen->message[index].data, message.data,
                    (size_t) message.length);
    }
    if (status == HVC_JPP_ERROR) {
        seen->error = 1;
        std::printf("FAIL reader rejected valid writer output: %s\n",
                    hvc_jpp_error(&reader));
        failures++;
        return;
    }
    seen->reason = hvc_jpp_reason(&reader);
}

/* Metadata and main-header bins, which open a channel's first response, plus
 * the empty complete tile header the server sends for tile 0. */
static void TestHeadersAndMetadata() {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("METADATA-AND-HEADER-BYTES", 24);
    ptrdiff_t used;
    struct Seen seen;

    writer.SetBuffer(buffer, (int) sizeof buffer);
    writer.StartResponse();
    Check(writer.Write(jpip::DataBinClass::META_DATA, 0, 0, 0, file,
                       jpip::FileSegment(0, 10)) ==
              jpip::DataBinWriter::Result::WRITTEN,
          "writer emits metadata bin");
    Check(writer.Write(jpip::DataBinClass::MAIN_HEADER, 0, 0, 0, file,
                       jpip::FileSegment(10, 8)) ==
              jpip::DataBinWriter::Result::WRITTEN,
          "writer emits main header bin");
    Check(writer.Write(jpip::DataBinClass::TILE_HEADER, 0, 0, 0, file,
                       jpip::FileSegment::Null, true) ==
              jpip::DataBinWriter::Result::WRITTEN,
          "writer emits empty tile header bin");
    Check(writer.WriteEOR(jpip::EOR::WINDOW_DONE), "writer emits EOR");
    used = writer.Finalize();

    Read((const uint8_t *) buffer, (size_t) used, &seen);
    Check(!seen.error, "headers response parses");
    Check(seen.reason == jpip::EOR::WINDOW_DONE, "headers response reason");
    Check(seen.count == 3, "headers response message count");
    if (seen.count == 3) {
        Check(seen.message[0].cls == jpip::DataBinClass::META_DATA,
              "first message is metadata");
        Check(seen.message[0].length == 10, "metadata bin length");
        Check(std::memcmp(seen.message[0].data, "METADATA-A", 10) == 0,
              "metadata bin payload");
        Check(seen.message[1].cls == jpip::DataBinClass::MAIN_HEADER,
              "second message is the main header");
        Check(std::memcmp(seen.message[1].data, "ND-HEADER", 8) == 0,
              "main header payload comes from the source range");
        Check(seen.message[2].cls == jpip::DataBinClass::TILE_HEADER,
              "third message is the tile header");
        Check(seen.message[2].length == 0, "tile header bin is empty");
        Check(seen.message[2].last, "empty tile header bin is complete");
    }
}

/* Precinct bins split across several messages, with class and codestream
 * inherited between them, which is how a window response is fragmented. */
static void TestPrecinctFragments() {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("0123456789abcdefghij", 20);
    ptrdiff_t used;
    struct Seen seen;

    writer.SetBuffer(buffer, (int) sizeof buffer);
    writer.StartResponse();
    /* First packet of precinct bin 0 in codestream 2. */
    writer.Write(jpip::DataBinClass::PRECINCT, 2, 0, 0, file,
                 jpip::FileSegment(0, 6));
    /* The next layer of the same bin, at offset 6. */
    writer.Write(jpip::DataBinClass::PRECINCT, 2, 0, 6, file,
                 jpip::FileSegment(6, 6));
    /* A third contribution, which the writer coalesces into the previous
     * message and whose header it then rewrites. */
    writer.Write(jpip::DataBinClass::PRECINCT, 2, 0, 12, file,
                 jpip::FileSegment(12, 8), true);
    /* A different precinct bin in the same codestream. */
    writer.Write(jpip::DataBinClass::PRECINCT, 2, 1, 0, file,
                 jpip::FileSegment(0, 4), true);
    writer.WriteEOR(jpip::EOR::WINDOW_DONE);
    used = writer.Finalize();

    Read((const uint8_t *) buffer, (size_t) used, &seen);
    Check(!seen.error, "precinct response parses");
    Check(seen.count == 2, "precinct response coalesces to two messages");
    if (seen.count == 2) {
        Check(seen.message[0].cls == jpip::DataBinClass::PRECINCT,
              "precinct class");
        Check(seen.message[0].codestream == 2, "precinct codestream");
        Check(seen.message[0].bin_id == 0, "precinct bin id");
        Check(seen.message[0].offset == 0, "precinct bin starts at zero");
        Check(seen.message[0].length == 20, "coalesced precinct length");
        Check(std::memcmp(seen.message[0].data, "0123456789abcdefghij", 20) == 0,
              "coalesced precinct payload");
        Check(seen.message[0].last, "coalesced precinct bin is complete");
        Check(seen.message[1].cls == jpip::DataBinClass::PRECINCT,
              "second message inherits its class");
        Check(seen.message[1].codestream == 2, "second message inherits CSn");
        Check(seen.message[1].bin_id == 1, "second precinct bin id");
        Check(seen.message[1].length == 4, "second precinct length");
        Check(seen.message[1].last, "second precinct bin is complete");
    }
}

/* A response cut short by the len budget ends with a limit reason, and the
 * channel stays usable afterwards. */
static void TestLimitReason() {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("PARTIAL", 7);
    ptrdiff_t used;
    struct Seen seen;

    writer.SetBuffer(buffer, (int) sizeof buffer);
    writer.StartResponse();
    writer.Write(jpip::DataBinClass::PRECINCT, 0, 0, 0, file,
                 jpip::FileSegment(0, 7));
    writer.WriteEOR(jpip::EOR::BYTE_LIMIT_REACHED);
    used = writer.Finalize();

    Read((const uint8_t *) buffer, (size_t) used, &seen);
    Check(!seen.error, "limited response parses");
    Check(seen.reason == jpip::EOR::BYTE_LIMIT_REACHED, "byte limit reason");
    Check(hvc_jpp_reason_continues(seen.reason),
          "byte limit leaves the channel usable");
}

/* Metadata placeholders: the server synthesises phld boxes, so the client must
 * accept a bin whose bytes the server generated rather than read from a file. */
static void TestPlaceholderBin() {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("jP  ", 4);
    ptrdiff_t used;
    struct Seen seen;
    jpip::PlaceHolder place_holder(0, true, jpip::FileSegment(0, 4));

    writer.SetBuffer(buffer, (int) sizeof buffer);
    writer.StartResponse();
    writer.WritePlaceHolder(jpip::DataBinClass::META_DATA, 0, 0, 0, file,
                            place_holder);
    writer.WriteEOR(jpip::EOR::WINDOW_DONE);
    used = writer.Finalize();

    Read((const uint8_t *) buffer, (size_t) used, &seen);
    Check(!seen.error, "placeholder bin parses");
    Check(seen.count == 1, "placeholder bin message count");
    if (seen.count == 1) {
        /* A jp2c placeholder is the phld box plus the caller's header bytes, at
         * the total length jpip::PlaceHolder::length() reports. The box runs
         * LBox, TBox, Flags, OrigID, EquivID, EquivBH and CSID before the
         * caller's own header bytes, then zero padding out to its length. */
        Check(seen.message[0].length == place_holder.length(),
              "placeholder bin length");
        Check(std::memcmp(seen.message[0].data + 4, "phld", 4) == 0,
              "placeholder box type");
        /* The caller's header bytes are the jp2c box header the source file held
         * in place of the codestream. */
        Check(std::memcmp(seen.message[0].data + 20, "jP  ", 4) == 0,
              "placeholder carries its header bytes");
    }
}

/* Every truncation of a valid response must be rejected: a response that ends
 * mid-message cannot be applied to the client's bins. */
static void TestTruncations() {
    char buffer[4096];
    jpip::DataBinWriter writer;
    jpip::Source file("TRUNCATE-ME-PLEASE", 17);
    ptrdiff_t used;
    ptrdiff_t cut;

    writer.SetBuffer(buffer, (int) sizeof buffer);
    writer.StartResponse();
    writer.Write(jpip::DataBinClass::PRECINCT, 0, 0, 0, file,
                 jpip::FileSegment(0, 17), true);
    writer.WriteEOR(jpip::EOR::WINDOW_DONE);
    used = writer.Finalize();

    for (cut = 1; cut < used; cut++) {
        hvc_jpp_reader reader;
        hvc_jpp_message message;
        int status;
        hvc_jpp_begin(&reader, (const uint8_t *) buffer, (size_t) cut);
        while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE) {}
        if (status != HVC_JPP_ERROR) {
            Check(false, "truncated response is rejected");
            return;
        }
    }
}

// Offset fields use the whole uint64_t range, even though the server's
// supported image sizes need much less. Zero payload avoids range addition.
static void TestIntegerBoundaries() {
    const uint64_t values[] = {0, 127, 128, UINT64_C(1) << 63, UINT64_MAX};
    for (uint64_t value : values) {
        char buffer[128];
        jpip::DataBinWriter writer;
        jpip::Source file("", 0);
        writer.SetBuffer(buffer, sizeof buffer);
        writer.StartResponse();
        Check(writer.Write(jpip::DataBinClass::PRECINCT, 0, 0, value, file,
                           jpip::FileSegment::Null, true) == jpip::DataBinWriter::Result::WRITTEN,
              "writer emits integer boundary");
        writer.WriteEOR(jpip::EOR::WINDOW_DONE);
        hvc_jpp_reader reader;
        hvc_jpp_message message;
        hvc_jpp_begin(&reader, (const uint8_t *)buffer, (size_t)writer.Finalize());
        Check(hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE && message.offset == value,
              "integer boundary round trips");
        Check(hvc_jpp_next(&reader, &message) == HVC_JPP_EOR, "integer boundary reaches EOR");
    }
}

// The client's writer at Bin-ID and integer boundaries; nothing inherited.
static void TestClientWriter() {
    const uint64_t ids[] = {0, 15, 16, 2047, 2048, (UINT64_C(1) << 18) - 1, UINT64_C(1) << 18,
                            (UINT64_C(1) << 25) - 1, UINT64_C(1) << 25, (UINT64_C(1) << 32) - 1,
                            UINT64_C(1) << 32, (UINT64_C(1) << 37) - 1};
    const uint64_t values[] = {0, 127, 128, UINT64_C(1) << 63, UINT64_MAX};
    const int classes[] = {HVC_BIN_PRECINCT, HVC_BIN_TILE_HEADER, HVC_BIN_MAIN_HEADER, HVC_BIN_META_DATA};
    const uint8_t payload[] = {1, 2, 3};
    for (uint64_t id : ids)
        for (uint64_t value : values)
            for (int cls : classes)
                for (int last = 0; last < 2; last++) {
                    hvc_jpp_message boundary = {cls, value, id, value, 0, last, nullptr};
                    hvc_jpp_message filled = {HVC_BIN_PRECINCT, 0, 0, 0, sizeof payload, 1, payload};
                    uint8_t buffer[128];
                    std::memset(buffer, 0xA5, sizeof buffer);
                    size_t first = hvc_jpp_write(&boundary, nullptr, 0);
                    Check(hvc_jpp_write(&boundary, buffer, first - 1) == first && buffer[0] == 0xA5,
                          "client writer leaves a short buffer alone");
                    Check(hvc_jpp_write(&boundary, buffer, first) == first && buffer[first] == 0xA5,
                          "client writer size is stable");
                    size_t size = first + hvc_jpp_write(&filled, buffer + first, sizeof buffer - first);
                    hvc_jpp_reader reader;
                    hvc_jpp_message message;
                    hvc_jpp_begin(&reader, buffer, size);
                    Check(hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE && message.bin_class == cls &&
                          message.codestream == value && message.bin_id == id && message.offset == value &&
                          message.length == 0 && !message.last_byte == !last,
                          "client writer boundary round trips");
                    Check(hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE &&
                          message.bin_class == HVC_BIN_PRECINCT && message.codestream == 0 &&
                          message.bin_id == 0 && message.offset == 0 && message.length == sizeof payload &&
                          message.last_byte && std::memcmp(message.data, payload, sizeof payload) == 0 &&
                          reader.position == size, "client writer payload round trips");
                }
}

int main(void) {
    TestClientWriter();
    TestIntegerBoundaries();
    TestHeadersAndMetadata();
    TestPrecinctFragments();
    TestLimitReason();
    TestPlaceholderBin();
    TestTruncations();
    if (failures) {
        std::printf("%d round-trip check(s) failed\n", failures);
        return 1;
    }
    std::printf("JPP round-trip checks passed\n");
    return 0;
}