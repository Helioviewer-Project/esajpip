// hvc_reconstruct: each codestream is served by jpip::DataBinServer, read by
// hvc_jpp into an hvc_cache and written back, at each of its resolutions. The
// result must be the file's main header (without TLM and PLM, progression
// RPCL), one tile-part, and the file's own packets in resolution, position,
// component, layer order, with an empty packet in place of each one above
// the requested resolution. For the images, it must also decode to the
// pixels the file's own codestream decodes to at that resolution. The
// resolutions delivered, and no more, must be complete in the store
// (hvc_reconstruct_status).
//
// The codestreams: every file of the corpus the server accepts (one
// precinct each); the transcoder's reference images, which have many
// precincts, resolutions, layers and components; and made-up codestreams of
// two resolutions and two components in each progression order, with
// unequal precinct sizes, EPH markers, and the default precincts.
//
// Last, a movie on one channel: its frame count, each frame's XML from
// the metadata bins, and every frame reconstructed from the same store.
#include "jpeg2000/hv_served.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hvc_cache.h"
#include "hvc_frame.h"
#include "hvc_openjpeg.h"
#include "hvc_jpp.h"
#include "hvc_metadata.h"
#include "hvc_reconstruct.h"
#include "jpeg2000/hv_reader.h"
#include "jpip/index/image_index.h"
#include "jpip/response/databin_server.h"

using Bytes = std::vector<uint8_t>;

static void check(bool ok, const std::string &message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static int read_metadata(const hvc_cache &cache, hv_metadata *metadata, char *error, size_t error_size) {
    hv_presentation presentation = {};
    const uint8_t *data;
    *metadata = {};
    if (hvc_metadata_presentation(&cache, &data, &presentation, error, error_size) != 0) return -1;
    int result = hvc_metadata_open(&cache, &presentation, metadata, error, error_size);
    hv_presentation_free(&presentation);
    return result;
}

struct Sources : jpip::SourceProvider {
    struct Entry { std::vector<char> bytes; jpip::Source source; };
    std::map<std::string, Entry> files;
    const jpip::Source *GetSource(const std::string &path) override {
        auto found = files.find(path);
        if (found != files.end()) return &found->second.source;
        std::ifstream input(path, std::ios::binary);
        if (!input) return NULL;
        Entry &entry = files[path];
        entry.bytes.assign(std::istreambuf_iterator<char>(input), {});
        entry.source = jpip::Source(entry.bytes.data(), entry.bytes.size());
        return &entry.source;
    }
};

// The response to one request of a channel, through the client's parser,
// into the store.
static int deliver(jpip::DataBinServer &server, jpip::ImageIndex &image, Sources &sources,
                   const jpip::ResponseRequest &request, hvc_cache *cache, const std::string &name) {
    std::string error;
    check(server.SetRequest(image, request, &error), name + ": " + error);
    Bytes response;
    bool last = false;
    while (!last) {
        char chunk[997];
        int length = sizeof chunk;
        check(server.GenerateChunk(sources, chunk, &length, &last), name + ": " + server.GetError());
        response.insert(response.end(), chunk, chunk + length);
    }
    hvc_jpp_reader reader;
    hvc_jpp_message message;
    hvc_jpp_begin(&reader, response.data(), response.size());
    int status;
    while ((status = hvc_jpp_next(&reader, &message)) == HVC_JPP_MESSAGE)
        check(hvc_cache_apply(cache, &message), name + ": " + hvc_cache_error(cache));
    check(status == HVC_JPP_EOR, name + ": " + hvc_jpp_error(&reader));
    return hvc_jpp_reason(&reader);
}

// The codestream written from the store.
static Bytes reconstruct(const hvc_cache &cache, int stream, const std::string &name) {
    char error[256] = "";
    size_t size = hvc_reconstruct(&cache, stream, NULL, 0, error, sizeof error);
    check(size != 0, name + ": " + error);
    Bytes out(size + 1, 0xAA);
    check(hvc_reconstruct(&cache, stream, out.data(), size - 1, error, sizeof error) == size,
          name + ": size changed with a short buffer");
    check(hvc_reconstruct(&cache, stream, out.data(), size, error, sizeof error) == size &&
          out[size] == 0xAA, name + ": wrote other than its size");
    out.pop_back();
    uint8_t *snapshot = nullptr;
    check(hvc_reconstruct_alloc(&cache, stream, 0, &snapshot, error, sizeof error) == size &&
          std::memcmp(snapshot, out.data(), size) == 0, name + ": single-pass snapshot differs");
    free(snapshot);
    return out;
}

// A codestream of the store.
static hvc_frame_status status(const hvc_cache &cache, int stream, const std::string &name) {
    hvc_frame_status found;
    char error[256] = "";
    check(hvc_reconstruct_status(&cache, stream, &found, error, sizeof error) == 0, name + ": " + error);
    return found;
}

// What it must be, from the file alone: `resolutions` of them delivered.
static Bytes expected(jpip::ImageIndex &image, Sources &sources, int stream, int resolutions,
                      const std::string &name, const std::vector<int> &quality = {}) {
    const jpip::CodingParameters &p = *image.GetCodingParameters(stream);
    const jpip::Source *source = sources.GetSource(image.GetPathName(stream));
    check(source != NULL, name + ": missing source");
    const uint8_t *file = source->Data();
    jpip::FileSegment header = image.GetMainHeader(stream);
    Bytes out(file + header.offset, file + header.offset + 2);
    bool eph = false;
    for (size_t at = header.offset + 2; at < header.offset + header.length;) {
        unsigned marker = file[at] << 8 | file[at + 1];
        size_t size = 2 + (file[at + 2] << 8 | file[at + 3]);
        if (marker != 0xFF55 && marker != 0xFF57) {
            out.insert(out.end(), file + at, file + at + size);
            if (marker == 0xFF52) {
                eph = (file[at + 4] & 4) != 0;
                out[out.size() - size + 5] = 2;
            }
        }
        at += size;
    }
    const uint8_t tile_part[] = {0xFF, 0x90, 0, 10, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF, 0x93};
    out.insert(out.end(), tile_part, tile_part + sizeof tile_part);
    for (int r = 0; r <= p.num_levels; ++r)
        for (int y = 0; y < p.resolutions[r].num_precincts.y; ++y)
            for (int x = 0; x < p.resolutions[r].num_precincts.x; ++x)
                for (int c = 0; c < p.num_components; ++c)
                    for (int l = 0; l < p.num_layers; ++l) {
                        if (r >= resolutions || (!quality.empty() && l >= quality[r])) {
                            out.push_back(0);
                            if (eph) { out.push_back(0xFF); out.push_back(0x92); }
                            continue;
                        }
                        jpip::FileSegment packet;
                        check(image.GetPacket(source, stream, jpip::Packet(l, r, c, jpip::Point(x, y)),
                                              &packet), name + ": " + image.GetError());
                        out.insert(out.end(), file + packet.offset,
                                   file + packet.offset + packet.length);
                    }
    out.push_back(0xFF);
    out.push_back(0xD9);
    return out;
}

static jpip::ResponseRequest whole(int stream, const jpip::CodingParameters &p, int resolutions);

static void verify_quality(jpip::ImageIndex &image, Sources &sources, int stream,
                           const std::string &name, bool decoded) {
    const jpip::CodingParameters &p = *image.GetCodingParameters(stream);
    jpip::DataBinServer server;
    hvc_cache cache;
    hvc_cache_begin(&cache);
    std::vector<int> quality(p.num_levels + 1, 0);
    char error[256] = "";
    for (int resolutions : {1, p.num_levels + 1}) {
        for (int layers = 1; layers <= p.num_layers; layers++) {
            jpip::ResponseRequest request = whole(stream, p, resolutions);
            request.layers = layers;
            int reason = deliver(server, image, sources, request, &cache, name);
            check(reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_IMAGE_DONE,
                  name + ": quality window did not complete");
            int reduce = p.num_levels + 1 - resolutions;
            check(hvc_reconstruct_confirm(&cache, stream, reduce, layers, error, sizeof error) == 0,
                  name + ": " + error);
            for (int r = 0; r < resolutions; r++) quality[r] = std::max(quality[r], layers);
            hvc_frame_status has = status(cache, stream, name);
            check(has.layers == p.num_layers &&
                  std::equal(quality.begin(), quality.end(), has.quality),
                  name + ": quality status differs");
            int complete = 0;
            while (complete <= p.num_levels && quality[complete] == p.num_layers) complete++;
            check(has.complete == complete, name + ": partial quality marked complete");
            Bytes preview = reconstruct(cache, stream, name);
            check(preview ==
                  expected(image, sources, stream, p.num_levels + 1, name, quality),
                  name + ": progressive packets or empty padding differ");
            if (decoded) {
                hvc_image pixels;
                check(hvc_openjpeg_decode(preview.data(), preview.size(), reduce, HVC_IMAGE_SAMPLES,
                                      &pixels, error, sizeof error) == 0,
                      name + ": progressive decode: " + error);
                free(pixels.pixels);
            }
            size_t bytes = hvc_cache_total_bytes(&cache);
            deliver(server, image, sources, request, &cache, name);
            check(hvc_cache_total_bytes(&cache) == bytes, name + ": repeated quality resent bytes");
            check(hvc_reconstruct_confirm(&cache, stream, reduce, 1, error, sizeof error) == 0 &&
                  std::equal(quality.begin(), quality.end(), status(cache, stream, name).quality),
                  name + ": confirmation reduced cached quality");
            for (const auto &invalid : std::vector<std::pair<int, int>>{
                    {-1, layers}, {p.num_levels + 1, layers}, {reduce, 0}, {reduce, p.num_layers + 1}})
                check(hvc_reconstruct_confirm(&cache, stream, invalid.first, invalid.second,
                                            error, sizeof error) == -1 &&
                      std::equal(quality.begin(), quality.end(), status(cache, stream, name).quality),
                      name + ": invalid confirmation changed quality");
        }
    }
    hvc_cache_release(&cache);

    if (p.num_layers > 1) {
        jpip::DataBinServer partial_server;
        hvc_cache_begin(&cache);
        jpip::ResponseRequest request = whole(stream, p, p.num_levels + 1);
        request.layers = 1;
        deliver(partial_server, image, sources, request, &cache, name);
        check(hvc_reconstruct_confirm(&cache, stream, 0, 1, error, sizeof error) == 0, error);
        Bytes preview = reconstruct(cache, stream, name);
        const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, stream, 0);
        const uint8_t unfinished[] = {0xAB};
        hvc_jpp_message message = {HVC_BIN_PRECINCT, static_cast<uint64_t>(stream), 0,
                                 bin->length, sizeof unfinished, 0, unfinished};
        check(hvc_cache_apply(&cache, &message), hvc_cache_error(&cache));
        check(reconstruct(cache, stream, name) == preview,
              name + ": unconfirmed refinement changed the preview");
        check(hvc_reconstruct_confirm(&cache, stream, 0, p.num_layers, error, sizeof error) == -1,
              name + ": full quality confirmed incomplete bins");
        check(reconstruct(cache, stream, name) == preview,
              name + ": rejected confirmation changed the preview");
        hvc_cache_release(&cache);
    }
}

static jpip::ResponseRequest whole(int stream, const jpip::CodingParameters &p, int resolutions) {
    jpip::ResponseRequest request;
    request.AddStream(stream, stream);
    int reduce = p.num_levels + 1 - resolutions;
    request.has.fsiz = true;
    request.resolution_size = jpip::Size(((p.size.x - 1) >> reduce) + 1, ((p.size.y - 1) >> reduce) + 1);
    return request;
}

struct Totals {
    size_t images = 0, codestreams = 0, written = 0, reduced = 0, reordered = 0, precincts = 0;
};

// A codestream decoded without its `reduce` highest resolutions.
struct Pixels {
    hvc_image image;
    Bytes bytes;
};
static Pixels decode(const uint8_t *codestream, size_t size, int reduce, const std::string &name) {
    Pixels pixels;
    char error[256] = "";
    check(hvc_openjpeg_decode(codestream, size, reduce, HVC_IMAGE_SAMPLES, &pixels.image, error, sizeof error) == 0,
          name + ": " + error);
    size_t count = static_cast<size_t>(pixels.image.width) * pixels.image.height * pixels.image.components;
    pixels.bytes.assign(pixels.image.pixels, pixels.image.pixels + count);
    free(pixels.image.pixels);
    pixels.image.pixels = NULL;
    return pixels;
}

// Every codestream of one image, on one channel each: a resolution more
// with each request, as a client zooming in receives it.
static void verify(const std::string &path, bool jpx, bool standard, bool decoded, Sources &sources,
                   const std::string &name, Totals *totals) {
    const jpip::Source *source = sources.GetSource(path);
    check(source != NULL, "missing " + name);
    jpip::ImageIndex image(path);
    check(image.Open(*source, sources, jpx), name + ": " + image.GetError());
    ++totals->images;
    for (size_t stream = 0; stream < image.GetNumCodestreams(); ++stream) {
        const jpip::CodingParameters &p = *image.GetCodingParameters(stream);
        ++totals->codestreams;
        totals->reordered += p.progression != jpip::CodingParameters::RPCL_PROGRESSION;
        for (const auto &resolution : p.resolutions)
            totals->precincts += static_cast<size_t>(resolution.num_precincts.x) *
                                 resolution.num_precincts.y * p.num_components;
        jpip::DataBinServer server;
        hvc_cache cache;
        hvc_cache_begin(&cache);
        check(status(cache, stream, name).resolutions == 0, name + ": an empty store has a codestream");
        jpip::ResponseRequest header_request = whole(stream, p, 1);
        header_request.layers = 0;
        deliver(server, image, sources, header_request, &cache, name);
        hvc_frame_status header_status = status(cache, stream, name);
        check(header_status.width == static_cast<uint32_t>(p.size.x) &&
              header_status.height == static_cast<uint32_t>(p.size.y) &&
              header_status.resolutions == p.num_levels + 1 && header_status.complete == 0,
              name + ": header-only response has wrong geometry or contains precinct data");
        const hvc_frame *prepared = hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, stream, 0)->frame;
        check(prepared != NULL, name + ": frame information was not prepared");
        char confirmation_error[256] = "";
        check(hvc_reconstruct_confirm(&cache, stream, p.num_levels, 1,
                                    confirmation_error, sizeof confirmation_error) == -1 &&
              status(cache, stream, name).quality[0] == 0,
              name + ": confirmation accepted missing precincts");
        for (int resolutions = 1; resolutions <= p.num_levels + 1; ++resolutions) {
            int reason = deliver(server, image, sources, whole(stream, p, resolutions), &cache, name);
            check(reason == HVC_EOR_WINDOW_DONE || reason == HVC_EOR_IMAGE_DONE,
                  name + ": response did not complete");
            hvc_frame_status has = status(cache, stream, name);
            check(has.complete == resolutions && has.resolutions == p.num_levels + 1 &&
                  has.width == static_cast<uint32_t>(p.size.x) &&
                  has.height == static_cast<uint32_t>(p.size.y) && has.components == p.num_components,
                  name + ": codestream " + std::to_string(stream) + " is not complete at " +
                  std::to_string(resolutions) + " resolutions");
            Bytes out = reconstruct(cache, stream, name);
            check(hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, stream, 0)->frame == prepared,
                  name + ": frame information was rebuilt after receiving more data");
            check(out == expected(image, sources, stream, resolutions, name),
                  name + ": codestream " + std::to_string(stream) + " differs with " +
                  std::to_string(resolutions) + " resolutions");
            // The reader accepts it as T.800, unless the file itself is not.
            size_t at = 0;
            const char *error = hv_codestream_check(out.data(), 0, out.size(), HV_READ_VALIDATE, &at);
            check(error == NULL || !standard,
                  name + ": written codestream is invalid: " + (error ? error : ""));
            if (decoded) {
                jpip::FileSegment header = image.GetMainHeader(stream);
                const jpip::Source *file = sources.GetSource(image.GetPathName(stream));
                int reduce = p.num_levels + 1 - resolutions;
                Pixels ours = decode(out.data(), out.size(), reduce, name);
                Pixels theirs = decode(file->Data() + header.offset, file->GetSize() - header.offset,
                                       reduce, name);
                check(ours.image.resolutions == p.num_levels + 1 &&
                      ours.image.full_width == static_cast<uint32_t>(p.size.x) &&
                      ours.image.full_height == static_cast<uint32_t>(p.size.y) &&
                      ours.image.width == static_cast<uint32_t>(((p.size.x - 1) >> reduce) + 1) &&
                      ours.image.height == static_cast<uint32_t>(((p.size.y - 1) >> reduce) + 1) &&
                      ours.image.components == (p.num_components >= 3 ? 3 : 1),
                      name + ": decoded image has other dimensions");
                check(ours.bytes == theirs.bytes && ours.image.width == theirs.image.width &&
                      ours.image.components == theirs.image.components,
                      name + ": decoded pixels differ with " + std::to_string(resolutions) +
                      " resolutions");
            }
            ++totals->written;
            totals->reduced += resolutions <= p.num_levels;
        }
        hvc_cache_release(&cache);
        verify_quality(image, sources, stream, name, decoded);
    }
}

static void report(const char *what, const Totals &t) {
    std::cout << what << ": " << t.images << " images, " << t.codestreams << " codestreams, "
              << t.precincts << " precincts, " << t.written << " written (" << t.reduced
              << " at a reduced resolution)\n";
}

static void put(std::vector<char> &out, uint64_t value, int n) {
    while (n--) out.push_back(static_cast<char>(value >> (8 * n)));
}

// A JP2 file whose codestream has two components, two resolutions and two
// layers, in two tile-parts: of 5x3 samples, with 3x1 precincts in each
// resolution (1x2 and 2x4 samples), or (wide) of 40000x3 samples with the
// default precincts, two across the higher resolution. Each packet is one
// byte, different from every other's.
static std::vector<char> synthetic(int progression, bool eph, bool wide) {
    const unsigned xsiz = wide ? 40000 : 5, ysiz = 3;
    const int ppx[] = {0, 1}, ppy[] = {1, 2};
    size_t packets = 0;
    for (int r = 0; r < 2; ++r) {
        unsigned w = (xsiz + (1 - r)) >> (1 - r), h = (ysiz + (1 - r)) >> (1 - r);
        int px = wide ? 15 : ppx[r], py = wide ? 15 : ppy[r];
        packets += static_cast<size_t>(((w - 1) >> px) + 1) * (((h - 1) >> py) + 1) * 2 * 2;
    }
    std::vector<char> cs;
    put(cs, 0xFF4F, 2); put(cs, 0xFF51, 2); put(cs, 44, 2); put(cs, 0, 2);
    for (unsigned v : {xsiz, ysiz, 0u, 0u, xsiz, ysiz, 0u, 0u}) put(cs, v, 4);
    put(cs, 2, 2);
    for (int c = 0; c < 2; ++c) cs.insert(cs.end(), {7, 1, 1});
    put(cs, 0xFF52, 2); put(cs, wide ? 12 : 14, 2); put(cs, (wide ? 0 : 1) | (eph ? 4 : 0), 1);
    put(cs, progression, 1); put(cs, 2, 2);
    cs.insert(cs.end(), {0, 1, 0, 0, 0, 1});
    if (!wide)
        for (int r = 0; r < 2; ++r) put(cs, ppy[r] << 4 | ppx[r], 1);
    put(cs, 0xFF5C, 2); put(cs, 7, 2); cs.insert(cs.end(), {0, 0, 0, 0, 0});
    for (size_t part = 0, first = 0; part < 2; ++part) {
        size_t count = part == 0 ? packets / 2 : packets - packets / 2;
        put(cs, 0xFF90, 2); put(cs, 10, 2); put(cs, 0, 2); put(cs, 19 + 2 * count, 4);
        put(cs, part, 1); put(cs, 2, 1);
        put(cs, 0xFF58, 2); put(cs, 3 + count, 2); put(cs, 0, 1);
        for (size_t i = 0; i < count; ++i) put(cs, 1, 1);
        put(cs, 0xFF93, 2);
        for (size_t i = 0; i < count; ++i) put(cs, 1 + first++, 1);
    }
    put(cs, 0xFFD9, 2);
    std::vector<char> file;
    put(file, 12, 4); put(file, 0x6A502020, 4); put(file, 0x0D0A870A, 4);
    put(file, 20, 4); put(file, 0x66747970, 4); put(file, 0x6A703220, 4); put(file, 0, 4);
    put(file, 0x6A703220, 4);
    put(file, 8 + cs.size(), 4); put(file, 0x6A703263, 4);
    file.insert(file.end(), cs.begin(), cs.end());
    return file;
}

int main() {
    Totals corpus, images, orders, movie;
    std::ifstream manifest(std::string(VECTORS) + "/manifest.tsv");
    check(manifest.good(), "missing corpus manifest");
    std::string line;
    std::getline(manifest, line);
    while (std::getline(manifest, line)) {
        std::istringstream row(line);
        std::string name, kind, standard, profile;
        std::getline(row, name, '\t'); std::getline(row, kind, '\t');
        std::getline(row, standard, '\t'); std::getline(row, profile, '\t');
        if (profile != "valid")
            continue;
        Sources sources;
        verify(std::string(VECTORS) + "/" + name, kind == "jpx", standard == "valid", false, sources,
               name, &corpus);
    }
    report("corpus", corpus);
    check(corpus.images >= 300 && corpus.reduced > 0, "corpus did not cover the cases");

    for (const char *name : {"2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2",
                             "solo_fsi174_127x129_RLCP_PLT.jp2", "solo_fsi174_509x513_PCRL.jp2",
                             "solo_fsi174_510x514_LRCP.jp2", "solo_fsi174_511x513_LRCP_PLT.jp2",
                             "synthetic_rgb_129x129_CPRL_SOP_EPH.jp2"}) {
        Sources sources;
        verify(std::string(IMAGES) + "/" + name, false, true, true, sources, name, &images);
    }
    report("images", images);
    check(images.precincts > 1000 && images.reduced >= 30, "images did not cover the cases");
    {
        Sources sources;
        verify(MOVIE, true, true, true, sources, "merged.jpx", &movie);
    }
    report("movie", movie);
    check(movie.codestreams == 8, "movie did not cover the cases");

    for (int progression = 0; progression < 5; ++progression)
        for (int shape = 0; shape < 3; ++shape) {
            Sources sources;
            Sources::Entry &entry = sources.files["synthetic"];
            entry.bytes = synthetic(progression, shape == 1, shape == 2);
            entry.source = jpip::Source(entry.bytes.data(), entry.bytes.size());
            verify("synthetic", false, false, false, sources,
                   "progression " + std::to_string(progression) + " shape " + std::to_string(shape),
                   &orders);
        }
    report("progressions", orders);
    check(orders.reordered == 12 && orders.precincts == 5 * (12 + 12 + 6),
          "progressions did not cover the cases");

    // A movie of eight frames of several sizes, a frame at a time on one
    // channel; then a frame seen before, at a lower resolution.
    {
        Sources sources;
        std::string name = "merged.jpx", path = MOVIE;
        const jpip::Source *file = sources.GetSource(path);
        check(file != NULL, "missing " + name);
        jpip::ImageIndex image(path);
        check(image.Open(*file, sources, true), name + ": " + image.GetError());
        // Each frame's XML, from the file: its association boxes in order.
        std::vector<Bytes> xml;
        for (size_t at = 0; at + 8 <= file->GetSize();) {
            const uint8_t *box = file->Data() + at;
            size_t size = static_cast<size_t>(box[0]) << 24 | box[1] << 16 | box[2] << 8 | box[3];
            if (std::memcmp(box + 4, "asoc", 4) == 0) {
                check(std::memcmp(box + 12, "nlst", 4) == 0 && std::memcmp(box + 28, "xml ", 4) == 0,
                      name + ": association box is not a number list and XML");
                xml.emplace_back(box + 32, box + size);
            }
            at += size;
        }
        size_t frames = image.GetNumCodestreams();
        check(frames == 8 && xml.size() == frames, name + ": fixture is not eight frames with XML");
        jpip::DataBinServer server;
        hvc_cache cache;
        hvc_cache_begin(&cache);
        char error[256] = "";
        const uint8_t *found = NULL;
        size_t size = 0;
        hv_metadata metadata = {};
        check(read_metadata(cache, &metadata, error, sizeof error) == -1 &&
              std::string(error) == "metadata data-bin 0 is missing", "empty store: " + std::string(error));
        check(hv_metadata_codestream_xml(&metadata, 0, &found, &size, error, sizeof error) == -1 && found == NULL,
              "empty store gave XML");
        for (size_t frame = 0; frame < frames; ++frame) {
            const jpip::CodingParameters &p = *image.GetCodingParameters(frame);
            deliver(server, image, sources, whole(frame, p, p.num_levels + 1), &cache, name);
            if (frame == 0)
                check(read_metadata(cache, &metadata, error, sizeof error) == 0, error);
            check(metadata.codestream_count == frames,
                  name + ": frame count: " + error);
            check(reconstruct(cache, frame, name) == expected(image, sources, frame, p.num_levels + 1, name),
                  name + ": frame " + std::to_string(frame) + " differs");
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            const jpip::CodingParameters &p = *image.GetCodingParameters(frame);
            check(hv_metadata_codestream_xml(&metadata, frame, &found, &size, error, sizeof error) == 0 &&
                  found != NULL && Bytes(found, found + size) == xml[frame],
                  name + ": XML of frame " + std::to_string(frame) + " differs: " + error);
            // Nothing more arrives for a frame the channel has whole.
            size_t before = hvc_cache_total_bytes(&cache);
            deliver(server, image, sources, whole(frame, p, 1), &cache, name);
            check(hvc_cache_total_bytes(&cache) == before, name + ": a frame was sent twice");
            check(reconstruct(cache, frame, name) == expected(image, sources, frame, p.num_levels + 1, name),
                  name + ": frame " + std::to_string(frame) + " differs when shown again");
        }
        check(hv_metadata_codestream_xml(&metadata, frames, &found, &size, error, sizeof error) == -1 && found == NULL,
              name + ": XML for a frame the movie does not have");
        hv_metadata_close(&metadata);
        hvc_cache_release(&cache);

        // The metadata arriving in limited responses: nothing is read from
        // a bin that is not whole.
        jpip::DataBinServer slow;
        hvc_cache_begin(&cache);
        jpip::ResponseRequest request = whole(0, *image.GetCodingParameters(0), 1);
        request.has.len = true;
        request.length_response = 400;
        size_t partial_file = 0, partial_frame = 0;
        for (int responses = 0; responses < 1000; ++responses) {
            deliver(slow, image, sources, request, &cache, name);
            if (read_metadata(cache, &metadata, error, sizeof error) != 0) {
                std::string why = error;
                check(metadata.codestream_count == 0 && metadata.frames == NULL,
                      "failed metadata index retained entries");
                if (why == "metadata data-bin 0 is incomplete") {
                    ++partial_file;
                } else {
                    check(why.compare(0, 18, "metadata data-bin ") == 0 &&
                          (why.find(" is missing") != std::string::npos ||
                           why.find(" is incomplete") != std::string::npos),
                          "partial association: " + why);
                    ++partial_frame;
                }
            } else {
                check(hv_metadata_codestream_xml(&metadata, frames - 1, &found, &size, error, sizeof error) == 0,
                      error);
                break;
            }
        }
        check(partial_file > 0 && partial_frame > 0 && found != NULL &&
              Bytes(found, found + size) == xml[frames - 1], name + ": XML from limited responses");
        hv_metadata_close(&metadata);
        hvc_cache_release(&cache);
    }

    // A JP2 file: one codestream, described by its top-level xml box.
    {
        Sources sources;
        std::string name = "solo_fsi174_127x129_RLCP_PLT.jp2", path = std::string(IMAGES) + "/" + name;
        const jpip::Source *file = sources.GetSource(path);
        check(file != NULL, "missing " + name);
        jpip::ImageIndex image(path);
        check(image.Open(*file, sources, false), name + ": " + image.GetError());
        Bytes xml;
        for (size_t at = 0; at + 8 <= file->GetSize();) {
            const uint8_t *box = file->Data() + at;
            size_t size = static_cast<size_t>(box[0]) << 24 | box[1] << 16 | box[2] << 8 | box[3];
            if (std::memcmp(box + 4, "xml ", 4) == 0)
                xml.assign(box + 8, box + size);
            at += size;
        }
        jpip::DataBinServer server;
        hvc_cache cache;
        hvc_cache_begin(&cache);
        deliver(server, image, sources, whole(0, *image.GetCodingParameters(0), 1), &cache, name);
        char error[256] = "";
        const uint8_t *found = NULL;
        size_t size = 0;
        hv_metadata metadata = {};
        check(read_metadata(cache, &metadata, error, sizeof error) == 0 && metadata.codestream_count == 1,
              name + ": frame count: " + error);
        check(!xml.empty() && hv_metadata_codestream_xml(&metadata, 0, &found, &size, error, sizeof error) == 0 &&
              found != NULL && Bytes(found, found + size) == xml, name + ": XML differs: " + error);
        hv_metadata_close(&metadata);
        hvc_cache_release(&cache);
    }

    // The decoder itself, on an image whose pixels are known: at (x, y) the
    // components are (x + 3y) mod 256, (5x + y) mod 256 and x xor y.
    {
        Sources sources;
        std::string name = "synthetic_rgb_129x129_CPRL_SOP_EPH.jp2";
        const jpip::Source *file = sources.GetSource(std::string(IMAGES) + "/" + name);
        check(file != NULL, "missing " + name);
        hv_box jp2c;
        size_t at = 0;
        check(hv_served_jp2(file->Data(), file->GetSize(), &jp2c, &at) == NULL, name + ": not a JP2 file");
        const uint8_t *codestream = file->Data() + jp2c.payload;
        size_t size = jp2c.end - jp2c.payload;
        Pixels full = decode(codestream, size, 0, name);
        check(full.image.width == 129 && full.image.height == 129 && full.image.components == 3 &&
              full.image.resolutions == 3, name + ": decoded image has other dimensions");
        for (unsigned y = 0; y < 129; ++y)
            for (unsigned x = 0; x < 129; ++x) {
                const uint8_t *pixel = &full.bytes[(y * 129 + x) * 3];
                check(pixel[0] == (x + 3 * y) % 256 && pixel[1] == (5 * x + y) % 256 &&
                      pixel[2] == (x ^ y), name + ": decoded pixels are not the image's");
            }
        Pixels lowest = decode(codestream, size, 99, name);
        check(lowest.image.width == 33 && lowest.image.height == 33 && lowest.image.full_width == 129,
              name + ": lowest resolution has other dimensions");
        hvc_image none;
        char error[256] = "";
        check(hvc_openjpeg_decode(codestream, size / 2, 0, HVC_IMAGE_SAMPLES, &none, error, sizeof error) == -1 &&
              none.pixels == NULL && error[0] != 0, name + ": decoded a truncated codestream");
        check(hvc_openjpeg_decode(codestream + 2, size - 2, 0, HVC_IMAGE_SAMPLES, &none, error, sizeof error) == -1 &&
              none.pixels == NULL && error[0] != 0, name + ": decoded a codestream without SOC");
    }

    // A channel whose responses are cut by a byte limit: a partial main
    // header is refused; unconfirmed precinct bytes stay cached but are omitted.
    {
        Sources sources;
        std::string name = "jpx-graph-frame2.jp2", path = std::string(VECTORS) + "/" + name;
        jpip::ImageIndex image(path);
        check(image.Open(*sources.GetSource(path), sources, false), image.GetError());
        const jpip::CodingParameters &p = *image.GetCodingParameters(0);
        jpip::DataBinServer server;
        hvc_cache cache;
        hvc_cache_begin(&cache);
        char error[256] = "";
        check(hvc_reconstruct(&cache, 0, NULL, 0, error, sizeof error) == 0 &&
              std::string(error) == "main header data-bin is missing", "empty store: " + std::string(error));
        jpip::ResponseRequest request = whole(0, p, p.num_levels + 1);
        request.has.len = true;
        request.length_response = 200;
        size_t partial_header = 0, partial_precinct = 0, empty_image = 0;
        int reason;
        do {
            reason = deliver(server, image, sources, request, &cache, name);
            if (reason == HVC_EOR_WINDOW_DONE)
                break;
            check(reason == HVC_EOR_BYTE_LIMIT_REACHED, "limited response ended otherwise");
            check(status(cache, 0, name).complete <= p.num_levels, "a cut window is complete");
            if (hvc_reconstruct(&cache, 0, NULL, 0, error, sizeof error) != 0) {
                Bytes bytes = reconstruct(cache, 0, name);
                hvc_image decoded = {};
                check(hvc_openjpeg_decode(bytes.data(), bytes.size(), 0, HVC_IMAGE_SAMPLES,
                      &decoded, error, sizeof error) == 0, error);
                free(decoded.pixels);
                const hvc_bin *bin = hvc_cache_find(&cache, HVC_BIN_PRECINCT, 0, 0);
                if (bin != NULL && bin->length && !bin->complete) ++partial_precinct;
                else ++empty_image;
            } else if (std::string(error) == "main header data-bin is missing" ||
                       std::string(error) == "main header data-bin is incomplete") {
                ++partial_header;
            } else {
                check(false, "partial store: " + std::string(error));
            }
        } while (partial_header + partial_precinct + empty_image < 1000);
        check(reason == HVC_EOR_WINDOW_DONE && partial_header > 0 && partial_precinct > 0,
              "limited responses did not cover the partial bins");
        check(reconstruct(cache, 0, name) == expected(image, sources, 0, p.num_levels + 1, name),
              "codestream differs after limited responses");
        check(status(cache, 0, name).complete == p.num_levels + 1,
              "the window is not complete after limited responses");

        // A tile header with marker segments, which the server never sends.
        const hvc_bin *header = hvc_cache_find(&cache, HVC_BIN_MAIN_HEADER, 0, 0);
        const uint8_t segment[] = {0xFF, 0x64, 0, 2};
        hvc_jpp_message messages[] = {
            {HVC_BIN_MAIN_HEADER, 0, 0, 0, header->length, 1, header->data},
            {HVC_BIN_TILE_HEADER, 0, 0, 0, sizeof segment, 1, segment}};
        hvc_cache other;
        hvc_cache_begin(&other);
        check(hvc_cache_apply(&other, &messages[0]) && hvc_cache_apply(&other, &messages[1]),
              hvc_cache_error(&other));
        check(hvc_reconstruct(&other, 0, NULL, 0, error, sizeof error) == 0 &&
              std::string(error) == "tile header data-bin is not empty",
              "tile header: " + std::string(error));
        hvc_cache_release(&other);

        // A main header that declares more packets than the server serves.
        Bytes huge(header->data, header->data + header->length);
        for (size_t at : {8, 12, 24, 28})          // Xsiz, Ysiz, XTsiz, YTsiz
            for (int i = 0; i < 4; ++i) huge[at + i] = i == 0 ? 0x7F : 0xFF;
        messages[0].data = huge.data();
        hvc_cache_begin(&other);
        check(hvc_cache_apply(&other, &messages[0]), hvc_cache_error(&other));
        check(hvc_reconstruct(&other, 0, NULL, 0, error, sizeof error) == 0 &&
              std::string(error) == "main header: more than 2,147,483,647 packets",
              "huge image: " + std::string(error));
        hvc_frame_status none;
        check(hvc_reconstruct_status(&other, 0, &none, error, sizeof error) == -1 &&
              std::string(error) == "main header: more than 2,147,483,647 packets" && none.complete == 0,
              "huge image status: " + std::string(error));
        check(hvc_cache_find(&other, HVC_BIN_MAIN_HEADER, 0, 0)->frame == NULL,
              "failed preparation was cached");
        hvc_cache_release(&other);
        hvc_cache_release(&cache);
    }
    return 0;
}
