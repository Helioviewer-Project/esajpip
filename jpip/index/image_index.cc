#include "jpeg2000/hv_served.h"
#include "image_index.h"
#include <climits>
#include <cstring>
#include <sstream>

namespace jpip {

bool ImageIndex::Fail(const char *reason, size_t at, const std::string &source) {
    std::ostringstream message;
    message << (source.empty() ? path_name : source) << ": " << reason << " at " << at;
    error = message.str();
    failed = true;
    return false;
}

bool ImageIndex::ReadCodestream(const Source &source, size_t start, size_t end,
                                const std::string &path) {
    hv_codestream cs;
    hv_item item;
    struct Close { hv_codestream *cs; ~Close() { hv_codestream_close(cs); } } close{&cs};
    if (hv_codestream_open(&cs, source.Data(), start, end, HV_READ_JPIP_INDEX) != 0)
        return Fail(cs.error, cs.error_at, path);
    codestreams.emplace_back(path);
    Codestream &stream = codestreams.back();
    stream.header.offset = start;
    int status;
    while ((status = hv_codestream_next(&cs, &item)) == 1) {
        if (item.kind == HV_TILE_PART) {
            if (stream.tile_parts.empty()) stream.header.length = item.start - start;
            stream.tile_parts.emplace_back();
        } else if (item.kind == HV_TILE_SEGMENT && item.plt != NULL) {
            stream.tile_parts.back().plt.push_back(*item.plt);
        } else if (item.kind == HV_TILE_DATA) {
            TilePart &tile = stream.tile_parts.back();
            tile.data = FileSegment(item.start, item.end - item.start);
            size_t at = item.start;
            const char *reason = hv_plt_check_segments(tile.plt.data(), tile.plt.size(), &at);
            if (reason != NULL) return Fail(reason, at, path);
        }
    }
    if (status < 0) return Fail(cs.error, cs.error_at, path);
    const hv_siz *siz = hv_codestream_siz(&cs);
    const Cod *cod = hv_codestream_cod(&cs);
    stream.packets = hv_rule_packets(siz, &cod->sgcod, &cod->spcod);
    CodingParameters &params = stream.parameters;
    params.size = Size(siz->fixed->xsiz, siz->fixed->ysiz);
    params.origin = Point();
    params.num_components = siz->fixed->csiz;
    params.num_levels = cod->spcod.levels;
    params.num_layers = cod->sgcod.layers;
    params.progression = cod->sgcod.progression;
    params.resolutions.reserve(params.num_levels + 1);
    for (int r = 0; r <= params.num_levels; ++r) {
        int px = 15, py = 15;
        if (cod->spcod.precincts.nCount != 0) {
            px = cod->spcod.precincts.arr[r].ppx;
            py = cod->spcod.precincts.arr[r].ppy;
        }
        params.resolutions.emplace_back(1 << px, 1 << py);
    }
    return params.FillPrecinctCounts() || Fail("codestream.packet-count", start, path);
}

bool ImageIndex::ReadMetadata(const Source &source, bool jpx) {
    hv_boxes boxes;
    hv_box box;
    const char *reason = NULL;
    size_t at = 0, previous = 0, stream = 0;
    uint64_t length = 0;
    hv_boxes_file(&boxes, source.Data(), source.GetSize());
    int status;
    while ((status = hv_boxes_next(&boxes, &box, &reason, &at)) == 1) {
        bool codestream = box.type == HV_BOX_JP2C || (jpx && box.type == HV_BOX_FTBL);
        if (!codestream && !(jpx && box.type == HV_BOX_ASOC)) continue;
        int id;
        if (codestream) id = static_cast<int>(stream++);
        else {
            meta_data.bins.emplace_back(box.payload, box.end - box.payload);
            id = static_cast<int>(meta_data.bins.size());
        }
        PlaceHolder placeholder(id, codestream, FileSegment(box.start, box.payload - box.start));
        meta_data.bin0.emplace_back(FileSegment(previous, box.start - previous), placeholder);
        length += box.start - previous + placeholder.length();
        previous = box.end;
    }
    if (status < 0) return Fail(reason, at);
    meta_data.tail = FileSegment(previous, source.GetSize() - previous);
    length += meta_data.tail.length;
    return length <= INT_MAX || Fail("metadata bin exceeds INT_MAX", source.GetSize());
}

bool ImageIndex::Open(const Source &source, SourceProvider &sources, bool jpx) {
    size_t at = 0;
    const char *reason;
    if (!jpx) {
        hv_box box;
        reason = hv_served_jp2(source.Data(), source.GetSize(), &box, &at);
        if (reason != NULL) return Fail(reason, at);
        if (!ReadCodestream(source, box.payload, box.end, path_name)) return false;
    } else {
        hv_served_sources parsed = {};
        struct Close { hv_served_sources *p; ~Close() { hv_served_sources_free(p); } } close{&parsed};
        reason = hv_served_jpx(source.Data(), source.GetSize(), &parsed, &at);
        if (reason != NULL) return Fail(reason, at);
        codestreams.reserve(parsed.count);
        std::string path;
        for (size_t i = 0; i < parsed.count; ++i) {
            if (parsed.jp2c != NULL) {
                if (!ReadCodestream(source, parsed.jp2c[i].payload, parsed.jp2c[i].end, path_name))
                    return false;
                continue;
            }
            const hv_served_source &link = parsed.links[i];
            path.resize(path_name.size() + link.loc_size + 2);
            reason = hv_served_path(&link, path_name.c_str(), &path[0], path.size());
            if (reason != NULL) return Fail(reason, 0);
            path.resize(std::strlen(path.c_str()));
            const Source *linked = sources.GetSource(path);
            if (linked == NULL) return Fail("cannot open linked source", 0, path);
            struct Release {
                SourceProvider &sources;
                const std::string &path;
                ~Release() { sources.ReleaseSource(path); }
            } release{sources, path};
            hv_box box;
            reason = hv_served_jp2(linked->Data(), linked->GetSize(), &box, &at);
            if (reason != NULL) return Fail(reason, at, path);
            if (link.offset != box.payload || link.length != box.end - box.payload)
                return Fail("flst.source-extent", box.start, path);
            if (!ReadCodestream(*linked, box.payload, box.end, path)) return false;
        }
    }
    return ReadMetadata(source, jpx);
}

int ImageIndex::NextPacket(const Source &source, Codestream &cs, FileSegment *packet) {
    while (cs.tile < cs.tile_parts.size()) {
        const TilePart &tile = cs.tile_parts[cs.tile];
        while (cs.segment < tile.plt.size()) {
            const hv_plt &plt = tile.plt[cs.segment];
            if (plt.end > source.GetSize()) { Fail("source extent changed", plt.start, cs.path); return -1; }
            if (!cs.started) {
                const char *reason = hv_plt_begin(&cs.reader, &plt);
                if (reason != NULL) { Fail(reason, plt.start, cs.path); return -1; }
                cs.started = true;
            }
            uint64_t offset, length;
            int status = hv_plt_packet(&cs.reader, source.Data(), tile.data.length,
                                      cs.segment + 1 == tile.plt.size(), cs.packets,
                                      &offset, &length);
            if (status < 0) { Fail(cs.reader.error, cs.reader.pos, cs.path); return -1; }
            if (status == 0) { ++cs.segment; cs.started = false; continue; }
            *packet = FileSegment(tile.data.offset + offset, length);
            return 1;
        }
        const char *reason = hv_plt_end_tile(&cs.reader, tile.data.length);
        if (reason != NULL) { Fail(reason, tile.data.offset, cs.path); return -1; }
        ++cs.tile;
        cs.segment = 0;
    }
    const char *reason = hv_plt_end(&cs.reader, cs.packets);
    if (reason != NULL) { Fail(reason, source.GetSize(), cs.path); return -1; }
    return 0;
}

bool ImageIndex::BuildIndex(const Source &source, Codestream &cs, int max_index) {
    while (cs.packet_index.Size() <= max_index) {
        FileSegment packet;
        if (NextPacket(source, cs, &packet) != 1) return false;
        if (cs.packet_index.Size() + 1 == cs.parameters.GetNumPackets()) {
            FileSegment extra;
            int status = NextPacket(source, cs, &extra);
            if (status != 0) {
                if (status == 1) Fail("plt.packet-count", extra.offset, cs.path);
                return false;
            }
        }
        if (!cs.packet_index.Add(packet)) return Fail("packet index limit", packet.offset, cs.path);
    }
    return true;
}

bool ImageIndex::GetPacket(const Source *source, int stream, const Packet &packet, FileSegment *segment) {
    if (failed) return false;
    error.clear();
    if (source == NULL || stream < 0 || static_cast<size_t>(stream) >= codestreams.size()) {
        error = "invalid packet source";
        return false;
    }
    Codestream &cs = codestreams[stream];
    const CodingParameters &params = cs.parameters;
    if (packet.layer < 0 || packet.layer >= params.num_layers ||
        packet.component < 0 || packet.component >= params.num_components ||
        packet.resolution < 0 || packet.resolution > params.num_levels) {
        error = "invalid packet coordinates";
        return false;
    }
    const Size &bounds = params.resolutions[packet.resolution].num_precincts;
    if (packet.precinct_xy.x < 0 || packet.precinct_xy.y < 0 ||
        packet.precinct_xy.x >= bounds.x || packet.precinct_xy.y >= bounds.y) {
        error = "invalid precinct coordinates";
        return false;
    }
    int index = cs.parameters.GetProgressionIndex(packet);
    if (index < 0 || index >= cs.parameters.GetNumPackets()) {
        error = "invalid progression index";
        return false;
    }
    if (index >= cs.packet_index.Size() && !BuildIndex(*source, cs, index)) return false;
    return cs.packet_index.Get(index, segment);
}
}
