# Connections and JPIP channels

This document explains how a client creates, uses, replaces, and closes an
esajpip channel over HTTP, and when the server accepts a connection, holds
channel state, and releases it. The [README](README.md#concepts) defines the
recurring terms; [JPIP_PROFILE.md](JPIP_PROFILE.md) lists the supported JPIP
fields and JPEG 2000 source restrictions.

## Connection model

A TCP connection carries HTTP requests and responses. A JPIP channel holds the
state for one target: its image index, cache model, response progress, and the
`cid` returned by the server. The channel can outlive a particular TCP
connection.

esajpip supports one target and one request/response at a time per channel. A
client may keep using the original HTTP connection or send a later request for
the same `cid` on a new connection. One connection may carry requests for
different channels, which allows normal browser and reverse-proxy connection
pooling. Requests must use HTTP/1.1 `GET`.

[JPEG 2000 Part 9](https://www.itu.int/rec/T-REC-T.808/en) separates HTTP
connections from JPIP sessions. A persistent HTTP connection neither establishes
nor identifies the session, and a later request may arrive on another
connection. esajpip implements the smaller model it needs: the channel is the
session, and it owns the state for one target and one serial request stream.

The examples below omit optional JPIP fields for clarity. Target paths are
relative to the configured image directory. Paths and `target` values are
percent-decoded once; clients should escape reserved characters in file names.
Malformed escapes, NUL and decoded `..` path segments are rejected.

## Create a channel

The first request names a target and includes `cnew`. The target may be the URI
path:

```http
GET /movie.jpx?cnew=http&type=jpp-stream&stream=0&len=2097152 HTTP/1.1
Host: server.example:8900

```

The `cnew` value is a comma-separated list of the transports the client
accepts. The server implements only `http`, so the list must contain it;
JHelioviewer sends `cnew=http`.

It may instead be supplied through `target`:

```http
GET /jpip?target=movie.jpx&cnew=http&type=jpp-stream&stream=0&len=2097152 HTTP/1.1
Host: server.example:8900

```

A successful response creates the channel and returns its opaque identifier:

```http
HTTP/1.1 200 OK
JPIP-cnew: cid=0123456789abcdef0123456789abcdef,path=jpip,transport=http
JPIP-tid: 0
Transfer-Encoding: chunked
Content-Type: image/jpp-stream

```

`JPIP-tid: 0` means that the server does not assign a stable target identifier.
The client must not use it to recover cache state in another channel.

Image responses contain a chunked JPP stream. Each completed response ends with
a JPIP end-of-response message, followed by the terminating HTTP chunk. If the
request contains `metareq` and `Accept-Encoding` contains `gzip`, the chunked
body is gzip encoded and the response includes `Content-Encoding: gzip`.

## Continue a channel

Every later image request includes the returned `cid`:

```http
GET /jpip?cid=0123456789abcdef0123456789abcdef&stream=7&fsiz=4096,4096,closest&rsiz=1024,1024&roff=0,0&len=2097152 HTTP/1.1
Host: server.example:8900

```

The request may use the existing persistent connection. It may also use a new
connection. This is useful for browser and HTTP-library clients that do not
control socket reuse.

The server does not run concurrent requests or cancel an active response within
one channel. One later request may wait for that channel. An additional request
for the busy channel receives `503 Service Unavailable`. Requests for other
channels remain independent, even when a browser sends them on the same
connection.

Closing a persistent connection between complete responses does not close the
channel immediately. The server waits for another connection carrying its
`cid`, up to `connections.timeout`.

If a request contains `Connection: close`, the server completes that response
with `Connection: close` and closes the connection. The JPIP channel remains
available for a later connection carrying its `cid`.

## Close a channel

Send `cclose` with the channel identifier:

```http
GET /jpip?cclose=0123456789abcdef0123456789abcdef HTTP/1.1
Host: server.example:8900

```

The server replies with `200 OK`, `Content-Length: 0`, and
`Connection: close`, then closes the connection and releases the channel state.
`cclose=*`, channel lists, and session-wide closure are not supported.

## HTTP responses

These are all HTTP status codes emitted by the server:

| Status | When it is returned | Effect on the channel |
| --- | --- | --- |
| `200 OK` | A channel is created, a channel request is served, or `cclose` succeeds. Image responses use `Transfer-Encoding: chunked` and `Content-Type: image/jpp-stream`; `cclose` has `Content-Length: 0`. | The channel remains available after an image response. A successful `cclose` ends it. |
| `400 Bad Request` | The HTTP request head, body framing, supported JPIP fields, cache model, codestream selection, or window is invalid. The response body identifies the invalid field or constraint. | The connection and any referenced channel are closed. The rejected request does not modify the channel cache before termination. |
| `404 Not Found` | A `cnew` request names a target that is missing, has an invalid client-supplied path, uses an unsupported file type, or is not accepted by the supported JPEG 2000 served profile. | No usable channel is created; the connection is closed. Correct the requested target or repository source. |
| `431 Request Header Fields Too Large` | An identified connection sends more than 2 KiB for the request line or 4 KiB for the headers. | The connection and any channel identified by the request target are closed. |
| `501 Not Implemented` | A valid `cnew` request lists no transport the server implements. The response has no `JPIP-cnew` header. | No usable channel is created; the connection is closed. Retry with `http` in the transport list. |
| `500 Internal Server Error` | The selected source is unreadable, or the server encounters another internal failure such as being unable to generate a channel ID. The response body identifies the failure category. | The connection and channel are closed. An unreadable source normally requires correcting repository access or storage. |
| `503 Service Unavailable` | A request names an unknown or ended channel, both request slots for a channel are occupied, a channel wait expires, or the active-channel limit has been reached. The response body distinguishes these cases. | A routing rejection leaves any existing channel unchanged. After an ended or unknown-channel response, create a new channel. |

Every response that closes its connection includes `Connection: close`. Error
responses contain a short plain-text body. Responses include CORS and no-cache
headers, and `cnew` exposes `JPIP-cnew` and `JPIP-tid` to browser clients. TLS
and the rest of the deployment boundary are covered in the
[README](README.md#reverse-proxy-and-security).

Some failures close the socket without an HTTP response:

- The initial bytes are not a recognizable HTTP/1.1 JPIP `GET` request.
- The initial request does not arrive before `connections.initial_timeout`.
- The connection limit, `connections.limit`, has been reached.
- The socket fails while a request or response is in progress.
- The server process exits.

Requests must not contain a body. A nonzero `Content-Length`, an invalid
`Content-Length`, or any `Transfer-Encoding` receives `400 Bad Request`, and
the connection is closed after the response.

A response that ends before its HTTP chunk terminator or JPIP end-of-response
message is incomplete. The client must discard that response. Since all channel
state is lost when the server process exits, recovery requires a new channel.
For the same immutable target, a client can retain its cache and declare its
complete bins and exact byte prefixes through `model` before resuming requests.
The [client library](client/README.md#failures) implements bounded recovery;
otherwise rebuild the client cache from the new channel's responses.

## Request and connection limits

The parser limits a request line to 2 KiB and identifies only lines containing
`cnew`, `cid`, or a usable `cclose`. The identification deadline is absolute:
sending a few bytes does not extend `connections.initial_timeout`. Rejected
connections do not allocate a channel or JPEG 2000 state.

After identification, the request line is limited to 2 KiB and the headers
(including their terminating blank line) to a separate 4 KiB.
`connections.timeout` sets an absolute deadline to complete that head. After a
response, it also bounds the wait for the next request, including its head.
Incoming bytes do not restart either deadline. The same setting limits
response-write progress, a busy-channel wait, and channel idle time; each
completed write restarts only the write-progress wait. Both timeout settings
must be positive.

For `cnew`, one deadline covers the full queue and image-open time. Generating
a response has no deadline of its own; the write-progress wait starts with its
first chunk.

`connections.limit` limits physical HTTP connections.
`channels.limit` separately limits active JPIP channels. A channel
can have one active request and at most one waiting request. A connection may
be reused for multiple channels, but responses on that connection remain in
request order. Clients should close connections they no longer use.

## Channel lifetime

A channel ends after a successful `cclose`, a malformed request, an invalid
image request, a response failure, or an inactivity timeout. It also disappears
when the server process exits. Channels are not restored after a restart.

The server deliberately ends a channel after an incomplete response. Its cache
model may already include bytes that did not reach the client, so reusing that
state could make a later response incomplete from the client's point of view.

## Ownership

One control thread owns the listener, signals, admission, the channel table,
and every channel's routing state. It never reads or writes a connected socket.

Each connection owns its socket, request-head parser, deadline timer, and the
response in flight. That state is used only on the connection's strand, which
runs on one of the server's I/O threads at a time. The control thread and the
connections exchange messages and share no state.

Each channel owns its `FileManager`, `ImageIndex`, linked-JPX graph,
`DataBinServer`, cache model, and traversal state. The control thread lends
that state to one user at a time: the pool thread that opens the image, then
the connection generating each response, and finally the thread that destroys
it. It is never shared with another channel. A response is generated on the
strand of the connection that receives it, and each chunk is written as soon
as it is generated, so the work may move between I/O threads only at chunk
boundaries.

## Admission and dispatch

```text
TCP accept on the control thread
    |
    v
connection strand: absolute identification deadline
    |
    +-- no complete request line before deadline -----> close
    +-- unsupported or unrelated traffic -------------> close
    |
    v
bounded HTTP parsing, then cnew/cid/cclose routing on the control thread
    |
    v
selected channel: active slot or one waiting slot
```

llhttp consumes each request head once. A 2 KiB request-line limit and 4 KiB
header-section limit bound parser storage to 6 KiB in total. Rejected and expired connections do
not allocate JPEG 2000 state.

For `cnew`, the control thread reserves an opaque channel ID and queues the
image open on a separate pool of eight threads. That pool's queue is the FIFO
of pending opens, and opening never occupies an I/O thread. For `cid` or
`cclose`, the control thread routes the parsed request to the channel; the
connection keeps its socket. Each channel has one active and one waiting
request slot.

## Termination

When a channel terminates, the control thread cancels its image open if that
is still queued, aborts the connection of a response in progress, waits for
the channel state to be returned, then releases it. A waiting request receives
`503` immediately. An incomplete response closes its connection without
inventing an HTTP terminator or JPIP end-of-response marker.

The control thread handles `SIGINT` and `SIGTERM`. The server stops admission,
ends its channels, closes its connections, drains the log, and exits. A host
process manager or container runtime may restart it, but accepted sockets and
channel state are not preserved. Clients must create new channels after a
restart.

## Deliberate limits

- `connections.limit` and `channels.limit` independently limit
  physical connections and JPIP channels.
- A channel has one active request/response and at most one waiting request.
- One connection can serve different channels, but has only one response in
  flight and one retained request.
- Channel JPEG 2000 state is never reconstructed or shared between channels.

These limits keep ownership clear and work bounded while preserving the current
JHelioviewer wire behavior. They still allow an HTTP library or browser to
replace its underlying connection without losing the JPIP channel.

## Source layout

The source responsibilities are (paths under `server/`):

| Source | Responsibility |
| --- | --- |
| `main.cc` | Configuration and server startup |
| `server.cc` | Listener, signals, control thread, admission, routing, and channel lifetime |
| `http/connection.cc` | Connection strand: parsing, deadlines, and generated, ordered response writes |
| `http/request_head.cc` | Bounded llhttp request-head parser |
| `channel_engine.cc` | Socket-free JPIP and JPEG 2000 processing for one channel |
| `vendor/asio/` | Vendored Asio headers; see its README |
