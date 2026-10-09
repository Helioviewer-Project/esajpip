#include <algorithm>
#include <climits>

#include "client_support.h"
#include "hvc_openjpeg.h"

extern "C" int replay_client_source(const uint8_t *data, size_t size) {
    using namespace client_fuzz;
    if (size < 4 || size > 1024 * 1024) return 0;
    Context &fixture = context(data[0]);
    Client client;
    require(hvc_response(client.value, fixture.opening.data(), fixture.opening.size()) >= 0,
            "opening source failed");
    size_t frames = hvc_frames(client.value);
    require(frames == fixture.image.GetNumCodestreams(), "fixture frame count differs");
    size_t index = data[1] % frames;
    hvc_options options = {data[2] == 255 ? INT_MAX : data[2], 0, 0, data[3]};
    hvc_view view;
    require(hvc_prepare(client.value, index, &options, &view) == 0 &&
            view.request == HVC_FRAME, "fixture frame request failed");
    int reason = hvc_response(client.value, data + 4, size - 4);
    require(hvc_status(client.value, index, &options, &view) == 0, "cached source status failed");
    if (reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_IMAGE_DONE)
        require(view.ready, "completed source response did not deliver requested quality");
    hvc_view before = view;
    size_t cursor = 0;
    for (;;) {
        char model[128], again[128];
        size_t repeated_cursor = cursor;
        int length = hvc_model(client.value, &cursor, model, sizeof model);
        int repeated_length = hvc_model(client.value, &repeated_cursor, again, sizeof again);
        require(length == repeated_length && cursor == repeated_cursor &&
                (length < 0 || std::equal(model, model + length + 1, again)),
                "cache model is not stable");
        if (length <= 0) break;
    }
    require(hvc_restore_response(client.value, fixture.restoration.data(), fixture.restoration.size()) >= 0,
            "unchanged metadata replay rejected");
    require(hvc_status(client.value, index, &options, &view) == 0 &&
            std::equal(before.source.quality, before.source.quality + 33, view.source.quality) &&
            before.ready == view.ready, "restoration changed cached quality");

    size_t length = hvc_codestream(client.value, index, nullptr, 0);
    if (length && length <= 2 * 1024 * 1024) {
        Bytes bytes(length + 1, 0xA5), again(length);
        require(hvc_codestream(client.value, index, bytes.data(), length) == length &&
                bytes[length] == 0xA5 &&
                hvc_codestream(client.value, index, again.data(), length) == length &&
                std::equal(again.begin(), again.end(), bytes.begin()), "reconstruction is not stable");
        if (static_cast<uint64_t>(view.width) * view.height > 1024 * 1024 ||
            view.source.components > 4) return 0;
        hvc_image image;
        char error[256];
        if (hvc_openjpeg_decode(bytes.data(), length, view.reduce, NULL,
                            &image, error, sizeof error) == 0) {
            require(image.width == view.width && image.height == view.height,
                    "decoded dimensions differ from selected resolution");
            free(image.pixels);
        }
    }
    return 0;
}

#ifndef ESAJPIP_FUZZ_REPLAY
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return replay_client_source(data, size);
}
#endif
