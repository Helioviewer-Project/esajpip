// The native source API, driven by actual server responses without HTTP.
#include "jpeg2000/hv_served.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "hvc_decode.h"
#include "openjpeg.h"
#include <cstring>
#include <unistd.h>
#include "hvc_jpp.h"
#include "hvc_openjpeg.h"
#include "jpeg2000/hv_reader.h"
#include "jpip/index/image_index.h"
#include "jpip/response/databin_server.h"

using Bytes = std::vector<uint8_t>;

static void check(bool ok, const std::string &message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

struct Sources : jpip::SourceProvider {
    struct Entry { std::vector<char> bytes; jpip::Source source; };
    std::map<std::string, Entry> files;
    const jpip::Source *GetSource(const std::string &path) override {
        auto found = files.find(path);
        if (found != files.end()) return &found->second.source;
        std::ifstream input(path, std::ios::binary);
        if (!input) return nullptr;
        Entry &entry = files[path];
        entry.bytes.assign(std::istreambuf_iterator<char>(input), {});
        entry.source = jpip::Source(entry.bytes.data(), entry.bytes.size());
        return &entry.source;
    }
};

static Bytes response(jpip::DataBinServer &server, jpip::ImageIndex &image, Sources &sources,
                      const jpip::ResponseRequest &request) {
    std::string error;
    check(server.SetRequest(image, request, &error), error);
    Bytes body;
    bool last = false;
    while (!last) {
        char chunk[997];
        int size = sizeof chunk;
        check(server.GenerateChunk(sources, chunk, &size, &last), server.GetError());
        const uint8_t *payload = reinterpret_cast<const uint8_t *>(chunk);
        body.insert(body.end(), payload, payload + size);
    }
    return body;
}

static void submit(hvc *client, const Bytes &body) {
    int reason = hvc_response(client, body.data(), body.size());
    check(reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_IMAGE_DONE, hvc_error(client));
}

static jpip::ResponseRequest request(const hvc_view &view) {
    jpip::ResponseRequest fields;
    fields.AddStream(static_cast<int>(view.codestream), static_cast<int>(view.codestream));
    fields.layers = view.request == HVC_HEADER ? 0 : view.requested_layers;
    if (view.request == HVC_FRAME) {
        fields.has.fsiz = true;
        fields.resolution_size = jpip::Size(view.width, view.height);
    }
    return fields;
}

static Bytes reconstruct(hvc *client, size_t frame) {
    size_t size = hvc_codestream(client, frame, nullptr, 0);
    check(size != 0, hvc_error(client));
    Bytes bytes(size);
    check(hvc_codestream(client, frame, bytes.data(), bytes.size()) == size,
          hvc_error(client));
    return bytes;
}

static void verify(const char *path, bool jpx) {
    Sources sources;
    jpip::ImageIndex image(path);
    const jpip::Source *file = sources.GetSource(path);
    check(file && image.Open(*file, sources, jpx), image.GetError());
    jpip::DataBinServer server;
    hvc *client = hvc_create(NULL, NULL);
    check(client != nullptr, "client allocation");
    check(hvc_frames(client) == 0 && hvc_error(client)[0], "empty metadata accepted");
    hvc_view view;
    hvc_options options = {};
    jpip::ResponseRequest opening;
    opening.AddStream(0, 0);
    opening.layers = 0;
    submit(client, response(server, image, sources, opening));
    size_t frames = hvc_frames(client);
    check(frames == static_cast<size_t>(image.GetNumCodestreams()), "frame count differs");
    const uint8_t *initial_xml;
    size_t initial_xml_size;
    check(hvc_xml(client, 0, &initial_xml, &initial_xml_size) == 0, hvc_error(client));
    check(hvc_status(client, frames, &options, &view) == -1, "invalid frame accepted");
    for (hvc_options invalid : {hvc_options{-1, 0, 0, 0},
                                      hvc_options{0, 1, 0, 0},
                                      hvc_options{1, 1, 1, 0},
                                      hvc_options{0, 0, 0, -1}})
        check(hvc_prepare(client, 0, &invalid, &view) == -1, "invalid options accepted");

    for (size_t frame = 0; frame < frames; frame++) {
        const jpip::CodingParameters &coding = *image.GetCodingParameters(static_cast<int>(frame));
        options = {INT_MAX, 0, 0, 1};
        check(hvc_prepare(client, frame, &options, &view) == 0, hvc_error(client));
        if (frame) {
            check(view.request == HVC_HEADER && view.width == 0, "missing-header plan differs");
            const uint8_t empty[] = {0, HVC_EOR_WINDOW_DONE, 0};
            check(hvc_response(client, empty, sizeof empty) == -1, "absent header accepted");
            submit(client, response(server, image, sources, request(view)));
            check(hvc_prepare(client, frame, &options, &view) == 0, hvc_error(client));
        }
        check(view.request == HVC_FRAME && view.reduce == coding.num_levels &&
              view.width == static_cast<uint32_t>(((coding.size.x - 1) >> view.reduce) + 1),
              "lowest-resolution plan differs");
        hvc_view inspected;
        check(hvc_status(client, frame, &options, &inspected) == 0, hvc_error(client));
        check(hvc_prepare(client, frame, &options, &inspected) == -1, "pending request overwritten");
        hvc_cancel_request(client);
        check(hvc_status(client, frame, &options, &inspected) == 0 && !inspected.ready,
              "cancelling a lost request confirmed quality");
        check(hvc_prepare(client, frame, &options, &view) == 0,
              "cancelled request prevented replanning");
        const uint8_t limited[] = {0, HVC_EOR_BYTE_LIMIT_REACHED, 0};
        check(hvc_response(client, limited, sizeof limited) == HVC_EOR_BYTE_LIMIT_REACHED,
              "limited response reason lost");
        check(hvc_prepare(client, frame, &options, &view) == 0 && !view.ready,
              "limited response confirmed or prevented retry");
        const uint8_t empty[] = {0, HVC_EOR_WINDOW_DONE, 0};
        check(hvc_response(client, empty, sizeof empty) == -1, "missing precincts confirmed");
        check(hvc_status(client, frame, &options, &inspected) == 0 && !inspected.ready,
              "rejected response changed readiness");

        // Metadata restoration must neither mutate the source nor finish its
        // pending request; the same request can then be submitted successfully.
        jpip::DataBinServer replacement;
        jpip::ResponseRequest restore;
        restore.AddStream(static_cast<int>(frames), static_cast<int>(frames));
        restore.layers = 0;
        Bytes replay = response(replacement, image, sources, restore);
        jpip::DataBinServer limited_replacement;
        restore.has.len = true;
        restore.length_response = 128;
        int reason, chunks = 0;
        do {
            Bytes chunk = response(limited_replacement, image, sources, restore);
            reason = hvc_restore_response(client, chunk.data(), chunk.size());
            check(reason == HVC_EOR_BYTE_LIMIT_REACHED || reason == HVC_EOR_WINDOW_DONE ||
                  reason == HVC_EOR_IMAGE_DONE, hvc_error(client));
            check(++chunks < 10000, "restoration made no progress");
        } while (reason == HVC_EOR_BYTE_LIMIT_REACHED);
        check(chunks > 1, "restoration did not exercise continuation");
        check(hvc_prepare(client, frame, &options, &inspected) == -1,
              "restoration completed a pending frame request");
        check(hvc_restore_response(client, replay.data(), replay.size()) >= 0,
              hvc_error(client));
        hvc_jpp_reader reader;
        hvc_jpp_message message;
        hvc_jpp_begin(&reader, replay.data(), replay.size());
        while (hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE)
            if (message.length) {
                replay[static_cast<size_t>(message.data - replay.data())] ^= 1;
                check(hvc_restore_response(client, replay.data(), replay.size()) == -1,
                      "different metadata accepted");
                break;
            }
        submit(client, response(server, image, sources, request(view)));
        check(hvc_prepare(client, frame, &options, &view) == 0 && view.ready &&
              view.layers >= 1 && view.request == HVC_READY, "preview not confirmed");
        Bytes preview = reconstruct(client, frame);
        check(!preview.empty(), "preview reconstruction failed");

        // Odd dimensions, viewport fit, independent axes, and large layer
        // requests use this frame's own header rather than another frame's.
        options = {0, static_cast<double>(coding.size.x), static_cast<double>(coding.size.y), INT_MAX};
        check(hvc_prepare(client, frame, &options, &view) == 0 && view.reduce == 0 &&
              view.width == static_cast<uint32_t>(coding.size.x) &&
              view.height == static_cast<uint32_t>(coding.size.y) &&
              view.requested_layers == coding.num_layers, "full-size fit differs");
        if (!view.ready) submit(client, response(server, image, sources, request(view)));
        check(hvc_status(client, frame, &options, &view) == 0 && view.ready &&
              view.layers == coding.num_layers, "full quality not ready");
        Bytes full = reconstruct(client, frame);
        if (coding.num_levels) {
            int width = ((coding.size.x - 1) >> coding.num_levels) + 1;
            int height = ((coding.size.y - 1) >> coding.num_levels) + 1;
            options = {0, static_cast<double>(width), static_cast<double>(height), 0};
            check(hvc_status(client, frame, &options, &view) == 0 &&
                  view.reduce == coding.num_levels, "exact lowest-resolution fit differs");
            options.fit_width += 1;
            options.fit_height += 1;
            check(hvc_status(client, frame, &options, &view) == 0 &&
                  view.reduce < coding.num_levels, "fit above boundary stayed too coarse");
        }
        options = {0, 1, 100000, 1};
        check(hvc_status(client, frame, &options, &view) == 0 && view.reduce == coding.num_levels &&
              view.ready && view.layers == coding.num_layers, "narrow fit or quality retention differs");

        if (frames == 1) {
            const jpip::Source *original = sources.GetSource(image.GetPathName(static_cast<int>(frame)));
            hv_box box;
            size_t offset = 0;
            check(hv_served_jp2(original->Data(), original->GetSize(), &box, &offset) == nullptr,
                  "original container invalid");
            hvc_image decoded, reference;
            char error[256];
            int decoded_result = hvc_openjpeg_decode(full.data(), full.size(), 0, HVC_IMAGE_SAMPLES,
                                                 &decoded, error, sizeof error);
            check(decoded_result == 0, error);
            int reference_result = hvc_openjpeg_decode(original->Data() + box.payload, box.end - box.payload, 0,
                                                   HVC_IMAGE_SAMPLES, &reference, error, sizeof error);
            check(reference_result == 0, error);
            size_t size = static_cast<size_t>(decoded.width) * decoded.height * decoded.components;
            check(decoded.width == reference.width && decoded.height == reference.height &&
                  decoded.components == reference.components &&
                  std::equal(decoded.pixels, decoded.pixels + size, reference.pixels),
                  "native client decoded pixels differ");
            free(decoded.pixels);
            free(reference.pixels);
        }
    }
    size_t cursor = 0;
    char model[128];
    do { check(hvc_model(client, &cursor, model, sizeof model) >= 0, hvc_error(client)); }
    while (model[0]);
    const uint8_t *xml;
    size_t size;
    check(hvc_xml(client, 0, &xml, &size) == 0, hvc_error(client));
    check(xml == initial_xml && size == initial_xml_size, "borrowed XML changed during refinement");
    int channels;
    check(hvc_palette(client, 0, &channels, nullptr, 0) >= 0, hvc_error(client));
    hvc_destroy(client);
    hvc_destroy(nullptr);
}


// Identical decoder adapter and inspector for both kinds of input.
struct Input {
    hvc_input *source;
    size_t position = 0;
    explicit Input(hvc *client, size_t frame, int reduce = 0) : source(hvc_input_open(client, frame, reduce)) {
        check(source != nullptr, hvc_error(client));
    }
    ~Input() { hvc_input_close(source); }
    static OPJ_SIZE_T read(void *out, OPJ_SIZE_T count, void *context) {
        Input &input = *static_cast<Input *>(context);
        size_t n = hvc_input_read(input.source, input.position, static_cast<uint8_t *>(out), count);
        if (!n || n == SIZE_MAX) return static_cast<OPJ_SIZE_T>(-1);
        input.position += n;
        return n;
    }
    static OPJ_BOOL seek(OPJ_OFF_T offset, void *context) {
        Input &input = *static_cast<Input *>(context);
        if (offset < 0 || static_cast<uint64_t>(offset) > hvc_input_size(input.source)) return OPJ_FALSE;
        input.position = static_cast<size_t>(offset);
        return OPJ_TRUE;
    }
    static OPJ_OFF_T skip(OPJ_OFF_T count, void *context) {
        Input &input = *static_cast<Input *>(context);
        if (count < 0) return -1;
        size_t n = std::min(static_cast<size_t>(count), hvc_input_size(input.source) - input.position);
        input.position += n;
        return static_cast<OPJ_OFF_T>(n);
    }
};

static int inspect(hvc *client, size_t frame, void *context, hvc_info *info,
                   char *error, size_t error_size) {
    ++*static_cast<int *>(context);
    Input input(client, frame);
    opj_stream_t *stream = opj_stream_create(4096, OPJ_TRUE);
    opj_codec_t *codec = opj_create_decompress(OPJ_CODEC_J2K);
    check(stream && codec, "decoder allocation");
    opj_stream_set_user_data(stream, &input, nullptr);
    opj_stream_set_user_data_length(stream, hvc_input_size(input.source));
    opj_stream_set_read_function(stream, Input::read);
    opj_stream_set_seek_function(stream, Input::seek);
    opj_stream_set_skip_function(stream, Input::skip);
    opj_dparameters_t parameters;
    opj_set_default_decoder_parameters(&parameters);
    opj_image_t *image = nullptr;
    check(opj_setup_decoder(codec, &parameters) && opj_read_header(stream, codec, &image), "decoder header");
    opj_codestream_info_v2_t *coding = opj_get_cstr_info(codec);
    check(coding && coding->tw == 1 && coding->th == 1, "single tile fixture");
    info->components = static_cast<int>(image->numcomps);
    info->resolutions = coding->m_default_tile_info.tccp_info[0].numresolutions;
    info->layers = coding->m_default_tile_info.numlayers;
    hv_render render;
    int result = hvc_render_read(client, frame, image->numcomps, &render);
    if (result) std::snprintf(error, error_size, "%s", hvc_error(client));
    // The fixtures have matching zero-origin component grids. Decode every
    // reduction below to independently verify these header-derived dimensions.
    for (int level = 0; level < info->resolutions; level++) {
        uint64_t scale = uint64_t{1} << level;
        info->width[level] = static_cast<uint32_t>((image->x1 + scale - 1) / scale - (image->x0 + scale - 1) / scale);
        info->height[level] = static_cast<uint32_t>((image->y1 + scale - 1) / scale - (image->y0 + scale - 1) / scale);
    }
    opj_destroy_cstr_info(&coding);
    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return result;
}

static void u32(Bytes &bytes, uint32_t value) {
    for (int i = 3; i >= 0; i--) bytes.push_back(static_cast<uint8_t>(value >> (8*i)));
}
static void box(Bytes &bytes, const char *type, const Bytes &payload) {
    u32(bytes, static_cast<uint32_t>(8 + payload.size()));
    bytes.insert(bytes.end(), type, type + 4);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}
static Bytes fixture(int layers, bool palette_mapping = true) {
    Sources sources;
    const jpip::Source *original = sources.GetSource(IMAGE);
    check(original != nullptr, "fixture input");
    hv_boxes boxes;
    hv_box child;
    const char *reason;
    size_t at;
    Bytes headers, codestream;
    hv_boxes_file(&boxes, original->Data(), original->GetSize());
    while (hv_boxes_next(&boxes, &child, &reason, &at) == 1) {
        if (child.type == HV_BOX_JP2H) headers.assign(original->Data()+child.payload, original->Data()+child.end);
        if (child.type == HV_BOX_JP2C) codestream.assign(original->Data()+child.payload, original->Data()+child.end);
    }
    check(!headers.empty() && !codestream.empty(), "fixture source boxes");
    Bytes bytes;
    box(bytes, "jP  ", {13,10,135,10});
    box(bytes, "ftyp", {'j','p','x',' ',0,0,0,0,'j','p','x',' '});
    box(bytes, "jp2h", headers);
    for (int cs = 0; cs < 2; cs++) {
        Bytes stream_header, palette = {1,0,3,7,7,7};
        for (int entry = 0; entry < 256; entry++) {
            palette.push_back(static_cast<uint8_t>(entry + cs));
            palette.push_back(static_cast<uint8_t>(255-entry));
            palette.push_back(static_cast<uint8_t>(entry/2 + cs));
        }
        if (palette_mapping) {
            box(stream_header, "pclr", palette);
            box(stream_header, "cmap", {0,0,1,0, 0,0,1,1, 0,0,1,2});
        }
        box(bytes, "jpch", stream_header);
    }
    for (int layer = 0; layer < layers; layer++) {
        Bytes header;
        box(header, "creg", {0,1,0,1,0,static_cast<uint8_t>(1-layer),1,1,0,0});
        // Reverse channel order; do not confuse channel order with CS order.
        if (palette_mapping)
            box(header, "cdef", {0,3, 0,0,0,0,0,3, 0,1,0,0,0,2, 0,2,0,0,0,1});
        box(bytes, "jplh", header);
    }
    for (int cs = 0; cs < 2; cs++) {
        box(bytes, "jp2c", codestream);
        Bytes association;
        box(association, "nlst", {1,0,0,static_cast<uint8_t>(cs)});
        box(association, "xml ", {static_cast<uint8_t>('A'+cs)});
        box(bytes, "asoc", association);
    }
    return bytes;
}

static Bytes read_input(const hvc_input *input) {
    Bytes bytes(hvc_input_size(input));
    // Deliberately cross headers and packets with small reads, then seek back.
    for (size_t at = 0; at < bytes.size(); at += 37)
        check(hvc_input_read(input, at, bytes.data()+at, std::min(size_t{37}, bytes.size()-at)) ==
              std::min(size_t{37}, bytes.size()-at), "ranged decoder input");
    uint8_t prefix[4];
    check(hvc_input_read(input, 0, prefix, 4) == 4 && std::memcmp(prefix, bytes.data(), 4) == 0, "seek backwards");
    check(hvc_input_read(input, bytes.size(), prefix, 4) == 0 &&
          hvc_input_read(input, bytes.size()+1, prefix, 4) == SIZE_MAX &&
          hvc_input_read(input, 0, nullptr, 1) == SIZE_MAX, "input bounds");
    return bytes;
}

static Bytes check_palette(hvc *client, size_t frame) {
    int channels = 0;
    int entries = hvc_palette(client, frame, &channels, nullptr, 0);
    check(entries >= 0, hvc_error(client));
    Bytes expected(static_cast<size_t>(entries) * channels);
    check(hvc_palette(client, frame, &channels, expected.data(), expected.size()) == entries,
          "palette after size query");
    for (int repeat = 0; repeat < 3; repeat++) {
        Bytes copy(expected.size() + 1, 0xEE);
        int copied_channels = -1;
        if (!expected.empty()) {
            check(hvc_palette(client, frame, &copied_channels, copy.data(), expected.size()-1) == entries &&
                  copied_channels == channels && copy == Bytes(copy.size(), 0xEE),
                  "undersized palette buffer remains untouched");
        }
        check(hvc_palette(client, frame, &copied_channels, copy.data(), expected.size()) == entries &&
              copied_channels == channels && copy.back() == 0xEE &&
              std::equal(expected.begin(), expected.end(), copy.begin()), "repeat palette bytes");
        // Mutating the caller's copy must not alter the retained table.
        std::fill(copy.begin(), copy.end(), 0);
    }
    return expected;
}

static void equivalent(const char *path, bool jpx) {
    Sources sources;
    jpip::ImageIndex image(path);
    const jpip::Source *file = sources.GetSource(path);
    check(file && image.Open(*file, sources, jpx), image.GetError());
    char error[256];
    int local_inspections = 0, remote_inspections = 0;
    hvc *local = hvc_open_local(path, inspect, &local_inspections, error, sizeof error);
    hvc *remote = hvc_create(inspect, &remote_inspections);
    check(local && remote, "cross-source open");
    jpip::DataBinServer server;
    jpip::ResponseRequest opening;
    opening.AddStream(image.GetNumCodestreams(), image.GetNumCodestreams());
    opening.layers = 0;
    submit(remote, response(server, image, sources, opening));
    size_t frames = hvc_frames(local);
    check(frames == hvc_frames(remote) && frames > 0 &&
          hvc_codestreams(local) == hvc_codestreams(remote), "local/remote identity counts");
    for (size_t frame = 0; frame < frames; frame++) {
        Bytes palette = check_palette(local, frame);
        check(check_palette(remote, frame) == palette, "local/remote palette bytes");
        hv_render a, b;
        check(hvc_render_read(local, frame, 3, &a) == 0, hvc_error(local));
        check(hvc_render_read(remote, frame, 3, &b) == 0, hvc_error(remote));
        check(a.codestream == b.codestream && a.channel_count == b.channel_count &&
              a.colour_space == b.colour_space, "presentation identity");
        for (size_t channel = 0; channel < a.channel_count; channel++)
            check(a.channel[channel].component == b.channel[channel].component &&
                  a.channel[channel].palette_column == b.channel[channel].palette_column, "channel instructions");
        check(a.palette.entry_count == b.palette.entry_count &&
              a.palette.column_count == b.palette.column_count, "palette dimensions");
        for (size_t entry = 0; entry < a.palette.entry_count; entry++)
            for (size_t column = 0; column < a.palette.column_count; column++) {
                hv_palette_sample x, y;
                check(!hv_palette_read(&a.palette, entry, column, &x) && !hv_palette_read(&b.palette, entry, column, &y) &&
                      x.value == y.value && x.bits == y.bits && x.is_signed == y.is_signed, "exact palette samples");
            }
        const uint8_t *xml_a, *xml_b;
        size_t size_a, size_b;
        check(hvc_xml(local, frame, &xml_a, &size_a) == 0 && hvc_xml(remote, frame, &xml_b, &size_b) == 0 &&
              size_a == size_b && (!size_a || std::memcmp(xml_a, xml_b, size_a) == 0), "layer XML identity");
        hvc_view before, view;
        check(hvc_prepare(remote, frame, nullptr, &view) == 0 && view.request == HVC_HEADER &&
              view.codestream == a.codestream && remote_inspections == static_cast<int>(frame), "mapped header request");
        submit(remote, response(server, image, sources, request(view)));
        check(hvc_status(local, frame, nullptr, &before) == 0, hvc_error(local));
        check(hvc_status(remote, frame, nullptr, &view) == 0, hvc_error(remote));
        check(before.width == view.width && before.height == view.height &&
              before.source.resolutions == view.source.resolutions, "common decoder geometry");
        const hvc_info *info = hvc_info_read(remote, frame);
        check(info && info->width[0] == view.width && info->height[0] == view.height &&
              hvc_info_read(remote, frame) == info, "immutable decoder information");
        Input snapshot(remote, frame, before.source.resolutions - 1);
        Bytes original_snapshot = read_input(snapshot.source);
        check(hvc_prepare(remote, frame, nullptr, &view) == 0 && view.request == HVC_FRAME, "mapped data request");
        submit(remote, response(server, image, sources, request(view)));
        check(read_input(snapshot.source) == original_snapshot, "open input mutated by refinement");
        check(check_palette(local, frame) == palette && check_palette(remote, frame) == palette,
              "palette survives XML indexing and data refinement");
        Input local_input(local, frame), remote_input(remote, frame);
        Bytes left = read_input(local_input.source), right = read_input(remote_input.source);
        for (int reduce = 0; reduce < before.source.resolutions; reduce++) {
            hvc_options options = {reduce,0,0,0};
            check(hvc_status(local,frame,&options,&before)==0 && hvc_status(remote,frame,&options,&view)==0 &&
                  before.width == view.width && before.height == view.height && view.ready, "selected geometry");
            hvc_image x, y;
            check(hvc_openjpeg_decode(left.data(),left.size(),reduce,HVC_IMAGE_SAMPLES,&x,error,sizeof error)==0,error);
            check(hvc_openjpeg_decode(right.data(),right.size(),reduce,HVC_IMAGE_SAMPLES,&y,error,sizeof error)==0,error);
            size_t count = static_cast<size_t>(x.width)*x.height*x.components;
            check(x.width==view.width && x.height==view.height && x.width==y.width && x.height==y.height &&
                  x.components==y.components && std::memcmp(x.pixels,y.pixels,count)==0, "decoded pixels/geometry");
            Input reduced(remote, frame, reduce);
            Bytes limited = read_input(reduced.source);
            check(limited.size() <= right.size(), "reduced snapshot grew");
            hvc_image z;
            check(hvc_openjpeg_decode(limited.data(),limited.size(),reduce,HVC_IMAGE_SAMPLES,&z,error,sizeof error)==0,error);
            check(z.width==x.width && z.height==x.height && z.components==x.components &&
                  std::memcmp(z.pixels,x.pixels,count)==0, "reduced snapshot pixels differ");
            free(z.pixels); free(x.pixels); free(y.pixels);
        }
        check(local_inspections == static_cast<int>(frame+1) && remote_inspections == local_inspections, "cached inspection count");
    }
    hvc_destroy(local); hvc_destroy(remote);
}

static int mismatched_inspect(hvc *client, size_t frame, void *context, hvc_info *info,
                               char *error, size_t error_size) {
    int result = inspect(client, frame, context, info, error, error_size);
    if (*static_cast<int *>(context) == 1) info->width[0]++;
    return result;
}

static void inspection_retry() {
    Sources sources;
    jpip::ImageIndex image(IMAGE);
    const jpip::Source *file = sources.GetSource(IMAGE);
    check(file && image.Open(*file, sources, false), image.GetError());
    jpip::DataBinServer server;
    jpip::ResponseRequest opening;
    opening.AddStream(0,0); opening.layers = 0;
    int calls = 0;
    hvc *client = hvc_create(mismatched_inspect, &calls);
    check(client != nullptr, "inspection retry allocation");
    submit(client, response(server,image,sources,opening));
    hvc_view view;
    check(hvc_status(client,0,nullptr,&view) == -1 && calls == 1 &&
          std::strstr(hvc_error(client), "decoder geometry"), "mismatched JPIP geometry rejected");
    check(hvc_status(client,0,nullptr,&view) == 0 && calls == 2, "rejected geometry was not cached");
    check(hvc_status(client,0,nullptr,&view) == 0 && calls == 2, "valid retry cached");
    hvc_destroy(client);
}

static void cross_source() {
    equivalent(IMAGE, false);
    for (int layers : {1,2}) {
        Bytes bytes = fixture(layers);
        char path[] = "/tmp/hvc-equivalent-XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0 && write(fd,bytes.data(),bytes.size()) == static_cast<ssize_t>(bytes.size()) && close(fd)==0, "write JPX fixture");
        equivalent(path, true);
        check(unlink(path)==0, "remove JPX fixture");
    }
}

// First inspection must also work when data arrived before request planning.
static void partial_inspection(const char *path, int budget = 2000) {
    for (bool limited : {false, true}) {
        Sources sources;
        jpip::ImageIndex image(path);
        check(image.Open(*sources.GetSource(path), sources, false), image.GetError());
        jpip::DataBinServer server;
        int calls = 0;
        hvc *client = hvc_create(inspect, &calls);
        jpip::ResponseRequest opening;
        opening.AddStream(0, 0);
        opening.layers = 0;
        submit(client, response(server, image, sources, opening));
        jpip::ResponseRequest frame;
        frame.AddStream(0, 0);
        frame.has.fsiz = true;
        frame.resolution_size = image.GetCodingParameters(0)->size;
        frame.layers = limited ? image.GetCodingParameters(0)->num_layers : 1;
        frame.has.len = limited;
        frame.length_response = budget;
        Bytes body = response(server, image, sources, frame);
        int reason = hvc_response(client, body.data(), body.size());
        check(reason == (limited ? HVC_EOR_BYTE_LIMIT_REACHED : HVC_EOR_WINDOW_DONE),
              "partial inspection response reason");
        hvc_options options = {};
        hvc_view view;
        check(hvc_status(client, 0, &options, &view) == 0 && !view.ready &&
              view.request == HVC_FRAME && calls == 1, "partial first inspection");
        Bytes before = reconstruct(client, 0);
        check(hvc_response(client, body.data(), body.size()) == reason &&
              reconstruct(client, 0) == before, "unprepared response replay");
        options.layers = 1;
        check(hvc_prepare(client, 0, &options, &view) == 0, hvc_error(client));
        check(view.requested_layers == view.source.layers, "unconfirmed tail requires full quality");
        submit(client, response(server, image, sources, request(view)));
        check(hvc_status(client, 0, &options, &view) == 0 && view.ready && calls == 1,
              "partial inspection continuation");
        hvc *reference = hvc_create(nullptr, nullptr);
        jpip::DataBinServer other;
        frame.has.len = false;
        frame.layers = image.GetCodingParameters(0)->num_layers;
        submit(reference, response(other, image, sources, frame));
        check(reconstruct(client, 0) == reconstruct(reference, 0),
              "continued data differs from uninterrupted response");
        hvc_destroy(reference);
        hvc_destroy(client);
    }
}

static Bytes exported(hvc *client, size_t frame) {
    size_t size = hvc_export(client, frame, nullptr, 0);
    check(size != 0, hvc_error(client));
    Bytes block(size + 1, 0xA5);
    check(hvc_export(client, frame, block.data(), size - 1) == size && block[size - 1] == 0xA5,
          "short export buffer overrun");
    check(hvc_export(client, frame, block.data(), size) == size && block[size] == 0xA5,
          "export size differs");
    block.pop_back();
    return block;
}

// Every retained bin, to detect a changed source.
static std::string model(hvc *client) {
    std::string all;
    size_t cursor = 0;
    char text[512];
    int length;
    while ((length = hvc_model(client, &cursor, text, sizeof text)) > 0) all += std::string(text) + ',';
    check(length == 0, hvc_error(client));
    return all;
}

static Bytes block_of(std::initializer_list<hvc_jpp_message> messages) {
    Bytes block;
    for (const hvc_jpp_message &message : messages) {
        size_t at = block.size();
        block.resize(at + hvc_jpp_write(&message, nullptr, 0));
        check(hvc_jpp_write(&message, block.data() + at, block.size() - at) == block.size() - at,
              "written message size differs");
    }
    return block;
}

// Frames exported from one source and imported into another.
static void persisted(const char *path, bool jpx) {
    Sources sources;
    jpip::ImageIndex image(path);
    const jpip::Source *file = sources.GetSource(path);
    check(file && image.Open(*file, sources, jpx), image.GetError());
    jpip::ResponseRequest opening;
    opening.AddStream(0, 0);
    opening.layers = 0;
    jpip::DataBinServer first, second, third, fourth;
    hvc *saved = hvc_create(NULL, NULL), *loaded = hvc_create(NULL, NULL);
    hvc *target = hvc_create(NULL, NULL), *partial = hvc_create(NULL, NULL);
    check(saved && loaded && target && partial, "client allocation");
    submit(saved, response(first, image, sources, opening));
    submit(loaded, response(second, image, sources, opening));
    submit(target, response(third, image, sources, opening));
    submit(partial, response(fourth, image, sources, opening));
    size_t frames = hvc_frames(saved);
    for (size_t frame = 0; frame < frames; frame++) {
        // Alternate whole and reduced frames.
        hvc_options options = {static_cast<int>(frame % 2), 0, 0, 0};
        hvc_view view, restored;
        check(hvc_prepare(saved, frame, &options, &view) == 0, hvc_error(saved));
        if (view.request == HVC_HEADER) {
            check(hvc_export(saved, frame, nullptr, 0) == 0, "frame without a header exported");
            submit(saved, response(first, image, sources, request(view)));
            check(hvc_prepare(saved, frame, &options, &view) == 0, hvc_error(saved));
        }
        check(view.request == HVC_FRAME, "frame request expected");
        submit(saved, response(first, image, sources, request(view)));
        check(hvc_status(saved, frame, &options, &view) == 0 && view.ready, "exported frame not ready");
        Bytes block = exported(saved, frame);
        check(hvc_import(loaded, frame, block.data(), block.size()) == 0, hvc_error(loaded));
        check(hvc_status(loaded, frame, &options, &restored) == 0 && restored.ready &&
              restored.source.complete == view.source.complete &&
              std::equal(view.source.quality, view.source.quality + 33, restored.source.quality),
              "imported readiness differs");
        check(reconstruct(loaded, frame) == reconstruct(saved, frame) && exported(loaded, frame) == block,
              "imported frame differs");
        // The server resends the imported bins.
        options = {};
        check(hvc_prepare(loaded, frame, &options, &restored) == 0, hvc_error(loaded));
        check((restored.request == HVC_READY) == (view.source.complete == view.source.resolutions),
              "request after import differs");
        if (restored.request == HVC_FRAME) {
            submit(loaded, response(second, image, sources, request(restored)));
            check(hvc_status(loaded, frame, &options, &restored) == 0 && restored.ready,
                  "imported frame not refined");
        }
    }

    // The target holds frame 0's headers and one late precinct.
    Bytes whole = exported(saved, 0);
    std::vector<hvc_jpp_message> messages;
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    hvc_jpp_begin(&reader, whole.data(), whole.size());
    while (reader.position < whole.size()) {
        check(hvc_jpp_next(&reader, &message) == HVC_JPP_MESSAGE && message.codestream == 0,
              "exported block unreadable");
        messages.push_back(message);
    }
    check(messages.size() > 4, "exported block layout differs");
    hvc_jpp_message header = messages[0], tile = messages[1], early = messages[2], late = messages.back();
    check(header.bin_class == HVC_BIN_MAIN_HEADER && tile.bin_class == HVC_BIN_TILE_HEADER &&
          early.bin_class == HVC_BIN_PRECINCT && late.bin_class == HVC_BIN_PRECINCT && late.length > 2,
          "exported block layout differs");
    auto refuses = [](hvc *client, std::initializer_list<Bytes> blocks) {
        Bytes before = exported(client, 0);
        std::string bins = model(client);
        for (const Bytes &block : blocks) {
            check(hvc_import(client, 0, block.data(), block.size()) == -1 && hvc_error(client)[0],
                  "invalid block imported");
            check(exported(client, 0) == before && model(client) == bins, "refused block changed the source");
        }
    };
    Bytes held = block_of({header, late});
    check(hvc_import(target, 0, held.data(), held.size()) == 0, hvc_error(target));
    // Both headers came with the opening response.
    check(exported(target, 0) == block_of({header, tile, late}), "partly imported frame differs");

    Bytes damaged = whole, truncated(whole.begin(), whole.end() - 1), ended = whole;
    damaged[static_cast<size_t>(late.data - whole.data())] ^= 1;
    ended.insert(ended.end(), {0, HVC_EOR_WINDOW_DONE, 0});
    Bytes grown(late.data, late.data + late.length);
    grown.push_back(0);
    // Most faults follow a precinct the target lacks.
    hvc_jpp_message metadata = {HVC_BIN_META_DATA, 0, late.bin_id + 1, 0, 0, 1, nullptr};
    hvc_jpp_message numbered = header, unfinished = messages[3], offset = messages[3], longer = late;
    numbered.bin_id = 1;
    unfinished.last_byte = 0;
    offset.offset = 1;
    longer.data = grown.data();
    longer.length = grown.size();
    refuses(target, {damaged, truncated, ended, Bytes(), block_of({numbered}),
                     block_of({early, metadata}), block_of({early, unfinished}),
                     block_of({early, offset}), block_of({early, longer}), block_of({early, early}),
                     block_of({late, late}), block_of({late, header})});
    check(hvc_import(target, frames, whole.data(), whole.size()) == -1, "invalid frame imported");
    check(hvc_import(target, 0, whole.data(), whole.size()) == 0, hvc_error(target));
    check(reconstruct(target, 0) == reconstruct(saved, 0), "completed frame differs");

    // An unfinished bin: not exported, completed only by matching bytes.
    hvc_view view;
    check(hvc_status(partial, 0, nullptr, &view) == 0, hvc_error(partial));
    hvc_jpp_message begun = late, shorter = late, different = late;
    begun.codestream = view.codestream;
    begun.length = late.length - 1;
    begun.last_byte = 0;
    Bytes delivery = block_of({begun});
    delivery.insert(delivery.end(), {0, HVC_EOR_BYTE_LIMIT_REACHED, 0});
    check(hvc_response(partial, delivery.data(), delivery.size()) == HVC_EOR_BYTE_LIMIT_REACHED,
          hvc_error(partial));
    check(exported(partial, 0) == block_of({header, tile}), "unfinished bin exported");
    shorter.length = late.length - 2;
    grown.pop_back();
    grown[0] ^= 1;
    different.data = grown.data();
    refuses(partial, {block_of({early, shorter}), block_of({early, different})});
    check(hvc_import(partial, 0, whole.data(), whole.size()) == 0, hvc_error(partial));
    check(reconstruct(partial, 0) == reconstruct(saved, 0), "completed bin differs");

    char error[256];
    hvc *local = hvc_open_local(path, NULL, NULL, error, sizeof error);
    check(local != nullptr, error);
    check(hvc_export(local, 0, nullptr, 0) == 0 && std::strstr(hvc_error(local), "remote source"),
          "local source exported data-bins");
    check(hvc_import(local, 0, whole.data(), whole.size()) == -1 &&
          std::strstr(hvc_error(local), "remote source"), "local source imported data-bins");
    hvc_destroy(local);
    hvc_destroy(saved);
    hvc_destroy(loaded);
    hvc_destroy(target);
    hvc_destroy(partial);
}

static void write_responses(const std::string &folder) {
    Sources sources;
    jpip::ImageIndex image(IMAGE);
    check(image.Open(*sources.GetSource(IMAGE), sources, false), image.GetError());
    auto save = [&](const std::string &name, const Bytes &bytes) {
        std::ofstream file(folder + "/" + name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        check(static_cast<bool>(file), "write response fixture");
    };
    for (const char *name : {"headers", "limited", "layers", "whole", "reduced"}) {
        jpip::DataBinServer server;
        jpip::ResponseRequest fields;
        fields.AddStream(0, 0);
        fields.layers = 0;
        save(std::string(name) + "-header.jpp", response(server, image, sources, fields));
        fields.layers = std::string(name) == "layers" ? 1 : image.GetCodingParameters(0)->num_layers;
        fields.has.fsiz = true;
        fields.resolution_size = std::string(name) == "reduced" ? jpip::Size(1, 1) : image.GetCodingParameters(0)->size;
        fields.has.len = std::string(name) == "limited";
        fields.length_response = 3000;
        if (std::string(name) == "headers") continue;
        save(std::string(name) + ".jpp", response(server, image, sources, fields));
        if (fields.has.len) {
            fields.has.len = false;
            save("continuation.jpp", response(server, image, sources, fields));
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-responses") {
        write_responses(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--write-fixtures") {
        for (int layers : {1,2}) {
            Bytes bytes = fixture(layers, false);
            std::string path = std::string(argv[2]) + (layers == 1 ? "/one-layer.jpx" : "/swapped.jpx");
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            check(static_cast<bool>(output), "write WASM layer fixture");
        }
        return 0;
    }
    verify(IMAGE, false);
    verify(MOVIE, true);
    persisted(IMAGE, false);
    persisted(MOVIE, true);
    cross_source();
    inspection_retry();
    partial_inspection(IMAGE);
    partial_inspection(GRAY);
    partial_inspection(GRAY_LARGE, 60000);
    return 0;
}
