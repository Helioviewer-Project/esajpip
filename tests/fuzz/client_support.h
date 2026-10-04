#pragma once

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#endif
#endif

#include "hv_client.h"
#include "jpip/index/image_index.h"
#include "jpip/response/databin_server.h"

namespace client_fuzz {
using Bytes = std::vector<uint8_t>;

inline void require(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
}

inline void mark_initialized(std::string &value) {
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
    __msan_unpoison(&value, sizeof value);
    __msan_unpoison(value.data(), value.size() + 1);
#endif
#endif
}

inline std::string initialized_string(const char *text) {
    std::string value(text);
    mark_initialized(value);
    return value;
}

struct Sources : jpip::SourceProvider {
    struct Entry {
        std::string path;
        std::vector<char> bytes;
        jpip::Source source;
        Entry *next = nullptr;
    };
    Entry *files = nullptr;

    ~Sources() override {
        while (files != nullptr) {
            Entry *next = files->next;
            delete files;
            files = next;
        }
    }

    const jpip::Source *GetSource(const std::string &path) override {
        for (Entry *entry = files; entry != nullptr; entry = entry->next)
            if (entry->path == path) return &entry->source;
        FILE *input = fopen(path.c_str(), "rb");
        if (input == NULL) return nullptr;
        if (fseek(input, 0, SEEK_END) != 0) {
            fclose(input);
            return nullptr;
        }
        long length = ftell(input);
        if (length < 0 || fseek(input, 0, SEEK_SET) != 0) {
            fclose(input);
            return nullptr;
        }
        Entry *entry = new Entry;
        entry->path = path;
        mark_initialized(entry->path);
        entry->bytes.resize((size_t)length);
        size_t read = entry->bytes.empty() ? 0 : fread(entry->bytes.data(), 1, entry->bytes.size(), input);
        if (read != entry->bytes.size() || fclose(input) != 0) {
            delete entry;
            return nullptr;
        }
        entry->source = jpip::Source(entry->bytes.data(), entry->bytes.size());
        entry->next = files;
        files = entry;
        return &entry->source;
    }
};

inline Bytes response(jpip::DataBinServer &server, jpip::ImageIndex &image, Sources &sources,
                      const jpip::ResponseRequest &request) {
    std::string error;
    require(server.SetRequest(image, request, &error), "client fuzz seed request failed");
    Bytes bytes;
    bool last = false;
    while (!last) {
        char chunk[4096];
        int size = sizeof chunk;
        require(server.GenerateChunk(sources, chunk, &size, &last), "client fuzz seed generation failed");
        const uint8_t *payload = reinterpret_cast<const uint8_t *>(chunk);
        bytes.insert(bytes.end(), payload, payload + size);
    }
    return bytes;
}

struct Context {
    std::string path;
    Sources sources;
    jpip::ImageIndex image;
    Bytes opening, restoration;
    explicit Context(const char *path_, bool jpx) : path(initialized_string(path_)), image(path) {
        const jpip::Source *file = sources.GetSource(path);
        require(file && image.Open(*file, sources, jpx), "client fuzz fixture cannot open");
        jpip::DataBinServer server;
        jpip::ResponseRequest request;
        request.AddStream(0, image.GetNumCodestreams() - 1);
        request.layers = 0;
        opening = response(server, image, sources, request);
        jpip::DataBinServer replacement;
        jpip::ResponseRequest restore;
        restore.AddStream(image.GetNumCodestreams(), image.GetNumCodestreams());
        restore.layers = 0;
        restoration = response(replacement, image, sources, restore);
    }
};

inline Context &context(unsigned selector) {
    // Immutable fixtures are loaded once, not opened on every fuzz iteration.
    switch (selector % 3) {
        case 0: { static Context rgb(CLIENT_RGB, false); return rgb; }
        case 1: { static Context gray(CLIENT_GRAY, false); return gray; }
        default: { static Context movie(CLIENT_MOVIE, true); return movie; }
    }
}

struct Client {
    hv_client *value = hv_client_create();
    Client() { require(value != nullptr, "client allocation failed"); }
    ~Client() { hv_client_destroy(value); }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
};
}
