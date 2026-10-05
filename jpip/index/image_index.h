#pragma once

#include <string>
#include <vector>
#include "coding_parameters.h"
#include "jpip/source/source.h"
#include "meta_data.h"
#include "packet_index.h"
#include "jpeg2000/hv_reader.h"

namespace jpip {
// One channel's target. Structural parsing is eager and PLT indexing lazy.
class ImageIndex {
    struct TilePart {
        FileSegment data;
        std::vector<hv_plt> plt;
    };
    struct Codestream {
        std::string path;
        CodingParameters parameters;
        FileSegment header;
        std::vector<TilePart> tile_parts;
        PacketIndex packet_index;
        uint64_t packets = 0;
        hv_plt_reader reader;
        size_t tile = 0, segment = 0;
        bool started = false;
        explicit Codestream(const std::string &path_) : path(path_) {
            hv_plt_init(&reader, HV_PLT_PROFILE);
        }
    };
    std::string path_name;
    Metadata meta_data;
    std::vector<Codestream> codestreams;
    std::string error;
    bool failed = false;
    bool Fail(const char *reason, size_t at, const std::string &source = "");
    bool ReadCodestream(const Source &source, size_t start, size_t end,
                        const std::string &path);
    bool ReadMetadata(const Source &source, bool jpx);
    int NextPacket(const Source &source, Codestream &cs, FileSegment *packet);
    bool BuildIndex(const Source &source, Codestream &cs, int max_index);
public:
    explicit ImageIndex(const std::string &path) : path_name(path) {}
    ImageIndex(const ImageIndex &) = delete;
    ImageIndex &operator=(const ImageIndex &) = delete;
    // Call once per index and discard on failure. Reads may use fresh mappings of the
    // same unchanged sources; no pointer into a source survives Open.
    // Mutation during the index lifetime is a configuration/operational error.
    bool Open(const Source &source, SourceProvider &sources, bool jpx);
    const std::string &GetError() const { return error; }
    size_t GetNumCodestreams() const { return codestreams.size(); }
    int GetIndexedPackets(int stream) const { return codestreams[stream].packet_index.Size(); }
    const Metadata &GetMetadata() const { return meta_data; }
    const std::string &GetPathName() const { return path_name; }
    const std::string &GetPathName(int stream) const { return codestreams[stream].path; }
    const FileSegment &GetMainHeader(int stream) const { return codestreams[stream].header; }
    const CodingParameters *GetCodingParameters(int stream) const { return &codestreams[stream].parameters; }
    // Argument errors are recoverable; a source/format failure is terminal.
    bool GetPacket(const Source *source, int stream, const Packet &packet,
                   FileSegment *segment);
};
}
