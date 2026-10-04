#include "client_support.h"

#include <cstdio>
#include <cstring>

static void write(const char *directory, const char *name, const client_fuzz::Bytes &bytes) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    std::vector<char> path(directory_length + 1 + name_length + 1);
    memcpy(path.data(), directory, directory_length);
    path[directory_length] = '/';
    memcpy(path.data() + directory_length + 1, name, name_length + 1);

    FILE *file = fopen(path.data(), "wb");
    client_fuzz::require(file != NULL, "cannot open client fuzz seed");
    size_t written = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), file);
    client_fuzz::require(written == bytes.size(), "cannot write client fuzz seed");
    client_fuzz::require(fclose(file) == 0, "cannot close client fuzz seed");
}

int main(int argc, char **argv) {
    using namespace client_fuzz;
    require(argc == 3, "client_seeds needs response and source corpus directories");
    write(argv[1], "window-done", {0, HV_EOR_WINDOW_DONE, 0});
    write(argv[1], "truncated-header", {0xE0});
    for (unsigned selector = 0; selector < 3; selector++) {
        Context &fixture = context(selector);
        for (size_t index = 0; index < fixture.image.GetNumCodestreams(); index++) {
            const jpip::CodingParameters &coding = *fixture.image.GetCodingParameters(static_cast<int>(index));
            for (int reduce : {0, coding.num_levels})
                for (int layers : {1, coding.num_layers}) {
                    jpip::ResponseRequest request;
                    request.AddStream(index, index);
                    request.layers = layers;
                    request.has.fsiz = true;
                    request.resolution_size = jpip::Size(((coding.size.x - 1) >> reduce) + 1,
                                                        ((coding.size.y - 1) >> reduce) + 1);
                    char name[128];
                    int name_length = snprintf(name, sizeof name, "%u-%zu-%d-%d", selector, index, reduce, layers);
                    require(name_length > 0 && (size_t)name_length < sizeof name, "client fuzz seed name too long");
                    jpip::DataBinServer fresh;
                    Bytes raw = response(fresh, fixture.image, fixture.sources, request);
                    write(argv[1], name, raw);
                    jpip::DataBinServer channel;
                    jpip::ResponseRequest opening;
                    opening.AddStream(0, fixture.image.GetNumCodestreams() - 1);
                    opening.layers = 0;
                    response(channel, fixture.image, fixture.sources, opening);
                    Bytes body = response(channel, fixture.image, fixture.sources, request);
                    Bytes input = {static_cast<uint8_t>(selector), static_cast<uint8_t>(index),
                                   static_cast<uint8_t>(reduce), static_cast<uint8_t>(layers)};
                    input.insert(input.end(), body.begin(), body.end());
                    write(argv[2], name, input);
                    input.resize(input.size() - 1);
                    char truncated[sizeof name + 16];
                    int truncated_length = snprintf(truncated, sizeof truncated, "%s-truncated", name);
                    require(truncated_length > 0 && (size_t)truncated_length < sizeof truncated,
                            "client fuzz truncated seed name too long");
                    write(argv[2], truncated, input);
                }
        }
    }
    return 0;
}
