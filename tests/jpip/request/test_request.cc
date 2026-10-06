#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "jpip/request/request.h"

using namespace std;

static void Check(bool condition, const char *message) {
    if (!condition) {
        cerr << message << endl;
        exit(EXIT_FAILURE);
    }
}

static vector<int> SelectCodestreams(const jpip::Request &request,
                                     size_t available) {
    vector<int> selected;
    Check(request.SelectCodestreams(available, &selected),
          "Could not expand JPIP codestream selection");
    return selected;
}

static bool RejectRequest(const string &line) {
    jpip::Request request;
    return !request.ParseTarget(line);
}

static void CheckJHVRequests() {
    jpip::Request channel_request;
    Check(channel_request.ParseTarget(
              "/movie.jpx?cnew=http&type=jpp-stream&tid=0&len=512"),
          "Could not parse JHV channel request");
    Check(channel_request.object == "/movie.jpx", "Wrong channel target");
    Check(channel_request.routing.cnew, "Missing cnew field");
    Check(channel_request.routing.tid, "Missing target ID field");
    Check(channel_request.accepts_http, "JHV HTTP transport was not accepted");
    Check(channel_request.has.len && channel_request.length_response == 512,
          "Wrong channel response limit");

    jpip::Request absolute_request;
    Check(absolute_request.ParseTarget(
              "http://get.jpeg.org/images/kids.jp2?cnew=http") &&
              absolute_request.object == "/images/kids.jp2",
          "Could not extract the path from an absolute request target");

    jpip::Request mismatched_target_request;
    Check(mismatched_target_request.ParseTarget(
              "/jpip?cid=7&model=M0:100&tid=1") &&
              mismatched_target_request.routing.tid &&
              !mismatched_target_request.has.model &&
              mismatched_target_request.model.empty(),
          "Retained a cache model for a different target ID");

    jpip::Request matching_target_request;
    Check(matching_target_request.ParseTarget(
              "/jpip?cid=7&model=M0:100&tid=0") &&
              matching_target_request.has.model,
          "Discarded a cache model for the current target ID");

    jpip::Request handled_request;
    Check(handled_request.ParseTarget("/jpip?cid=7&handled") &&
              handled_request.routing.handled,
          "Missing handled field");

    jpip::Request transport_request;
    Check(transport_request.ParseTarget(
              "/movie.jpx?cnew=http-tcp,http") &&
              transport_request.accepts_http,
          "HTTP was not selected from a transport list");
    Check(transport_request.ParseTarget(
              "/movie.jpx?cnew=http-tcp") &&
              !transport_request.accepts_http,
          "An unsupported transport offer was not preserved");
    Check(RejectRequest("/movie.jpx?cnew=http,,http-tcp"),
          "Accepted a malformed transport list");

    jpip::Request target_request;
    Check(target_request.ParseTarget(
              "/jpip?target=movie.jpx&cnew=http&len=512"),
          "Could not parse target-form channel request");
    Check(target_request.routing.target && target_request.target == "movie.jpx",
          "Missing target field");

    jpip::Request quoted_target_request;
    Check(quoted_target_request.ParseTarget(
              "/jpip?target=a\"b.jp2&cnew=http") &&
              quoted_target_request.target == "a\"b.jp2",
          "Changed the request while preparing it for logging");

    jpip::Request escaped_target;
    Check(escaped_target.ParseTarget("/dir/a%20%23%3F%25%26%2B%22%C3%A9.jp2?cnew=http") &&
              escaped_target.object == "/dir/a #?%&+\"é.jp2",
          "Did not decode an escaped target path");
    Check(escaped_target.ParseTarget("/jpip?target=a%3Fb%26c+%2520.jp2&cnew=http") &&
              escaped_target.target == "a?b&c+%20.jp2",
          "Target decoding changed plus or decoded percent twice");
    Check(escaped_target.ParseTarget("/%2e%2e/secret.jp2?cnew=http") &&
              escaped_target.object == "/../secret.jp2",
          "Escaped parent path bypassed file-manager validation");
    for (const char *uri : {"/a%00.jp2?cnew=http", "/a%2.jp2?cnew=http",
                            "/jpip?target=a%00.jp2", "/jpip?target=a%GG.jp2"})
        Check(RejectRequest(uri), "Accepted malformed or NUL target escaping");

    for (const char *route : {"cid", "cclose"}) {
        jpip::Request malformed_path;
        Check(!malformed_path.ParseTarget(string("/bad%GG?fsiz=1,1&") + route + "=7") &&
                  malformed_path.channel == "7" &&
                  (malformed_path.routing.cid || malformed_path.routing.cclose),
              "Malformed path discarded the channel needed for cleanup");
    }

    jpip::Request metadata_request;
    Check(metadata_request.ParseTarget(
              "/jpip?stream=0&metareq=[*]!!&len=2000000&cid=7"),
          "Could not parse JHV metadata request");
    Check(metadata_request.routing.cid && metadata_request.channel == "7",
          "Missing channel ID");
    Check(metadata_request.target.empty(), "Unexpected target in metadata request");
    Check(metadata_request.has_metareq, "Missing metadata request");
    Check(SelectCodestreams(metadata_request, 1) == vector<int>(1, 0),
          "Wrong metadata codestream");
    Check(metadata_request.length_response == 2000000,
          "Wrong metadata response limit");

    jpip::Request frame_request;
    Check(frame_request.ParseTarget(
              "/jpip?stream=4013&fsiz=4096,4096,closest&rsiz=4096,4096&"
              "roff=0,0&len=2097152&cid=7"),
          "Could not parse JHV frame request");
    Check(frame_request.HasWOI(), "Missing frame window");
    Check(SelectCodestreams(frame_request, 4014) == vector<int>(1, 4013),
          "Wrong frame codestream");
    Check(frame_request.resolution_size == jpip::Size(4096, 4096),
          "Wrong frame resolution size");
    Check(frame_request.woi_size == jpip::Size(4096, 4096),
          "Wrong frame region size");
    Check(frame_request.woi_position == jpip::Point(0, 0),
          "Wrong frame region offset");
    Check(frame_request.round_direction == jpip::Request::CLOSEST,
          "Wrong frame rounding mode");
    Check(frame_request.length_response == 2097152, "Wrong frame response limit");

    jpip::Request default_round_request;
    Check(default_round_request.ParseTarget(
              "/jpip?fsiz=4096,4096&cid=7") &&
              default_round_request.round_direction == jpip::Request::ROUNDDOWN,
          "Frame size did not default to round-down");

    jpip::Request incomplete_window;
    Check(!incomplete_window.ParseTarget("/jpip?rsiz=1,1&cid=7") &&
              incomplete_window.resolution_size == jpip::Size(),
          "Accepted a region size without a frame size");

    Check(RejectRequest("/jpip?fsiz=abc&cid=7"),
          "Accepted a malformed frame size");
    Check(RejectRequest("/jpip?fsiz=1,1,sideways&cid=7"),
          "Accepted an unknown frame-size rounding mode");
    Check(RejectRequest("/jpip?roff=abc&cid=7"),
          "Accepted a malformed region offset");
    Check(RejectRequest("/jpip?rsiz=abc&cid=7"),
          "Accepted a malformed region size");
    Check(RejectRequest("/jpip?stream=abc&cid=7"),
          "Accepted a malformed codestream selector");
    Check(RejectRequest("/jpip?context=abc&cid=7"),
          "Accepted a malformed context selector");
    jpip::Request model_request;
    Check(model_request.ParseTarget("/jpip?stream=0&cid=7&model=M0"),
          "Could not parse terminal cache model");
    Check(model_request.has.model, "Missing terminal cache model");
    Check(model_request.model.size() == 1 &&
              model_request.model[0].bin_class == jpip::DataBinClass::META_DATA &&
              model_request.model[0].id == 0 &&
              model_request.model[0].amount == INT_MAX,
          "Wrong terminal metadata model");

    jpip::Request partial_model_request;
    Check(partial_model_request.ParseTarget(
              "/jpip?stream=0&cid=7&model=M0:446"),
          "Could not parse terminal partial cache model");
    Check(partial_model_request.has.model, "Missing terminal partial cache model");
    Check(partial_model_request.model.size() == 1 &&
              partial_model_request.model[0].amount == 446,
          "Wrong terminal partial metadata model");

    Check(RejectRequest("/jpip?model=M-1"),
          "Accepted negative metadata ID");
    Check(RejectRequest("/jpip?model=P-1"),
          "Accepted negative precinct ID");
    Check(RejectRequest("/jpip?model=M0:-1"),
          "Accepted negative model length");
    Check(RejectRequest("/jpip?model="), "Accepted empty cache model");
    Check(RejectRequest("/jpip?model=M0%"),
          "Accepted truncated cache-model escape");
    Check(RejectRequest("/jpip?model=M0%00M1"),
          "Accepted a cache model containing NUL");
    Check(RejectRequest("/jpip?len=-1&cid=7"),
          "Accepted negative response length");
    Check(RejectRequest("/jpip?len=+1&cid=7"),
          "Accepted a signed response length");
    Check(RejectRequest("/jpip?len=2147483648&cid=7"),
          "Accepted an overflowing response length");
    jpip::Request minimum_offset_request;
    Check(minimum_offset_request.ParseTarget(
              "/jpip?fsiz=1,1&roff=-2147483648,0&cid=7") &&
              minimum_offset_request.woi_position.x == INT_MIN,
          "Could not parse the minimum signed window offset");

    jpip::Request close_request;
    Check(close_request.ParseTarget("/jpip?cclose=7&len=0"),
          "Could not parse JHV close request");
    Check(close_request.routing.cclose && close_request.channel == "7", "Missing close field");
    Check(SelectCodestreams(close_request, 1).empty(),
          "Unexpected codestream in close request");

    jpip::Request new_channel_request;
    Check(new_channel_request.ParseTarget("/movie.jpx?cnew=http&len=512"),
          "Could not parse a new channel request");
    Check(new_channel_request.channel.empty(), "Unexpected channel ID in new channel request");

    jpip::Request duplicate_channel_request;
    Check(duplicate_channel_request.ParseTarget("/jpip?cid=46&cid=47") &&
              duplicate_channel_request.channel == "47",
          "Full parsing did not use the last channel ID");
    Check(RejectRequest("/jpip?cid=47&cclose=*"),
          "Accepted the unsupported all-channel close form");
    Check(RejectRequest("/image.jp2?cnew=http&cclose=47"),
          "Accepted conflicting channel creation and closure fields");
    jpip::Request close_priority_request;
    Check(close_priority_request.ParseTarget("/jpip?cclose=47&cid=48") &&
              close_priority_request.routing.cclose && close_priority_request.channel == "47",
          "Full parsing did not give cclose routing priority");

    jpip::Request selection_request;
    Check(selection_request.ParseTarget(
              "/jpip?stream=0,2-6:2,9-:3&context=jpxl%3C1-2%3E&cid=7"),
          "Could not parse codestream selectors");
    Check(SelectCodestreams(selection_request, 16) ==
                  vector<int>({0, 2, 4, 6, 9, 12, 15, 1}),
          "Wrong codestream selector ranges");
    jpip::Request overlapping_selection;
    Check(overlapping_selection.ParseTarget(
              "/jpip?stream=0-2,1-3&cid=7") &&
              SelectCodestreams(overlapping_selection, 4) ==
                      vector<int>({0, 1, 2, 3}),
          "Did not combine overlapping codestream ranges");
    jpip::Request ordered_selection;
    Check(ordered_selection.ParseTarget(
              "/jpip?stream=5-7,0-6&cid=7") &&
              SelectCodestreams(ordered_selection, 8) ==
                      vector<int>({5, 6, 7, 0, 1, 2, 3, 4}),
          "Did not preserve codestream selection order");
    jpip::Request context_first;
    int model_stream = -1;
    Check(context_first.ParseTarget(
              "/jpip?context=jpxl%3C0%3E&stream=1&stream=0&cid=7&model=P0") &&
              SelectCodestreams(context_first, 2) == vector<int>({0, 1}) &&
              context_first.GetUnqualifiedModelCodestream(2, &model_stream) &&
              model_stream == 1,
          "Context or a later stream changed the unqualified cache-model default");

    jpip::Request sampled_single_request;
    Check(sampled_single_request.ParseTarget(
              "/jpip?stream=0:1&cid=7") &&
              SelectCodestreams(sampled_single_request, 10) == vector<int>(1, 0),
          "Treated a sampling factor as a range endpoint");
    jpip::Request finite_range_request;
    Check(finite_range_request.ParseTarget(
              "/jpip?stream=0-1&cid=7") &&
              SelectCodestreams(finite_range_request, 10) ==
                      vector<int>({0, 1}),
          "Could not parse a finite codestream range");
    Check(RejectRequest("/jpip?stream=3-1&cid=7"),
          "Accepted a descending standard codestream range");
    Check(RejectRequest(
              "/jpip?context=jpxl%3C3-1%3E&cid=7"),
          "Accepted a descending JPX layer range");
    Check(RejectRequest("/jpip?stream=0-10:0&cid=7"),
          "Accepted a zero codestream sampling factor");
    Check(RejectRequest("/jpip?stream=0,,1&cid=7"),
          "Accepted an empty codestream range");
    jpip::Request oversized_selection;
    Check(oversized_selection.ParseTarget(
              "/jpip?stream=0-&cid=7"),
          "Could not parse an open codestream range");
    vector<int> oversized_codestreams;
    Check(!oversized_selection.SelectCodestreams(100002,
                                                  &oversized_codestreams),
          "Accepted an oversized expanded codestream selection");

    jpip::Request encoded_model_request;
    Check(encoded_model_request.ParseTarget(
              "/jpip?model=%5B1-2%5DHm:3,H4:5,P6:7,M8:9&cid=7"),
          "Could not parse encoded cache-model descriptors");
    Check(encoded_model_request.model.size() == 4 &&
              encoded_model_request.model[0].bin_class ==
                      jpip::DataBinClass::MAIN_HEADER &&
              encoded_model_request.model[0].first_codestream == 1 &&
              encoded_model_request.model[0].last_codestream == 2 &&
              encoded_model_request.model[0].amount == 3 &&
              encoded_model_request.model[1].bin_class ==
                      jpip::DataBinClass::TILE_HEADER &&
              encoded_model_request.model[1].id == 4 &&
              encoded_model_request.model[1].amount == 5 &&
              encoded_model_request.model[2].bin_class == jpip::DataBinClass::PRECINCT &&
              encoded_model_request.model[2].id == 6 &&
              encoded_model_request.model[2].amount == 7 &&
              encoded_model_request.model[3].bin_class == jpip::DataBinClass::META_DATA &&
              encoded_model_request.model[3].id == 8 &&
              encoded_model_request.model[3].amount == 9,
          "Wrong encoded cache model");

    jpip::Request open_model_request;
    Check(open_model_request.ParseTarget(
              "/jpip?model=%5B5-%5DHm&cid=7") &&
              open_model_request.model.size() == 1 &&
              open_model_request.model[0].first_codestream == 5 &&
              open_model_request.model[0].last_codestream == INT_MAX,
          "Could not parse an open cache-model codestream range");

    jpip::Request encoded_target_request;
    Check(encoded_target_request.ParseTarget(
              "/jpip?target=movie%20name.jpx&cnew=http") &&
              encoded_target_request.target == "movie name.jpx",
          "Did not decode an escaped target value");
    Check(RejectRequest("/jpip?len=12junk&cid=7"),
          "Accepted a response length with trailing data");
}

static void CheckRouteClassification() {
    const char *not_routes[] = {
        "/status?notcnew=http",
        "/status?cnewer=http",
        "/status?value=cnew",
        "/status?x=cid",
        "/status?close=cclose"
    };
    for (const char *target : not_routes)
        Check(!jpip::HasRoutingParameter(target),
              "A non-routing query field was classified as a JPIP route");
    Check(jpip::HasRoutingParameter("/image.jp2?cnew") &&
              jpip::HasRoutingParameter("/image.jp2?x=1&cid=7") &&
              jpip::HasRoutingParameter("/image.jp2?cclose=7&x=1"),
          "A JPIP routing field was not recognized");

}

static void CheckOpenContextRanges() {
    struct Case {
        const char *context;
        size_t available;
        vector<int> selected;
    };
    const Case cases[] = {
        {"jpxl<0->", 4, {0, 1, 2, 3}},
        {"jpxl%3C2-%3E", 4, {2, 3}},
        {"jpxl<3->", 4, {3}},
        {"jpxl<4->", 4, {}},
        {"jpxl<0->", 0, {}},
        {"jpxl<2-99>", 4, {2, 3}},
        {"jpxl<100001->", 100002, {100001}},
        {"jpxl<100001>", 100002, {100001}},
        {"jpxl<2147483648->", 4, {}},
        {"jpxl<18446744073709551615->", 4, {}}
    };
    for (const Case &test : cases) {
        jpip::Request request;
        Check(request.ParseTarget(string("/jpip?cid=7&context=") + test.context),
              "Could not parse a valid context range");
        Check(SelectCodestreams(request, test.available) == test.selected,
              "Wrong context range expansion");
        int stream = -1;
        Check(request.GetUnqualifiedModelCodestream(4, &stream) && stream == 0,
              "Context range changed the unqualified model default");
    }

    jpip::Request combined;
    int stream = -1;
    Check(combined.ParseTarget(
              "/jpip?context=jpxl<2->&stream=1,3-&context=jpxl<0-1>&model=P0") &&
              SelectCodestreams(combined, 5) == vector<int>({2, 3, 4, 1, 0}) &&
              combined.GetUnqualifiedModelCodestream(5, &stream) && stream == 1,
          "Combined ranges lost order, deduplication or the model default");

    jpip::Request boundary;
    Check(boundary.ParseTarget("/jpip?context=jpxl<0->"),
          "Could not parse an open context selection");
    vector<int> selected;
    Check(boundary.SelectCodestreams(100001, &selected) &&
              selected.size() == 100001 && selected.front() == 0 &&
              selected.back() == 100000,
          "Rejected the maximum context selection");
    Check(!boundary.SelectCodestreams(100002, &selected),
          "Accepted an oversized open context selection");

    const char *invalid[] = {
        "jpxl<>", "jpxl<->", "jpxl<-1>", "jpxl<3-1>", "jpxl<1-->",
        "jpxl<1-a>", "jpxl<1-2-3>", "jpxl<1->junk", "jpxl<1-", "jpxl<+1->",
        "jpxl<1-:2>", "jpxl<1,2>", "jpxl<1->,jpxl<2>", "jpxl<1->[s0i0]",
        "jpxl<18446744073709551616->", "jpxl<0-18446744073709551616>"
    };
    for (const char *context : invalid)
        Check(RejectRequest(string("/jpip?context=") + context),
              "Accepted malformed or unsupported context syntax");
}

static void CheckLayers() {
    jpip::Request omitted;
    Check(omitted.ParseTarget("/jpip?cid=7") && omitted.layers == UINT64_MAX,
          "Omitted layers did not request all layers");
    for (uint64_t count : {uint64_t(0), uint64_t(1), uint64_t(65536),
                           uint64_t(INT_MAX) + 1, uint64_t(UINT64_MAX)}) {
        jpip::Request request;
        Check(request.ParseTarget("/jpip?cid=7&layers=" + to_string(count)) &&
                  request.layers == count && !request.HasWOI(),
              "Wrong layers value or unintended spatial window");
    }
    for (const char *value : {"", "-1", "+1", "1x", "1,2", "1-", "18446744073709551616"})
        Check(RejectRequest(string("/jpip?layers=") + value),
              "Accepted malformed layers");
}

static void CheckFrameSizeState() {
    struct Case {
        const char *value;
        jpip::ResponseRequest::RoundDirection direction;
    };
    const Case valid[] = {
        {"640,480,round-up", jpip::ResponseRequest::ROUNDUP},
        {"640,480,closest", jpip::ResponseRequest::CLOSEST},
        {"640,480", jpip::ResponseRequest::ROUNDDOWN},
        {"640,480,round-down", jpip::ResponseRequest::ROUNDDOWN}
    };
    jpip::Request request;
    for (const Case &test : valid) {
        Check(request.ParseTarget(string("/jpip?fsiz=") + test.value + "&cid=7") &&
                      request.has.fsiz && request.resolution_size == jpip::Size(640, 480) &&
                      request.round_direction == test.direction,
              "A valid frame size did not commit its dimensions and rounding mode");
    }
    const char *invalid[] = {"abc", "2147483648,1", "12,34,sideways",
                             "12,34,round-up,extra"};
    for (const char *value : invalid) {
        jpip::Request fresh;
        string error;
        Check(!fresh.ParseTarget(string("/jpip?fsiz=") + value + "&cid=8", &error),
              "Accepted an invalid frame size");
        Check(!fresh.has.fsiz && fresh.resolution_size == jpip::Size() &&
                      fresh.round_direction == jpip::ResponseRequest::ROUNDDOWN,
              "An invalid frame size changed the initial window state");
        Check(error == "Invalid JPIP fsiz parameter" &&
                      fresh.routing.cid && fresh.channel == "8",
              "Frame-size rejection lost its diagnostic or following route");

        Check(request.ParseTarget("/jpip?fsiz=640,480,round-up&cid=7"),
              "Could not initialize an existing frame size");
        Check(!request.ParseTarget(string("/jpip?fsiz=") + value + "&cid=8", &error),
              "Accepted an invalid replacement frame size");
        Check(request.has.fsiz && request.resolution_size == jpip::Size(640, 480) &&
                      request.round_direction == jpip::ResponseRequest::ROUNDUP,
              "An invalid frame size replaced previously parsed window state");
        Check(error == "Invalid JPIP fsiz parameter" && request.channel == "8",
              "Invalid replacement frame size stopped route parsing");
    }
    jpip::Request repeated;
    Check(!repeated.ParseTarget(
                  "/jpip?fsiz=640,480,closest&fsiz=12,34,sideways&cid=8") &&
                  repeated.has.fsiz && repeated.resolution_size == jpip::Size(640, 480) &&
                  repeated.round_direction == jpip::ResponseRequest::CLOSEST &&
                  repeated.channel == "8",
          "An invalid duplicate frame size overwrote an earlier valid value");
}

static void CheckModelRanges() {
    struct Case { const char *range; int first, last; };
    const Case valid[] = {
        {"0", 0, 0},
        {"0-0", 0, 0},
        {"3-9", 3, 9},
        {"9-9", 9, 9},
        {"0-", 0, INT_MAX},
        {"100000", 100000, 100000},
        {"99999-100000", 99999, 100000},
        {"100000-", 100000, INT_MAX}
    };
    for (const Case &test : valid) {
        jpip::Request request;
        Check(request.ParseTarget(string("/jpip?model=[") + test.range + "]P0&cid=7") &&
                      request.has.model && request.model.size() == 1 &&
                      request.model[0].has_codestream_qualifier &&
                      request.model[0].first_codestream == test.first &&
                      request.model[0].last_codestream == test.last,
              "A valid cache-model range changed its endpoints");
    }
    const char *invalid[] = {
        "-5-9", "9-3", "-1", "-1-", "0--1", "-0", "+1", "",
        "100001", "100001-", "0-100001", "100001-100000",
        "2147483647", "0-2147483647", "18446744073709551615",
        "0-18446744073709551615", "18446744073709551616",
        "1-2-3", "1-junk"
    };
    for (const char *range : invalid) {
        jpip::Request request;
        string error;
        Check(!request.ParseTarget(string("/jpip?model=[") + range + "]P0&cid=7", &error),
              "Accepted an invalid cache-model range");
        Check(!request.has.model && request.model.empty(),
              "An invalid range produced a cache-model update");
        Check(error == "Invalid or unsupported JPIP model parameter" &&
                      request.routing.cid && request.channel == "7",
              "Cache-model range rejection lost its diagnostic or following route");
    }
    Check(RejectRequest("/jpip?model=%5B-5-9%5DP0&cid=7") &&
                  RejectRequest("/jpip?model=%5B9-3%5DP0&cid=7"),
          "Accepted an encoded invalid cache-model range");
    jpip::Request repeated;
    Check(repeated.ParseTarget("/jpip?model=[0]P0&model=[1]Hm&cid=7") &&
                  repeated.model.size() == 1 && repeated.model[0].first_codestream == 1 &&
                  repeated.model[0].bin_class == jpip::DataBinClass::MAIN_HEADER,
          "A repeated cache-model parameter stopped using its last value");
}

static void CheckDiagnostics() {
    struct Case { const char *model; const char *error; };
    const Case cases[] = {
        {"-M0", "Subtractive bin-descriptors are not supported for model updating"},
        {"P0:L1", "Number of layers can not be used for model updating"},
        {"X0", "The bin-descriptor 'X' is not supported for model updating"}
    };
    for (const Case &test : cases) {
        jpip::Request request;
        string error;
        Check(!request.ParseTarget(string("/jpip?cid=7&model=") + test.model, &error),
              "Accepted an unsupported cache-model descriptor");
        Check(error == test.error, "Lost the cache-model diagnostic");
    }
}

int main() {
    CheckJHVRequests();
    CheckRouteClassification();
    CheckOpenContextRanges();
    CheckLayers();
    CheckFrameSizeState();
    CheckModelRanges();
    CheckDiagnostics();
    return EXIT_SUCCESS;
}
