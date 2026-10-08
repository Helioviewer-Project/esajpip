#include "hvc_metadata.h"
#include "hvc_reconstruct.h"
#include <cstring>

#include "client_support.h"
#include "hvc_frame.h"

extern "C" int replay_client_response(const uint8_t *data, size_t size) {
    using namespace client_fuzz;
    if (size > 1024 * 1024) return 0;
    Client client;
    hvc_cache cache;
    hvc_cache_begin(&cache);
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    hvc_jpp_begin(&reader, data, size);
    int status;
    bool accepted = true;
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE)
        if (!hvc_cache_apply(&cache, &message)) { accepted = false; break; }
    accepted = accepted && status == HVC_JPP_EOR;
    int reason = hvc_response(client.value, data, size);
    require((reason >= 0) == accepted, "source ingestion differs from message ingestion");
    if (accepted) require(reason == hvc_jpp_reason(&reader), "source changed EOR reason");

    hv_metadata metadata = {};
    char error[256];
    hv_presentation presentation = {};
    const uint8_t *presentation_data;
    bool presented = hvc_metadata_presentation(&cache, &presentation_data, &presentation, error, sizeof error) == 0;
    bool indexed = presented && hvc_metadata_open(&cache, &presentation, &metadata, error, sizeof error) == 0;
    size_t frames = hvc_frames(client.value);
    require(frames == (presented ? presentation.layers : 0), "source presentation indexing differs");
    for (size_t i = 0; indexed && i < frames && i < 16; i++) {
        const hv_registration &registration = presentation.layer[i].registration;
        const uint8_t *xml, *reference;
        size_t length, reference_length;
        require(hvc_xml(client.value, i, &xml, &length) == 0 &&
                hv_metadata_layer_xml(&metadata, i, &registration, &reference, &reference_length, error, sizeof error) == 0 &&
                length == reference_length && (!length || std::memcmp(xml, reference, length) == 0),
                "source XML differs");
        if (registration.count != 1) continue;
        hv_registration_entry entry;
        require(hv_registration_read(&registration, 0, &entry) == nullptr, "registration");
        size_t codestream = entry.codestream;
        int channels, copied_channels;
        int entries = hvc_palette(client.value, i, &channels, nullptr, 0);
        if (entries > 0) {
            require(entries <= 1024 && (channels == 1 || channels == 3), "palette dimensions");
            uint8_t table[HVC_PALETTE_MAX];
            std::memset(table, 0xee, sizeof table);
            size_t length = static_cast<size_t>(entries) * channels;
            require(hvc_palette(client.value, i, &copied_channels, table, length - 1) == entries &&
                    copied_channels == channels, "short palette query changed dimensions");
            for (size_t n = 0; n < sizeof table; n++) require(table[n] == 0xee, "short palette query wrote bytes");
            require(hvc_palette(client.value, i, &copied_channels, table, length) == entries &&
                    copied_channels == channels, "palette copy changed dimensions");
        }

        // Bound expensive traversal by decoded geometry, not by input size.
        // Invalid headers still reach the geometry parser; large valid geometry
        // is skipped here rather than consuming the campaign in long loops.
        const hvc_bin *header = hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, codestream, 0);
        if (!header || !header->complete) continue;
        const hvc_frame *frame = hvc_frame_get(&cache, codestream, error, sizeof error);
        if (!frame || frame->precinct_end[frame->resolutions - 1] > 4096 ||
            static_cast<uint64_t>(frame->width) * frame->height > 1024 * 1024 ||
            frame->components > 4 || frame->layers > 32) continue;
        hvc_view view;
        require(hvc_status(client.value, i, nullptr, &view) == 0, "source status failed");
        hvc_frame_status reference_status;
        require(hvc_reconstruct_status(&cache, codestream, &reference_status, error, sizeof error) == 0 &&
                std::memcmp(&view.source, &reference_status, sizeof reference_status) == 0,
                "source status differs");
        size_t length_j2k = hvc_codestream(client.value, i, nullptr, 0);
        require(length_j2k == hvc_reconstruct(&cache, codestream, nullptr, 0, error, sizeof error),
                "source reconstruction size differs");
        if (length_j2k && length_j2k <= 2 * 1024 * 1024) {
            Bytes out(length_j2k), expected(length_j2k);
            require(hvc_codestream(client.value, i, out.data(), out.size()) == out.size() &&
                    hvc_reconstruct(&cache, codestream, expected.data(), expected.size(), error, sizeof error) == expected.size() &&
                    out == expected, "source reconstructed bytes differ");
        }
    }
    hv_presentation_free(&presentation);
    hv_metadata_close(&metadata);
    hvc_cache_release(&cache);
    return 0;
}

#ifndef ESAJPIP_FUZZ_REPLAY
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return replay_client_response(data, size);
}
#endif
