#include <sys/resource.h>

#include "server_fixture.h"

using namespace server_test;

int main() {
    InitializeHarness();
    char directory_template[] = "/tmp/esajpip-server-XXXXXX";
    char *directory_name = mkdtemp(directory_template);
    Check(directory_name != NULL, "Could not create the server test directory");
    string directory = directory_name;
    test_directory = directory;

    static const unsigned char image[] = {
        0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A,
        0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x6A, 0x70, 0x32, 0x20,
        0x00, 0x00, 0x00, 0x00, 0x6A, 0x70, 0x32, 0x20, 0x00, 0x00, 0x00, 0x60,
        0x6A, 0x70, 0x32, 0x63, 0xFF, 0x4F, 0xFF, 0x51, 0x00, 0x29, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01,
        0x01, 0xFF, 0x52, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0xFF, 0x5C, 0x00, 0x04, 0x00, 0x00, 0xFF, 0x90, 0x00,
        0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x01, 0xFF, 0x58, 0x00,
        0x04, 0x00, 0x01, 0xFF, 0x93, 0x00, 0xFF, 0xD9
    };
    WriteFile(directory + "/image.jp2", image, sizeof image);
    vector<unsigned char> sop_image(image, image + sizeof image);
    bool cod_found = false;
    for (size_t i = 0; i + 4 < sop_image.size(); ++i) {
        if (sop_image[i] == 0xFF && sop_image[i + 1] == 0x52) {
            sop_image[i + 4] |= 2;
            cod_found = true;
            break;
        }
    }
    Check(cod_found, "Could not prepare SOP source fixture");
    WriteFile(directory + "/sop.jp2", sop_image.data(), sop_image.size());
    // A link to itself can not be opened, whatever the user's privileges.
    Check(symlink("loop.jp2", (directory + "/loop.jp2").c_str()) == 0,
          "Could not create the unreadable source fixture");

    uint16_t port = ReservePort();
    string config_text =
            "[listen]\nport = " + to_string(port) + "\naddress = 127.0.0.1\n"
            "[jpip]\nimage_directory = " + directory + "\nchunk_size = 128\n"
            "[connections]\ninitial_timeout = 1\ntimeout = 1\nlimit = 2\n"
            "[channels]\nlimit = 4\n"
            "[logging]\ndirectory =\nfile_enabled = false\nrequests = false\n";
    WriteFile(directory + "/server.ini", config_text.data(), config_text.size());
    server::Config config;
    string error;
    Check(config.Load((directory + "/server.ini").c_str(), error),
          "Could not load the server test configuration");

    pid_t primary_server = StartServer(config, directory + "/server");
    CheckJPPResponses(port, directory);

    int unavailable = Connect(port);
    Check(unavailable >= 0, "Server did not start");
    SendRequest(unavailable,
                "/jpip?cid=ffffffffffffffffffffffffffffffff&tid=0&handled");
    Response unavailable_response = ReadResponse(unavailable);
    Check(unavailable_response.headers.find("503 Service Unavailable") != string::npos &&
                  unavailable_response.headers.find("JPIP-tid: 0") != string::npos &&
                  unavailable_response.headers.find(HANDLED_HEADER) != string::npos &&
                  unavailable_response.headers.find(
                      "Access-Control-Expose-Headers: JPIP-tid,JPIP-handled") !=
                      string::npos,
          "Unknown channel did not return 503 with JPIP response headers");
    Check(unavailable_response.body == "JPIP channel does not exist",
          "Unknown channel response did not explain the failure");
    CheckClosed(unavailable, 1000,
                "Unknown-channel response retained its connection");
    close(unavailable);

    pid_t conflicting_server = StartServer(config, directory + "/server-bind");
    int bind_status = WaitForServer(conflicting_server);
    Check(WIFEXITED(bind_status) && WEXITSTATUS(bind_status) != 0,
          "A second server bound the same endpoint");

    int bad_request = Connect(port);
    Check(bad_request >= 0, "Could not connect for the bad-request test");
    SendRequest(bad_request, "/image.jp2?cnew=http&fsiz=abc");
    Response bad_request_response = ReadResponse(bad_request);
    Check(bad_request_response.headers.find("400 Bad Request") != string::npos &&
                  bad_request_response.body == "Invalid JPIP fsiz parameter",
          "Bad JPIP request did not explain the invalid field");
    CheckClosed(bad_request, 1000,
                "Bad JPIP request retained its connection");
    close(bad_request);

    int request_body = Connect(port);
    Check(request_body >= 0, "Could not connect for request-body test");
    SendRequest(request_body, "/image.jp2?cnew=http", "Content-Length: 1\r\n");
    WriteAll(request_body, "x", 1);
    Response request_body_response = ReadResponse(request_body);
    Check(request_body_response.headers.find("400 Bad Request") != string::npos &&
                  request_body_response.body ==
                      "HTTP request bodies are not supported",
          "Body-bearing request did not return 400");
    CheckClosed(request_body, 1000,
                "Body-bearing request retained its connection");
    close(request_body);

    int missing_image = Connect(port);
    Check(missing_image >= 0, "Could not connect for the missing-image test");
    SendRequest(missing_image, "/missing.jp2?cnew=http");
    Response missing_image_response = ReadResponse(missing_image);
    Check(missing_image_response.headers.find("404 Not Found") != string::npos &&
                  missing_image_response.body ==
                      "The requested image was not found",
          "Missing image did not return 404 with an explanation");
    CheckClosed(missing_image, 1000,
                "Missing-image response retained its connection");
    close(missing_image);

    int unreadable = Connect(port);
    Check(unreadable >= 0, "Could not connect for the unreadable-image test");
    SendRequest(unreadable, "/loop.jp2?cnew=http");
    Response unreadable_response = ReadResponse(unreadable);
    Check(unreadable_response.headers.find("500 Internal Server Error") !=
                      string::npos &&
                  unreadable_response.body ==
                      "The requested image could not be read",
          "Unreadable image did not return 500 with an explanation");
    CheckClosed(unreadable, 1000,
                "Unreadable-image response retained its connection");
    close(unreadable);

    int bad_image = Connect(port);
    Check(bad_image >= 0, "Could not connect for the bad-image test");
    SendRequest(bad_image, "/image.jpeg?cnew=http&handled");
    Response bad_image_response = ReadResponse(bad_image);
    Check(bad_image_response.headers.find("404 Not Found") !=
                      string::npos &&
                  bad_image_response.headers.find(HANDLED_HEADER) !=
                      string::npos &&
                  bad_image_response.body ==
                      "The requested image type is not supported",
          "Image failure did not explain the unsupported type");
    CheckClosed(bad_image, 1000,
                "Invalid-image response retained its connection");
    close(bad_image);

    int unsupported_source = Connect(port);
    Check(unsupported_source >= 0,
          "Could not connect for unsupported-source test");
    SendRequest(unsupported_source, "/sop.jp2?cnew=http");
    Response unsupported_source_response = ReadResponse(unsupported_source);
    Check(unsupported_source_response.headers.find("404 Not Found") !=
                      string::npos &&
                  unsupported_source_response.body ==
                      "The requested image is not a valid supported JPEG 2000 source",
          "A profile-excluded JPEG 2000 source did not return 404");
    CheckClosed(unsupported_source, 1000,
                "Unsupported-source response retained its connection");
    close(unsupported_source);

    int invalid_path = Connect(port);
    Check(invalid_path >= 0, "Could not connect for the invalid-path test");
    SendRequest(invalid_path,
                "/image.jp2?cnew=http&target=../image.jp2");
    Response invalid_path_response = ReadResponse(invalid_path);
    Check(invalid_path_response.headers.find("404 Not Found") != string::npos &&
                  invalid_path_response.body ==
                      "The requested image path is invalid",
          "Invalid target path did not return 404 with an explanation");
    CheckClosed(invalid_path, 1000,
                "Invalid-path response retained its connection");
    close(invalid_path);

    int malformed_head = Connect(port);
    Check(malformed_head >= 0,
          "Could not connect for malformed request-head test");
    const string malformed_request =
            "GET /image.jp2?cnew=http HTTP/1.1\r\n\r\n";
    WriteAll(malformed_head, malformed_request.data(), malformed_request.size());
    Response malformed_response = ReadResponse(malformed_head);
    Check(malformed_response.headers.find("400 Bad Request") != string::npos &&
                  malformed_response.body == "Invalid HTTP request head",
          "Malformed identified request head did not return a useful 400");
    CheckClosed(malformed_head, 1000,
                "Malformed request head retained its connection");
    close(malformed_head);

    int unsupported_transport = Connect(port);
    Check(unsupported_transport >= 0,
          "Could not connect for unsupported transport test");
    SendRequest(unsupported_transport, "/image.jp2?cnew=http-tcp&len=128");
    Response unsupported_transport_response = ReadResponse(unsupported_transport);
    Check(unsupported_transport_response.headers.find("501 Not Implemented") !=
                      string::npos &&
                  unsupported_transport_response.headers.find("JPIP-cnew:") ==
                      string::npos &&
                  unsupported_transport_response.body ==
                      "The requested JPIP channel transport is not supported",
          "An unsupported channel transport did not return 501 without a channel");
    close(unsupported_transport);

    int fragmented = Connect(port);
    Check(fragmented >= 0, "Could not connect for fragmented request test");
    const string fragments[] = {
        "GET /image.jp2?cnew=http&type=jpp-stream&stream=0&",
        "fsiz=1,1&rsiz=1,1&roff=0,0&len=128 HTTP/1.1\r\n",
        "Host: local",
        "host\r\n\r\n"
    };
    for (const string &fragment : fragments)
        WriteAll(fragmented, fragment.data(), fragment.size());
    Response fragmented_response = ReadResponse(fragmented);
    Check(fragmented_response.headers.find("HTTP/1.1 200 OK") == 0 &&
                  !fragmented_response.body.empty(),
          "Fragmented request head was not served");
    string fragmented_channel = ChannelId(fragmented_response.headers);
    SendRequest(fragmented, "/jpip?cclose=" + fragmented_channel);
    Check(ReadResponse(fragmented).headers.find("HTTP/1.1 200 OK") == 0,
          "Fragmented-request channel could not be closed");
    CheckClosed(fragmented, 1000,
                "Fragmented-request channel retained its connection");
    close(fragmented);

    int duplicate = Connect(port);
    Check(duplicate >= 0, "Could not connect for duplicate channel creation");
    SendRequest(duplicate, "/image.jp2?cnew=http-tcp,http&len=128");
    Response selected_transport = ReadResponse(duplicate);
    Check(selected_transport.headers.find("HTTP/1.1 200 OK") == 0 &&
                  selected_transport.headers.find("transport=http") !=
                      string::npos,
          "HTTP was not selected from the offered transports");
    SendRequest(duplicate, "/image.jp2?cnew=http&len=128");
    Response duplicate_response = ReadResponse(duplicate);
    Check(duplicate_response.headers.find("HTTP/1.1 200 OK") == 0 &&
                  ChannelId(duplicate_response.headers) !=
                      ChannelId(selected_transport.headers),
          "A pooled connection could not create a second channel");
    close(duplicate);

    int unrelated = Connect(port);
    Check(unrelated >= 0, "Could not connect for the unrelated-request test");
    SendRequest(unrelated, "/image.jp2?cnew=http&len=128");
    Response unrelated_created = ReadResponse(unrelated);
    Check(unrelated_created.headers.find("HTTP/1.1 200 OK") == 0,
          "Unrelated-request channel creation failed");
    SendRequest(unrelated, "/favicon.ico");
    CheckClosed(unrelated, 1000,
                "A later request without a JPIP route was answered or retained");
    close(unrelated);
    unrelated = Connect(port);
    Check(unrelated >= 0, "Could not reconnect after the unrelated request");
    SendRequest(unrelated,
                "/jpip?cclose=" + ChannelId(unrelated_created.headers));
    Check(ReadResponse(unrelated).headers.find("HTTP/1.1 200 OK") == 0,
          "A request without a JPIP route ended a channel it did not name");
    close(unrelated);

    int channel = Connect(port);
    Check(channel >= 0, "Could not connect for channel creation");
    SendRequest(channel,
                "http://localhost/image.jp2?cnew=http&type=jpp-stream&stream=0&"
                "metareq=[*]!!&fsiz=1,1&rsiz=1,1&roff=0,0&len=128&handled",
                "Accept-Encoding: gzip\r\n");
    Response created = ReadResponse(channel);
    Check(created.headers.find("HTTP/1.1 200 OK") == 0 &&
                  created.headers.find(HANDLED_HEADER) != string::npos,
          "Channel creation did not return 200 with JPIP-handled");
    Check(created.headers.find("Content-Encoding: gzip") != string::npos,
          "Metadata response was not gzip encoded");
    Check(!Gunzip(created.body).empty(), "Gzip JPIP response was empty");
    string channel_id = ChannelId(created.headers);
    Check(channel_id.size() == 32 &&
                  channel_id.find_first_not_of("0123456789abcdef") ==
                      string::npos,
          "The server returned an invalid channel ID");

    int conflicting_close = Connect(port);
    Check(conflicting_close >= 0,
          "Could not connect for conflicting close test");
    SendRequest(conflicting_close,
                "/image.jp2?cnew=http&cclose=" + channel_id);
    Response conflicting_close_response = ReadResponse(conflicting_close);
    Check(conflicting_close_response.headers.find("400 Bad Request") !=
                      string::npos &&
                  conflicting_close_response.body ==
                      "JPIP cnew and cclose can not be combined",
          "Conflicting channel fields did not return 400");
    CheckClosed(conflicting_close, 1000,
                "Conflicting close retained its connection");
    close(conflicting_close);

    string pipelined;
    for (int i = 0; i < 8; ++i) {
        pipelined += "GET /jpip?cid=" + channel_id +
                "&stream=0&fsiz=1,1&rsiz=1,1&roff=0,0&len=128";
        if (i % 2 == 0)
            pipelined += "&context=jpxl%3C0%3E&model=M0&tid=0&handled";
        pipelined += " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    }
    WriteAll(channel, pipelined.data(), pipelined.size());
    string pipelined_input;
    for (int i = 0; i < 8; ++i) {
        Response response = ReadResponse(channel, &pipelined_input);
        bool optional_headers = i % 2 == 0;
        Check(response.headers.find("HTTP/1.1 200 OK") == 0 &&
                      (response.headers.find("JPIP-tid: 0") != string::npos) ==
                              optional_headers &&
                      (response.headers.find("JPIP-handled:") != string::npos) ==
                              optional_headers &&
                      !response.body.empty(),
              "Pipelined responses were not returned in request order");
    }

    string partial = "GET /jpip?cid=" + channel_id;
    WriteAll(channel, partial.data(), partial.size());
    int replacement = Connect(port);
    Check(replacement >= 0, "Could not create a replacement connection");
    SendRequest(replacement,
                "/jpip?cid=" + channel_id +
                "&stream=0&fsiz=1,1&rsiz=1,1&roff=0,0&len=128&tid=0&handled");
    Response replaced = ReadResponse(replacement);
    Check(replaced.headers.find("HTTP/1.1 200 OK") == 0 &&
                  replaced.headers.find("JPIP-tid: 0") != string::npos &&
                  replaced.headers.find(HANDLED_HEADER) != string::npos &&
                  !replaced.body.empty(),
          "Replacement connection did not continue the channel");
    SendRequest(replacement,
                "/jpip?cid=" + channel_id +
                "&stream=0&fsiz=1,1&rsiz=1,1&roff=0,0&len=128",
                "Connection: close\r\n");
    Response close_response = ReadResponse(replacement);
    Check(close_response.headers.find("HTTP/1.1 200 OK") == 0 &&
                  close_response.headers.find("Connection: close") !=
                          string::npos,
          "Connection close request was not served with a close header");
    CheckClosed(replacement, 1000,
                "Connection close request retained its connection");
    close(replacement);

    replacement = Connect(port);
    Check(replacement >= 0,
          "Could not reconnect after Connection close");
    SendRequest(replacement,
                "/jpip?cid=" + channel_id +
                "&stream=0&fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
    Check(ReadResponse(replacement).headers.find("HTTP/1.1 200 OK") == 0,
          "Channel was lost after Connection close");

    SendRequest(replacement,
                "/jpip?cclose=" + channel_id + "&tid=0&handled");
    Response closed = ReadResponse(replacement);
    Check(closed.headers.find("HTTP/1.1 200 OK") == 0 &&
                  closed.headers.find("JPIP-tid: 0") != string::npos &&
                  closed.headers.find(HANDLED_HEADER) != string::npos &&
                  closed.headers.find("Connection: close") != string::npos,
          "Channel close did not return 200 with JPIP capability headers");
    CheckClosed(replacement, 1000, "Closed channel retained its connection");
    close(replacement);
    CheckClosed(channel, 2500, "Abandoned partial connection did not expire");
    close(channel);

    int oversized = Connect(port);
    Check(oversized >= 0, "Could not connect for the request-limit test");
    SendRequest(oversized,
                "/image.jp2?cnew=http&type=jpp-stream&stream=0&"
                "fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
    Response oversized_created = ReadResponse(oversized);
    Check(oversized_created.headers.find("HTTP/1.1 200 OK") == 0,
          "Request-limit channel creation failed");
    string oversized_channel = ChannelId(oversized_created.headers);
    SendRequest(oversized, "/jpip?cid=" + oversized_channel,
                "X-Large: " + string(4096, 'x') + "\r\n");
    Response oversized_response = ReadResponse(oversized);
    Check(oversized_response.headers.find(
                  "431 Request Header Fields Too Large") != string::npos &&
                  oversized_response.body == "HTTP request head is too large",
          "Oversized request head did not return 431");
    CheckClosed(oversized, 1000, "Oversized request retained its channel");
    close(oversized);
    int oversized_retry = Connect(port);
    Check(oversized_retry >= 0,
          "Could not reconnect after the oversized request");
    SendRequest(oversized_retry, "/jpip?cid=" + oversized_channel + "&len=128");
    Check(ReadResponse(oversized_retry).headers.find(
                  "503 Service Unavailable") != string::npos,
          "Oversized request did not end its channel");
    close(oversized_retry);

    for (const char *route : {"cid", "cclose"}) {
        int malformed = Connect(port);
        SendRequest(malformed, "/image.jp2?cnew=http&stream=0&fsiz=1,1&len=128");
        string malformed_channel = ChannelId(ReadResponse(malformed).headers);
        SendRequest(malformed, string("/bad%GG?") + route + "=" + malformed_channel);
        Check(ReadResponse(malformed).headers.find("400 Bad Request") != string::npos,
              "Malformed escape did not return 400");
        close(malformed);
        int retry = Connect(port);
        SendRequest(retry, "/jpip?cid=" + malformed_channel + "&len=128");
        Check(ReadResponse(retry).headers.find("503 Service Unavailable") != string::npos,
              "Malformed escape did not end its referenced channel");
        close(retry);
    }

    int rejected = Connect(port);
    Check(rejected >= 0, "Could not connect for rejected-request test");
    SendRequest(rejected,
                "/image.jp2?cnew=http&type=jpp-stream&stream=0&"
                "fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
    Response rejected_created = ReadResponse(rejected);
    Check(rejected_created.headers.find("HTTP/1.1 200 OK") == 0,
          "Rejected-request channel creation failed");
    string rejected_channel = ChannelId(rejected_created.headers);
    SendRequest(rejected,
                "/jpip?cid=" + rejected_channel +
                "&model=[1-]Hm&fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
    Response rejected_response = ReadResponse(rejected);
    Check(rejected_response.headers.find("400 Bad Request") != string::npos &&
                  rejected_response.body ==
                      "JPIP cache model does not match the selected image",
          "Invalid channel request did not return 400");
    CheckClosed(rejected, 1000,
                "Invalid request retained its connection");
    close(rejected);

    int rejected_retry = Connect(port);
    Check(rejected_retry >= 0,
          "Could not reconnect after the invalid request");
    SendRequest(rejected_retry,
                "/jpip?cid=" + rejected_channel + "&len=128");
    Check(ReadResponse(rejected_retry).headers.find(
                  "503 Service Unavailable") != string::npos,
          "Invalid request did not end its channel");
    close(rejected_retry);

    int idle = Connect(port);
    Check(idle >= 0, "Could not connect for the timeout test");
    SendRequest(idle,
                "/image.jp2?cnew=http&type=jpp-stream&stream=0&"
                "fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
    Check(ReadResponse(idle).headers.find("HTTP/1.1 200 OK") == 0,
          "Timeout-test channel creation failed");
    CheckClosed(idle, 2000, "Idle channel did not time out");
    close(idle);

    int expiring = Connect(port);
    Check(expiring >= 0, "Could not connect for the admission-timeout test");
    CheckClosed(expiring, 2000, "Unidentified connection did not expire");
    close(expiring);

    int limited[2];
    string limited_ids[2];
    for (int i = 0; i < 2; ++i) {
        int &connection = limited[i];
        connection = Connect(port);
        Check(connection >= 0, "Could not fill the connection limit");
        SendRequest(connection,
                    "/image.jp2?cnew=http&type=jpp-stream&stream=0&"
                    "fsiz=1,1&rsiz=1,1&roff=0,0&len=128");
        Response response = ReadResponse(connection);
        Check(response.headers.find("HTTP/1.1 200 OK") == 0,
              "Connection-limit channel creation failed");
        limited_ids[i] = ChannelId(response.headers);
    }
    Check(limited_ids[0] != limited_ids[1],
          "Two channels received the same random ID");
    int refused = Connect(port);
    Check(refused >= 0, "Could not connect for the connection-limit test");
    CheckClosed(refused, 1000, "Connection limit did not reject a client");
    close(refused);
    close(limited[0]);
    close(limited[1]);

    Check(kill(-primary_server, SIGTERM) == 0,
          "Could not stop the server process group");
    int status = WaitForServer(primary_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "SIGTERM did not stop the server cleanly");

    uint16_t channel_limit_port = ReservePort();
    string channel_limit_text =
            "[listen]\nport = " + to_string(channel_limit_port) +
            // A host name, so that this server resolves its listen address.
            "\naddress = localhost\n"
            "[jpip]\nimage_directory = " + directory + "\nchunk_size = 128\n"
            "[connections]\ninitial_timeout = 1\ntimeout = 1\nlimit = 1\n"
            "[channels]\nlimit = 2\n"
            "[logging]\ndirectory =\nfile_enabled = false\nrequests = false\n";
    WriteFile(directory + "/channel-limit.ini", channel_limit_text.data(),
              channel_limit_text.size());
    server::Config channel_limit_config;
    Check(channel_limit_config.Load((directory + "/channel-limit.ini").c_str(),
                                    error),
          "Could not load the channel-limit configuration");
    pid_t channel_limit_server = StartServer(
            channel_limit_config, directory + "/server-channel-limit");
    int pooled = Connect(channel_limit_port);
    Check(pooled >= 0, "Channel-limit server did not start");
    SendRequest(pooled, "/image.jp2?cnew=http&len=128");
    Check(ReadResponse(pooled).headers.find("HTTP/1.1 200 OK") == 0,
          "First pooled channel was not created");
    SendRequest(pooled, "/image.jp2?cnew=http&len=128");
    Check(ReadResponse(pooled).headers.find("HTTP/1.1 200 OK") == 0,
          "Second pooled channel was not created");
    SendRequest(pooled, "/image.jp2?cnew=http&len=128");
    Response channel_limit_response = ReadResponse(pooled);
    Check(channel_limit_response.headers.find("503 Service Unavailable") !=
                      string::npos &&
                  channel_limit_response.body ==
                      "JPIP channel limit has been reached",
          "The independent channel limit was not enforced");
    CheckClosed(pooled, 1000,
                "Channel-limit response retained its connection");
    close(pooled);
    Check(kill(channel_limit_server, SIGINT) == 0,
          "Could not stop the channel-limit server");
    status = WaitForServer(channel_limit_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "SIGINT did not stop the channel-limit server cleanly");

    // A server that runs out of descriptors keeps its listener and accepts
    // again once connections have closed. The idle clients below outnumber
    // its descriptors, and each is closed by the identification deadline
    // only after it has been accepted.
    uint16_t descriptor_port = ReservePort();
    string descriptor_text =
            "[listen]\nport = " + to_string(descriptor_port) +
            "\naddress = 127.0.0.1\n"
            "[jpip]\nimage_directory = " + directory + "\nchunk_size = 128\n"
            "[connections]\ninitial_timeout = 1\ntimeout = 1\nlimit = 256\n"
            "[channels]\nlimit = 2\n"
            "[logging]\ndirectory =\nfile_enabled = false\nrequests = false\n";
    WriteFile(directory + "/descriptors.ini", descriptor_text.data(),
              descriptor_text.size());
    server::Config descriptor_config;
    Check(descriptor_config.Load((directory + "/descriptors.ini").c_str(), error),
          "Could not load the descriptor-limit configuration");
    rlimit descriptors;
    Check(getrlimit(RLIMIT_NOFILE, &descriptors) == 0,
          "Could not read the descriptor limit");
    rlimit reduced = descriptors;
    reduced.rlim_cur = 64;
    Check(setrlimit(RLIMIT_NOFILE, &reduced) == 0,
          "Could not reduce the descriptor limit");
    pid_t descriptor_server = StartServer(
            descriptor_config, directory + "/server-descriptors", 2);
    Check(setrlimit(RLIMIT_NOFILE, &descriptors) == 0,
          "Could not restore the descriptor limit");
    vector<int> idle_clients;
    for (int i = 0; i < 100; ++i) {
        idle_clients.push_back(Connect(descriptor_port));
        Check(idle_clients.back() >= 0, "Could not connect an idle client");
    }
    for (int idle_client : idle_clients) {
        CheckClosed(idle_client, 5000,
                    "A client beyond the descriptor limit was never accepted");
        close(idle_client);
    }
    int after_exhaustion = Connect(descriptor_port);
    Check(after_exhaustion >= 0, "Could not connect after descriptor exhaustion");
    SendRequest(after_exhaustion, "/image.jp2?cnew=http&len=128");
    Check(ReadResponse(after_exhaustion).headers.find("HTTP/1.1 200 OK") == 0,
          "The server did not serve after descriptor exhaustion");
    close(after_exhaustion);
    Check(kill(descriptor_server, SIGTERM) == 0,
          "Could not stop the descriptor-limit server");
    status = WaitForServer(descriptor_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "The descriptor-limit server did not stop cleanly");

    string blocked_image = directory + "/blocked.jp2";
    Check(mkfifo(blocked_image.c_str(), 0600) == 0,
          "Could not create the blocking image FIFO");
    uint16_t open_timeout_port = ReservePort();
    string open_timeout_text =
            // No address: this server listens on every interface.
            "[listen]\nport = " + to_string(open_timeout_port) + "\naddress =\n"
            "[jpip]\nimage_directory = " + directory + "\nchunk_size = 128\n"
            "[connections]\ninitial_timeout = 1\ntimeout = 1\nlimit = 2\n"
            "[channels]\nlimit = 2\n"
            "[logging]\ndirectory =\nfile_enabled = false\nrequests = false\n";
    WriteFile(directory + "/open-timeout.ini", open_timeout_text.data(),
              open_timeout_text.size());
    server::Config open_timeout_config;
    Check(open_timeout_config.Load((directory + "/open-timeout.ini").c_str(),
                                   error),
          "Could not load the image-open timeout configuration");
    pid_t open_timeout_server = StartServer(
            open_timeout_config, directory + "/server-open-timeout", 2);
    int opening = Connect(open_timeout_port);
    Check(opening >= 0, "Image-open timeout server did not start");
    SendRequest(opening, "/blocked.jp2?cnew=http&len=128");
    this_thread::sleep_for(chrono::milliseconds(50));
    int queued = Connect(open_timeout_port);
    Check(queued >= 0, "Could not queue a second image-open request");
    SendRequest(queued, "/image.jp2?cnew=http&len=128");

    Response opening_timeout = ReadResponse(opening);
    Check(opening_timeout.headers.find("503 Service Unavailable") != string::npos &&
                  opening_timeout.body == "JPIP channel creation timed out",
          "Blocked image open did not return its timeout response");
    Response queued_timeout = ReadResponse(queued);
    Check(queued_timeout.headers.find("503 Service Unavailable") != string::npos &&
                  queued_timeout.body == "JPIP channel creation timed out",
          "Queued image open did not return its timeout response");
    CheckClosed(opening, 1000, "Blocked image-open connection stayed open");
    CheckClosed(queued, 1000, "Queued image-open connection stayed open");
    close(opening);
    close(queued);

    Check(kill(open_timeout_server, SIGINT) == 0,
          "Could not stop the server with a running image-open worker");
    WaitForLog(directory, "Server stopping", "server-open-timeout.");
    int pending_status;
    Check(waitpid(open_timeout_server, &pending_status, WNOHANG) == 0,
          "Server exited while its image-open worker was still blocked");
    int fifo_writer = open(blocked_image.c_str(), O_WRONLY | O_NONBLOCK);
    Check(fifo_writer >= 0, "Blocked image open was not executing");
    close(fifo_writer);
    status = WaitForServer(open_timeout_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "Image-open timeout server did not stop cleanly");

    pid_t failing_server = StartServer(config, directory + "/missing/server");
    status = WaitForServer(failing_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          "Server startup failure did not return an error");

    // A label over 63 characters can not be sent in a DNS query, so this
    // name fails to resolve without waiting for a name server.
    string unresolved_text =
            "[listen]\nport = " + to_string(port) +
            "\naddress = " + string(80, 'x') + "\n"
            "[jpip]\nimage_directory = " + directory + "\nchunk_size = 128\n"
            "[connections]\ninitial_timeout = 1\ntimeout = 1\nlimit = 2\n"
            "[channels]\nlimit = 4\n"
            "[logging]\ndirectory =\nfile_enabled = false\nrequests = false\n";
    WriteFile(directory + "/unresolved.ini", unresolved_text.data(),
              unresolved_text.size());
    server::Config unresolved_config;
    Check(unresolved_config.Load((directory + "/unresolved.ini").c_str(), error),
          "Could not load the unresolved-address configuration");
    pid_t unresolved_server = StartServer(
            unresolved_config, directory + "/server-unresolved");
    status = WaitForServer(unresolved_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          "An unresolvable listen address did not prevent startup");

    // One I/O thread leaves this harness no image-open thread to ask for.
    pid_t threadless_server = StartServer(config, directory + "/server-threads", 1);
    status = WaitForServer(threadless_server);
    Check(WIFEXITED(status) && WEXITSTATUS(status) != 0,
          "A server without image-open threads started");

    string logs = ReadLogs(directory);
    Check(logs.find("Server stopping") != string::npos,
          "Orderly server shutdown was not recorded");

    RemoveDirectory(directory);
    return EXIT_SUCCESS;
}
