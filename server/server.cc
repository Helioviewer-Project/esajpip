#include "server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>

#include "channel_engine.h"
#include "config.h"
#include "jpip/request/request.h"
#include "server/http/connection.h"
#include "server/http/request_head.h"
#include "trace.h"

using namespace std;

namespace {

using asio::ip::tcp;
using server::Connection;
typedef server::FileManager::OpenResult OpenResult;
typedef server::ChannelEngine::GenerateResult GenerateResult;

const char JPIP_HANDLED_HEADER[] =
        "JPIP-handled: tid,cid,cnew=http,stream,len,handled\r\n";
const char COMMON_HEADERS[] =
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n";

bool GenerateChannelId(string *id) {
    unsigned char random[16];
    if (getentropy(random, sizeof random) != 0)
        return false;
    static const char hex[] = "0123456789abcdef";
    id->resize(sizeof random * 2);
    for (size_t i = 0; i < sizeof random; ++i) {
        (*id)[i * 2] = hex[random[i] >> 4];
        (*id)[i * 2 + 1] = hex[random[i] & 15];
    }
    return true;
}

string OptionalHeaders(const jpip::Request &request, bool cnew = false) {
    string result;
    if (request.routing.tid && !cnew)
        result += "JPIP-tid: 0\r\n";
    if (request.routing.handled)
        result += JPIP_HANDLED_HEADER;
    if (request.routing.tid || request.routing.handled || cnew) {
        result += "Access-Control-Expose-Headers: ";
        bool comma = false;
        if (cnew) {
            result += "JPIP-cnew,JPIP-tid";
            comma = true;
        } else if (request.routing.tid) {
            result += "JPIP-tid";
            comma = true;
        }
        if (request.routing.handled) {
            if (comma)
                result += ',';
            result += "JPIP-handled";
        }
        result += "\r\n";
    }
    return result;
}

string ErrorResponse(int code, const char *reason, const string &message,
                     const string &optional_headers) {
    string result = "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n";
    result += optional_headers;
    result += COMMON_HEADERS;
    result += "Content-Type: text/plain\r\nContent-Length: ";
    result += to_string(message.size());
    result += "\r\nConnection: close\r\n\r\n";
    result += message;
    return result;
}

struct Channel;
struct Exchange;

struct Client {
    uint64_t id;
    shared_ptr<Connection> connection;
    Exchange *exchange;
};

// One request on a channel, from its routing to the end of its response.
struct Exchange {
    Exchange(Channel *_channel, Client *_client, jpip::Request _request,
             const server::RequestHead &head)
        : channel(_channel), client(_client), request(std::move(_request)),
          accepts_gzip(head.accepts_gzip), close_connection(head.close) {
        client->exchange = this;
    }

    ~Exchange() { Detach(); }

    Client *Detach() {
        Client *attached = client;
        if (attached && attached->exchange == this)
            attached->exchange = NULL;
        client = NULL;
        return attached;
    }

    Channel *channel;
    Client *client;
    jpip::Request request;
    const bool accepts_gzip;
    const bool close_connection;
    bool responding = false;  // Its connection is generating the response.
};

struct Channel {
    enum State { OPENING, OPEN, ENDING };

    Channel(asio::io_context &control, string _id, uint64_t _number,
            int chunk_size)
        : id(std::move(_id)), number(_number),
          engine(new server::ChannelEngine(chunk_size)), timer(control) {}

    string id;
    uint64_t number;
    unique_ptr<server::ChannelEngine> engine;
    asio::steady_timer timer;
    uint64_t timer_epoch = 0;
    atomic<bool> open_cancelled{false};
    unique_ptr<Exchange> waiting;
    unique_ptr<Exchange> exchange;
    State state = OPENING;
    // The engine is lent to an open, a response, or its destruction. Nothing
    // else may use it, and the channel outlives the loan.
    bool engine_lent = false;
};

const string &Target(const Exchange &exchange) {
    return exchange.request.routing.target ? exchange.request.target
                                           : exchange.request.object;
}

bool UseGzip(const Exchange &exchange) {
    return exchange.request.has_metareq && exchange.accepts_gzip;
}

string SuccessHeaders(const Channel &channel, bool gzip) {
    const Exchange &exchange = *channel.exchange;
    string result = "HTTP/1.1 200 OK\r\n";
    if (exchange.request.routing.cnew) {
        result += "JPIP-cnew: cid=" + channel.id +
                ",path=jpip,transport=http\r\nJPIP-tid: 0\r\n";
        result += OptionalHeaders(exchange.request, true);
    } else {
        result += OptionalHeaders(exchange.request);
    }
    result += COMMON_HEADERS;
    result += "Transfer-Encoding: chunked\r\nContent-Type: image/jpp-stream\r\n";
    if (gzip)
        result += "Content-Encoding: gzip\r\n";
    if (exchange.close_connection)
        result += "Connection: close\r\n";
    result += "\r\n";
    return result;
}

// Three sets of threads:
//   - the control thread runs `control` and owns every member below it:
//     admission, routing, channels and their lifetime. It never touches a
//     connected socket;
//   - the I/O threads run `io`, on which each connection parses, generates
//     and writes on its own strand;
//   - the `opens` pool runs blocking image opens, so its size is the number
//     of images opened at once.
// They communicate only by posting. An exception leaving a handler ends the
// process: the shared state can not be recovered after one.
class Server {
    const server::Config &cfg;
    const tcp::endpoint endpoint;
    const unsigned int io_threads;
    asio::io_context control;
    asio::io_context io;
    asio::thread_pool opens;
    asio::executor_work_guard<asio::io_context::executor_type> control_work;
    asio::executor_work_guard<asio::io_context::executor_type> io_work;
    tcp::acceptor acceptor;
    asio::signal_set signals;
    asio::steady_timer accept_retry;
    map<uint64_t, Client> clients;
    map<string, unique_ptr<Channel> > channels;
    uint64_t next_client = 1;
    uint64_t next_channel = 1;
    bool stopping = false;

    Client *Find(uint64_t id) {
        auto found = clients.find(id);
        return found == clients.end() ? NULL : &found->second;
    }

    // Connections report on their own strands; forward to the control thread.
    Connection::Events ConnectionEvents(uint64_t id) {
        Connection::Events events;
        events.request = [this, id](server::RequestHead &&head) {
            shared_ptr<server::RequestHead> data =
                    make_shared<server::RequestHead>(std::move(head));
            asio::post(control, [this, id, data] {
                if (Client *client = Find(id))
                    Route(*client, std::move(*data));
            });
        };
        events.read_failed = [this, id](Connection::ReadFailure failure,
                                        const string &target) {
            asio::post(control, [this, id, failure, target] {
                if (Client *client = Find(id))
                    ReadFailed(*client, failure, target);
            });
        };
        events.blocked = [this, id] {
            asio::post(control, [this, id] {
                if (Client *client = Find(id))
                    Blocked(*client);
            });
        };
        events.closed = [this, id] {
            asio::post(control, [this, id] {
                if (Client *client = Find(id))
                    Closed(*client);
            });
        };
        return events;
    }

    void Accept() {
        uint64_t id = next_client++;
        shared_ptr<Connection> connection = make_shared<Connection>(
                io, cfg.initial_timeout(), cfg.connection_timeout(),
                cfg.max_chunk_size(), ConnectionEvents(id));
        acceptor.async_accept(connection->Socket(),
                              [this, connection, id](error_code error) {
            if (stopping)
                return;
            if (error) {
                // Out of descriptors, most likely: do not spin on it.
                accept_retry.expires_after(chrono::milliseconds(100));
                accept_retry.async_wait([this](error_code cancelled) {
                    if (!cancelled && !stopping)
                        Accept();
                });
                return;
            }
            if (clients.size() >= static_cast<size_t>(cfg.max_connections())) {
                error_code ignored;
                connection->Socket().close(ignored);
            } else {
                clients[id] = Client{id, connection, NULL};
                LOG("Accepted connection [" << id << "]");
                connection->Start();
            }
            Accept();
        });
    }

    void Route(Client &client, server::RequestHead &&head) {
        if (client.exchange) {
            ERROR("An HTTP connection is already attached to a JPIP exchange");
            client.connection->Abort();
            return;
        }
        jpip::Request request;
        string error;
        if (!request.ParseTarget(head.target, &error)) {
            LOG("Bad request: " << server::EscapeForLog(head.target) << ": "
                                << server::EscapeForLog(error));
            SendError(client, 400, "Bad Request", error, &request);
            if (!request.routing.cnew)
                EndRequestedChannel(request);
            return;
        }
        if (!request.routing.cnew && !request.routing.cid && !request.routing.cclose) {
            client.connection->Abort();
            return;
        }
        if (head.unsupported_body) {
            SendError(client, 400, "Bad Request",
                      "HTTP request bodies are not supported", &request);
            EndRequestedChannel(request);
            return;
        }
        if (cfg.log_requests())
            LOG("Request: " << server::EscapeForLog(head.target));

        if (request.routing.cnew) {
            NewChannel(client, std::move(request), head);
            return;
        }
        auto found = channels.find(request.channel);
        if (found == channels.end() || found->second->state == Channel::ENDING) {
            SendError(client, 503, "Service Unavailable",
                      found == channels.end() ? "JPIP channel does not exist"
                                              : "JPIP channel has ended",
                      &request);
            return;
        }
        Channel &channel = *found->second;
        bool start = !channel.exchange && channel.state == Channel::OPEN;
        if (!start && channel.waiting) {
            SendError(client, 503, "Service Unavailable",
                      "JPIP channel is busy", &request);
            return;
        }
        unique_ptr<Exchange> exchange(new Exchange(
                &channel, &client, std::move(request), head));
        if (start)
            StartRequest(channel, std::move(exchange));
        else
            channel.waiting = std::move(exchange);
    }

    void NewChannel(Client &client, jpip::Request request,
                    const server::RequestHead &head) {
        if (!request.accepts_http) {
            SendError(client, 501, "Not Implemented",
                      "The requested JPIP channel transport is not supported",
                      &request);
            return;
        }
        if (channels.size() >= static_cast<size_t>(cfg.max_channels())) {
            SendError(client, 503, "Service Unavailable",
                      "JPIP channel limit has been reached", &request);
            return;
        }
        string id;
        if (!GenerateChannelId(&id) || channels.count(id) != 0) {
            ERROR("A secure JPIP channel ID can not be generated");
            SendError(client, 500, "Internal Server Error",
                      "Could not create JPIP channel", &request);
            return;
        }
        unique_ptr<Channel> created(new Channel(
                control, id, next_channel++, cfg.max_chunk_size()));
        if (!created->engine->Init(cfg.image_directory())) {
            SendError(client, 500, "Internal Server Error",
                      "The JPIP channel could not be initialized", &request);
            return;
        }
        Channel *channel = created.get();
        channel->exchange.reset(new Exchange(
                channel, &client, std::move(request), head));
        channels[id] = std::move(created);

        // The pool's queue is the FIFO of pending image opens.
        channel->engine_lent = true;
        server::ChannelEngine *engine = channel->engine.get();
        string target = Target(*channel->exchange);
        asio::post(opens, [this, channel, engine, target] {
            OpenResult result = OpenResult::UNREADABLE;
            if (!channel->open_cancelled.load()) {
                try {
                    result = engine->Open(target);
                } catch (const exception &error) {
                    ERROR("Cannot open '" << server::EscapeForLog(target) << "': " << server::EscapeForLog(error.what()));
                } catch (...) {
                    ERROR("Cannot open '" << server::EscapeForLog(target) << "': unknown exception");
                }
            }
            asio::post(control, [this, channel, result] {
                Opened(*channel, result);
            });
        });
    }

    void Opened(Channel &channel, OpenResult result) {
        channel.engine_lent = false;
        if (channel.state == Channel::ENDING) {
            Release(channel);
            return;
        }
        switch (result) {
        case OpenResult::OPENED:
            LOG("The channel " << channel.number
                                << " has been opened for the image '"
                                << server::EscapeForLog(Target(*channel.exchange))
                                << "'");
            channel.state = Channel::OPEN;
            Respond(channel);
            return;
        case OpenResult::NOT_FOUND:
            Fail(channel, 404, "Not Found", "The requested image was not found");
            return;
        case OpenResult::INVALID_PATH:
            Fail(channel, 404, "Not Found", "The requested image path is invalid");
            return;
        case OpenResult::UNSUPPORTED:
            Fail(channel, 404, "Not Found",
                 "The requested image type is not supported");
            return;
        case OpenResult::UNREADABLE:
            Fail(channel, 500, "Internal Server Error",
                 "The requested image could not be read");
            return;
        case OpenResult::INVALID:
            Fail(channel, 404, "Not Found",
                 "The requested image is not a valid supported JPEG 2000 source");
            return;
        }
    }

    void StartRequest(Channel &channel, unique_ptr<Exchange> exchange) {
        StopIdleTimer(channel);
        channel.exchange = std::move(exchange);
        if (!channel.exchange->request.routing.cclose) {
            Respond(channel);
            return;
        }
        Client *client = channel.exchange->client;
        string response = "HTTP/1.1 200 OK\r\n" +
                OptionalHeaders(channel.exchange->request) + COMMON_HEADERS +
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
        BeginTermination(channel);
        channel.exchange.reset();
        if (client)
            client->connection->Reply(std::move(response));
        Release(channel);
    }

    // Lend the engine to the exchange's connection for one response.
    void Respond(Channel &channel) {
        Exchange &exchange = *channel.exchange;
        if (!exchange.client) {
            End(channel);
            return;
        }
        server::ChannelEngine *engine = channel.engine.get();
        jpip::ResponseRequest request = exchange.request;
        bool gzip = UseGzip(exchange);
        string optional_headers = OptionalHeaders(exchange.request);
        bool begun = false;
        Channel *responding = &channel;

        Connection::Response response;
        response.headers = SuccessHeaders(channel, gzip);
        response.close = exchange.close_connection;
        response.generate = [engine, request, gzip, optional_headers, begun](
                char *buffer, int capacity) mutable {
            Connection::Chunk chunk = {Connection::Chunk::MORE, 0, string()};
            int code = 500;
            string error;
            try {
                if (!begun) {
                    begun = engine->Begin(request, gzip, &error);
                    if (!begun)
                        code = 400;
                }
                if (begun) {
                    GenerateResult result =
                            engine->Generate(buffer, capacity, &chunk.length);
                    if (result == GenerateResult::MORE)
                        return chunk;
                    if (result == GenerateResult::COMPLETE) {
                        chunk.status = Connection::Chunk::COMPLETE;
                        return chunk;
                    }
                    error = engine->GetError();
                }
            } catch (const exception &failure) {
                code = 500;
                error = failure.what();
            } catch (...) {
                code = 500;
                error = "Channel processing failed with an unknown exception";
            }
            chunk.status = Connection::Chunk::FAILED;
            chunk.length = 0;
            chunk.failure = ErrorResponse(
                    code, code == 400 ? "Bad Request" : "Internal Server Error",
                    error, optional_headers);
            return chunk;
        };
        response.done = [this, responding](bool complete) {
            asio::post(control, [this, responding, complete] {
                Responded(*responding, complete);
            });
        };
        channel.engine_lent = true;
        exchange.responding = true;
        exchange.client->connection->Respond(std::move(response));
    }

    // The connection has already resumed, replied, or closed on its own.
    void Responded(Channel &channel, bool complete) {
        channel.engine_lent = false;
        if (channel.state != Channel::ENDING && !complete)
            BeginTermination(channel);
        channel.exchange.reset();
        if (channel.state == Channel::ENDING) {
            Release(channel);
        } else if (channel.waiting) {
            unique_ptr<Exchange> waiting = std::move(channel.waiting);
            StartRequest(channel, std::move(waiting));
        } else {
            StartIdleTimer(channel);
        }
    }

    void StartIdleTimer(Channel &channel) {
        uint64_t epoch = ++channel.timer_epoch;
        string id = channel.id;
        channel.timer.expires_after(chrono::seconds(cfg.connection_timeout()));
        channel.timer.async_wait([this, id, epoch](error_code) {
            auto found = channels.find(id);
            if (found == channels.end() || found->second->timer_epoch != epoch)
                return;
            LOG("The channel " << found->second->number << " timed out");
            End(*found->second);
        });
    }

    void StopIdleTimer(Channel &channel) {
        ++channel.timer_epoch;
        channel.timer.cancel();
    }

    void BeginTermination(Channel &channel) {
        channel.state = Channel::ENDING;
        StopIdleTimer(channel);
        channel.open_cancelled.store(true);
        if (!channel.waiting)
            return;
        unique_ptr<Exchange> waiting = std::move(channel.waiting);
        if (Client *client = waiting->Detach())
            SendError(*client, 503, "Service Unavailable",
                      "JPIP channel has ended", &waiting->request);
    }

    // Destroy an ending channel once nothing holds its engine. The channel
    // is erased from a later handler, never inside a caller still using it.
    void Release(Channel &channel) {
        if (channel.engine_lent)
            return;
        channel.engine_lent = true;
        server::ChannelEngine *engine = channel.engine.release();
        string id = channel.id;
        asio::post(io, [this, engine, id] {
            delete engine;
            asio::post(control, [this, id] {
                channels.erase(id);
                FinishStop();
            });
        });
    }

    // End a channel whose client can still be told why.
    void Fail(Channel &channel, int code, const char *reason,
              const string &message) {
        Exchange *exchange = channel.exchange.get();
        Client *client = exchange ? exchange->client : NULL;
        BeginTermination(channel);
        if (client && exchange->responding)
            client->connection->Abort();
        else if (client)
            SendError(*client, code, reason, message, &exchange->request);
        if (exchange && !exchange->responding)
            channel.exchange.reset();
        Release(channel);
    }

    void End(Channel &channel) {
        if (channel.state != Channel::ENDING) {
            Exchange *exchange = channel.exchange.get();
            BeginTermination(channel);
            if (exchange && exchange->client)
                exchange->client->connection->Abort();
            if (exchange && !exchange->responding)
                channel.exchange.reset();
        }
        Release(channel);
    }

    void SendError(Client &client, int code, const char *reason,
                   const string &message, const jpip::Request *request) {
        client.connection->Reply(ErrorResponse(
                code, reason, message,
                request ? OptionalHeaders(*request) : string()));
    }

    void EndRequestedChannel(const jpip::Request &request) {
        if (!request.routing.cid && !request.routing.cclose)
            return;
        auto found = channels.find(request.channel);
        if (found != channels.end())
            End(*found->second);
    }

    void ReadFailed(Client &client, Connection::ReadFailure failure,
                    const string &target) {
        jpip::Request request;
        string error;
        request.ParseTarget(target, &error);
        if (failure == Connection::ReadFailure::TOO_LARGE)
            SendError(client, 431, "Request Header Fields Too Large",
                      "HTTP request head is too large", &request);
        else
            SendError(client, 400, "Bad Request", "Invalid HTTP request head",
                      &request);
        EndRequestedChannel(request);
    }

    // A connection has waited a full timeout for its request to be answered.
    void Blocked(Client &client) {
        Exchange *exchange = client.exchange;
        if (!exchange || exchange->responding)
            return;  // An answer is already on its way to the connection.
        Channel *channel = exchange->channel;
        if (channel->waiting.get() == exchange) {
            unique_ptr<Exchange> waiting = std::move(channel->waiting);
            waiting->Detach();
            SendError(client, 503, "Service Unavailable",
                      "JPIP channel request timed out", &waiting->request);
        } else if (channel->state == Channel::OPENING) {
            Fail(*channel, 503, "Service Unavailable",
                 "JPIP channel creation timed out");
        }
    }

    void Closed(Client &client) {
        if (Exchange *exchange = client.exchange) {
            Channel *channel = exchange->channel;
            if (channel->waiting.get() == exchange) {
                channel->waiting.reset();
            } else {
                exchange->Detach();
                End(*channel);
            }
        }
        uint64_t id = client.id;
        clients.erase(id);
        FinishStop();
    }

    void Stop() {
        if (stopping)
            return;
        stopping = true;
        LOG("Server stopping");
        error_code ignored;
        acceptor.close(ignored);
        accept_retry.cancel();
        signals.cancel(ignored);
        for (auto &entry : clients)
            entry.second.connection->Abort();
        for (auto &entry : channels)
            End(*entry.second);
        FinishStop();
    }

    void FinishStop() {
        if (!stopping || !clients.empty() || !channels.empty())
            return;
        io_work.reset();
        control_work.reset();
    }

public:
    Server(const server::Config &_cfg, const tcp::endpoint &_endpoint,
           unsigned int _io_threads, unsigned int open_threads)
        : cfg(_cfg), endpoint(_endpoint), io_threads(_io_threads),
          control(1), io(static_cast<int>(_io_threads)), opens(open_threads),
          control_work(asio::make_work_guard(control)),
          io_work(asio::make_work_guard(io)), acceptor(control),
          signals(control), accept_retry(control) {}

    bool Initialize(string *failure) {
        error_code error;
        acceptor.open(tcp::v4(), error);
        if (!error)
            acceptor.set_option(tcp::acceptor::reuse_address(true), error);
        if (!error)
            acceptor.bind(endpoint, error);
        if (!error)
            acceptor.listen(asio::socket_base::max_listen_connections, error);
        if (!error)
            signals.add(SIGINT, error);
        if (!error)
            signals.add(SIGTERM, error);
        if (error) {
            *failure = error.message();
            return false;
        }
        signals.async_wait([this](error_code cancelled, int) {
            if (!cancelled)
                Stop();
        });
        Accept();
        return true;
    }

    void Run() {
        vector<thread> threads;
        for (unsigned int i = 0; i < io_threads; ++i)
            threads.emplace_back([this] { io.run(); });
        control.run();
        for (thread &io_thread : threads)
            io_thread.join();
        opens.join();
    }
};

// The configured listen address; empty means every IPv4 interface.
bool ListenEndpoint(const server::Config &cfg, tcp::endpoint *endpoint) {
    unsigned short port = static_cast<unsigned short>(cfg.port());
    if (cfg.address().empty()) {
        *endpoint = tcp::endpoint(tcp::v4(), port);
        return true;
    }
    asio::io_context io;
    tcp::resolver resolver(io);
    error_code error;
    tcp::resolver::results_type found = resolver.resolve(
            tcp::v4(), cfg.address(), "", tcp::resolver::flags(), error);
    if (error || found.empty())
        return false;
    *endpoint = tcp::endpoint(found.begin()->endpoint().address(), port);
    return true;
}

} // namespace

namespace server {

int RunServer(const Config &cfg, const string &log_name,
              const string &description, unsigned int io_threads,
              unsigned int open_threads) {
    if (io_threads == 0 || open_threads == 0)
        return CERR("The thread counts must be positive");
    tcp::endpoint endpoint;
    if (!ListenEndpoint(cfg, &endpoint))
        return CERR("The listen address '" << cfg.address()
                                           << "' can not be resolved");
    if (!trace::Initialize(log_name))
        return -1;
    LOG(description << " started (PID = " << getpid() << ")");

    int result = -1;
    {
        Server server(cfg, endpoint, io_threads, open_threads);
        string failure;
        if (server.Initialize(&failure)) {
            server.Run();
            result = 0;
        } else {
            ERROR("The server can not be initialized: " << failure);
        }
    }
    trace::Drain();
    return result;
}

} // namespace server
