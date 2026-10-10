# gRPCClient API Reference

Header: `Supplimentary/gRPCClient.h`

`gRPCClient` speaks gRPC over `Fetch`/libcurl. It offers two layers:

- a **transport layer** that exposes gRPC frames directly and supports streaming in
  both directions;
- a **protobuf-c service wrapper** that plugs into generated stubs, at the cost of
  being unary-only.

Status: within this repository the module is exercised only by `Examples/gRPCClient`.

`struct GRPCTransmission` embeds `struct FetchTransmission` as its first member
(`super`), so a gRPC call is a Fetch transmission driven by the same `Fetch` instance
and ring. See [Fetch.md](Fetch.md) and [gRPC.md](gRPC.md).

## Methods

```c
struct GRPCMethod* CreateGRPCMethod(const char* location, const char* package, const char* service,
                                    const char* name, const char* token, long timeout, char resolution);
void HoldGRPCMethod(struct GRPCMethod* method);
void ReleaseGRPCMethod(struct GRPCMethod* method);
```

A `GRPCMethod` is the prepared, reusable description of one RPC endpoint:

- The path is composed as `/<package>.<service>/<name>`, or `/<service>/<name>` when
  `package` is `NULL`, and applied over `location`; user, password, options, query and
  fragment of `location` are discarded. `CreateGRPCMethod()` returns `NULL` when
  `location` does not parse.
- HTTP version defaults to `CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE`, and switches to
  `CURL_HTTP_VERSION_2TLS` when the scheme is `https`. Plaintext gRPC therefore works
  without an upgrade round-trip.
- `token`, when given, becomes an `authorization: Bearer` header.
- `timeout` with `resolution` `'S'` (seconds) or `'m'` (milliseconds) becomes the
  `grpc-timeout` header and also libcurl's transfer timeout, so an expired deadline
  normally ends as `-CURLE_OPERATION_TIMEDOUT` rather than `GRPC_STATUS_DEADLINE_EXCEEDED`.
  Another resolution, or `timeout <= 0`, sets neither.
- The object is reference counted (`count` starts at `1`). One method can back many
  concurrent transmissions; `HoldGRPCMethod()` / `ReleaseGRPCMethod()` manage that.

`MakeGRPCTransmission()` **does not take a reference of its own.** The method carries the
`CURLU` handle and the header list that libcurl uses without copying, so it has to stay
alive until every transmission made from it has completed — take a reference with
`HoldGRPCMethod()` if the method could otherwise be released first.

## Transport Layer

```c
struct GRPCTransmission* MakeGRPCTransmission(struct Fetch* fetch, struct GRPCMethod* method,
                                             HandleGRPCEventFunction function, void* closure);
void CancelGRPCTransmission(struct GRPCTransmission* transmission);
```

### Event callback

```c
typedef int (*HandleGRPCEventFunction)(void* closure, struct GRPCTransmission* transmission,
                                       int reason, int parameter, char* data, size_t length);
```

| Reason | `parameter` | `data` / `length` |
| --- | --- | --- |
| `GRPCCLIENT_REASON_FRAME` | Frame flags, i.e. `GRPC_FLAG_COMPRESSED` | One complete message body |
| `GRPCCLIENT_REASON_STATUS` | See below | See below; `length` is `0` |

`GRPCCLIENT_REASON_STATUS` carries one of four things:

| Outcome | `parameter` | `data` |
| --- | --- | --- |
| HTTP 200 | `grpc-status`; `GRPC_STATUS_OK` when the response carried none | `grpc-message`, or `NULL` |
| Another HTTP code | That code, positive | `NULL` |
| libcurl failure | `-CURLcode` | `curl_easy_strerror()` text |
| Cancellation or `ReleaseFetch()` | `FETCH_STATUS_CANCELLED` or `FETCH_STATUS_INCOMPLETE` | `NULL` |

- **Frames arrive already decompressed.** A `GRPC_FLAG_COMPRESSED` frame is inflated
  into a scratch buffer before the callback sees it, so `parameter` is informational.
  Frames larger than 16 MiB, compressed frames that inflate past 16 MiB, and gzip that
  cannot be decoded abort the transfer, which then ends with `-CURLE_WRITE_ERROR`.
- A non-zero return from the `FRAME` handler aborts the transfer the same way; the return
  value of the `STATUS` handler is ignored.
- `data` points into module-owned memory that is reused by the next frame. Decode or
  copy inside the callback.
- **`GRPCCLIENT_REASON_STATUS` is delivered exactly once and last.**
- After that callback returns, the transmission, all its frames and the status message
  are freed. Clear any stored pointer inside the `STATUS` branch; no function may be
  called on the transmission afterwards.

`CancelGRPCTransmission()` aborts an in-flight call; teardown then runs through the
same path, so the handler still receives `GRPCCLIENT_REASON_STATUS`, with
`FETCH_STATUS_CANCELLED` in `parameter` and no message. Outside a handler the status
callback runs before `CancelGRPCTransmission()` returns; from inside the status callback the
call does nothing.

**Cancelling from a frame handler is allowed** — "I have what I needed, stop the stream"
is the natural way to end a server stream. The cancellation is deferred in that case:
frames are dispatched from inside libcurl's write callback, where the easy handle must
not be destroyed, so the transfer is aborted instead and the completion path releases
everything afterwards. The status callback is delivered on the next pass rather than
inside the cancelling call, and it looks the same as any other cancellation. Frames still
queued behind the current one are dropped.

### Sending

```c
struct GRPCFrame* AllocateGRPCFrame(struct GRPCTransmission* transmission, size_t length);
void TransmitGRPCFrame(struct GRPCFrame* frame);

int TransmitGRPCMessage(struct GRPCTransmission* transmission, const ProtobufCMessage* message, int final);
```

`TransmitGRPCMessage()` is the normal entry point: it packs `message` into a frame and
queues it, and when `final` is non-zero also queues the end-of-stream marker. It
returns the number of frames queued — `2` for a final message, `1` for a non-final
one, `0` if nothing could be allocated.

A message with **no field set is still framed.** It packs into zero bytes, which is a
valid gRPC message, and goes out as a bare five-byte frame header declaring a length of
zero — so a request type like `google.protobuf.Empty`, or a proto3 message whose only
field is left at its default, behaves like any other. A return value below the expected
count therefore means an allocation failure, never an empty payload.

```c
TransmitGRPCMessage(transmission, (ProtobufCMessage*)&request, 0);   // one more to come
TransmitGRPCMessage(transmission, (ProtobufCMessage*)&request, 1);   // last, closes the stream
```

The raw pair is there for payloads that are not protobuf-c messages, and follows the
same shape as `CURLWSCore`: allocate, write the payload at `frame->data`, set
`frame->length` if it is shorter than requested (it is preset to the requested length),
then transmit. `frame->buffer` begins with the five-byte gRPC header that
`TransmitGRPCFrame()` fills in, and the payload follows it. Setting `frame->data = NULL` before `TransmitGRPCFrame()` makes the entry an
**end-of-stream marker** rather than a payload — that is exactly what `final` does.
`TransmitGRPCFrame()` takes ownership; frames are recycled through a per-transmission
free list.

Sending is driven by libcurl's read callback, which parks itself with
`CURL_READFUNC_PAUSE` whenever the queue runs dry. `TransmitGRPCFrame()` un-pauses it —
but only when it enqueues into an empty queue, since a non-empty queue means the sender
is already running. There is no "frame has been sent" event and the queue is unbounded,
so an application that can outrun its link has to account for that itself.

## ProtobufC Service Wrapper

```c
ProtobufCService* CreateGRPCService(struct Fetch* fetch, const ProtobufCServiceDescriptor* descriptor,
                                    const char* location, const char* token, long timeout,
                                    char resolution, HandleGRPCErrorFunction function, void* closure);
```

Returns a `ProtobufCService*` that generated protobuf-c stubs can be called against
directly:

```c
ProtobufCService* service = CreateGRPCService(fetch, &demo__echoer__descriptor,
                                             "http://localhost:50051", NULL, 0, 0, HandleError, NULL);

demo__echoer__unary_echo(service, &request, HandleEchoReply, NULL);   // demo.Echoer/UnaryEcho
...
protobuf_c_service_destroy(service);
```

`GRPCMethod` entries are filled in lazily, one per method of the descriptor, the first
time each is called.

**Unary calls only.** `ProtobufCClosure` may be invoked once per call by protobuf-c's
own contract, so the wrapper delivers the first response message and ignores the rest
of a server stream. Use the transport layer for anything streaming.

### Lifetime of a service

The service is reference counted, and **every call in flight holds a reference.**
`protobuf_c_service_destroy()` therefore releases the caller's reference rather than
freeing immediately: a service destroyed while calls are outstanding stays alive until
the last of them has reported its status. Each call takes its reference before it is
armed and releases it from the completion path, so cancelling or failing a call is
balanced exactly like a call that succeeds.

So a service can be destroyed without draining its calls first:

```c
protobuf_c_service_destroy(service);   // returns while calls may still be running
ReleaseFetch(fetch);                   // completes them with FETCH_STATUS_INCOMPLETE
```

### Error reporting

```c
typedef void (*HandleGRPCErrorFunction)(void* closure, struct GRPCService* service,
                                        const char* method, int status, const char* message);
```

Two channels, each with its own rule:

- the reply closure runs **exactly once** per call — with the response message if one
  arrived, otherwise with `NULL` at status time, so a caller that only watches the reply
  still notices a failure;
- `function`, when supplied, runs when the status is not `GRPC_STATUS_OK`, **and** when no
  response message was delivered at all, even if the status was OK. protobuf-c allows the
  closure to run once, so a missing reply cannot be signalled any other way.

The two are therefore not a pair. A call that succeeds **and delivers a reply** runs the
closure with it and never reaches the error handler. A call that fails **after** its reply
was delivered is reported through the error handler alone — the closure has already run,
with the real message. Only a call that fails without a reply uses both.

`status` follows the same convention as the transport layer: a gRPC status when the call
reached gRPC level, otherwise a negative Fetch completion code. `method` is the name from
the method descriptor, and it is set before the first frame is queued, so it is present
even when the call fails on its way out.

## Rules

- All callbacks run in the ring thread: normally from `WaitForFastRing()`, and
  synchronously from `CancelGRPCTransmission()`, `ReleaseFetch()` and a service call that
  fails to start.
- Do not use the transmission pointer after `GRPCCLIENT_REASON_STATUS`.
- Release a method only after every transmission made from it has reported its status,
  since libcurl reads its URL handle and header list directly. `ReleaseFetch()` reports
  all live transmissions with `FETCH_STATUS_INCOMPLETE`, so it may come before
  `ReleaseGRPCMethod()`.
- A service outlives its own calls on its own, but keeps the `Fetch` pointer without a
  reference: do not invoke it after its `Fetch` is released.

## Lifecycle

`gRPCClient` creates nothing on the ring itself. Each transmission is a libcurl easy handle
added to a `Fetch` instance, and `Fetch` owns the ring objects (see
[Fetch.md](Fetch.md#ring-lifecycle)). A transmission ends in exactly one
`GRPCCLIENT_REASON_STATUS`, after which `Fetch` frees the easy handle and the module frees the
queued and recycled frames, the buffers, the status message and the transmission. Release the
`Fetch`, which completes all transmissions, before the ring: if the ring is released first, no
gRPC callback is delivered and the `Fetch` can no longer be released safely. Nothing in the
module is locked.
