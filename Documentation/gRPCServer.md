# gRPCServer API Reference

Header: `Supplimentary/gRPCServer.h`

`gRPCServer` provides server-side gRPC dispatch on top of `H2OCore` and protobuf-c.

Status: within this repository the module is exercised only by `Examples/gRPCServer`.

## Request Dispatch

```c
typedef int (*AuthorizeGRPCRequestFunction)(
  struct GRPCDispatch* dispatch,
  const ProtobufCMethodDescriptor* descriptor,
  h2o_req_t* request);

typedef void (*HandleGRPCRequestFunction)(
  struct GRPCInvocation* invocation,
  int reason,
  uint8_t* data,
  size_t length);

int HandleGRPCDispatchRequest(h2o_handler_t* handler, h2o_req_t* request);
```

Invocation reasons:
- `GRPC_IV_REASON_CREATED` (0) - the invocation was created, before any payload
- `GRPC_IV_REASON_RECEIVED` (1) - `data`/`length` carry the serialized bytes of one
  request message, de-framed and inflated; valid during the callback only
- `GRPC_IV_REASON_FINISHED` (2) - the client half-closed, no further messages. Not
  delivered once `GRPC_IV_FLAG_IGNORING` is set
- `GRPC_IV_REASON_FAILED` (3) - either the input was malformed or too large, in which
  case the module has already queued an error status (`FAILED_PRECONDITION` for a
  compressed frame without `grpc-encoding: gzip`, `INTERNAL` for an allocation failure,
  `RESOURCE_EXHAUSTED` for a frame over 16 MiB or a failed inflate, `DATA_LOSS` for a
  truncated body) and set `GRPC_IV_FLAG_IGNORING`; or H2O stopped the response, which
  happens when the request is disposed and is followed by `GRPC_IV_REASON_RELEASED`. Both
  may happen to one invocation
- `GRPC_IV_REASON_RELEASED` (4) - the invocation is being destroyed, release
  module state here; `invocation->request` must not be touched any more

Without request streaming, the sequence up to `GRPC_IV_REASON_FINISHED` runs synchronously
inside `HandleGRPCDispatchRequest()`.

## Invocation Flags

`invocation->flags` reports the state of the stream:

- `GRPC_IV_FLAG_ACTIVE` (`1 << 0`) - the stream is live; cleared before
  `GRPC_IV_REASON_RELEASED`. `TransmitGRPCPing()`, `TransmitGRPCReply()` and
  `TransmitGRPCStatus()` return `-EINVAL` once it is clear
- `GRPC_IV_FLAG_SENDING` (`1 << 1`) - a non-empty frame has been handed to H2O, so the
  gRPC status is emitted as HTTP trailers rather than headers
- `GRPC_IV_FLAG_IGNORING` (`1 << 2`) - inbound data is being discarded (the call was
  already answered or rejected)
- `GRPC_IV_FLAG_INPUT_GZIP` (`1 << 3`) - the request declared `grpc-encoding: gzip`
  exactly, so compressed request messages are accepted
- `GRPC_IV_FLAG_OUTPUT_GZIP` (`1 << 4`) - the client listed `gzip` in
  `grpc-accept-encoding`; replies sent with `GRPC_FLAG_COMPRESSED` are compressed

## Wiring the Dispatch into H2O

`HandleGRPCDispatchRequest()` is an `h2o_handler_t` entry point, registered as an
`H2ORoute` whose closure is a `struct GRPCDispatch`:

```c
struct GRPCDispatch dispatch =
{
  .descriptor = &demo__echoer__descriptor,
  .authorize  = AuthorizeRequest,          // optional
  .handle     = HandleRequest,
  .closure    = self
};

struct H2ORoute routes[] =
{
  { GetGRPCServicePath(&demo__echoer__descriptor), 0, H2OCORE_ROUTE_OPTION_STREAMING, HandleGRPCDispatchRequest, &dispatch },
  { NULL }
};
```

`GetGRPCServicePath(service)` builds `"/<service name>/"` on the stack from a
`ProtobufCServiceDescriptor`, which is exactly the path prefix gRPC uses. It relies on
`alloca()`, so the result lives until the enclosing function returns — fine for a
route table built in the same frame as `CreateH2OCore()`.

`H2OCORE_ROUTE_OPTION_STREAMING` is what makes request bodies arrive incrementally,
which is required for client streaming.

### Request acceptance

Before any invocation is created, the dispatch declines anything that is not gRPC: the
method must be `POST`, `content-type` must be exactly `application/grpc` (so
`application/grpc+proto` is not accepted), and `te` must be `trailers`. A declined request
is left to the next H2O handler. The path is split at the last `/` into service and method,
and the method is resolved against the descriptor. An unknown service or method is answered
with `grpc-status: GRPC_STATUS_UNIMPLEMENTED` in the trailers of an empty HTTP 200 response,
and no invocation is created.

### Authorization

```c
typedef int (*AuthorizeGRPCRequestFunction)(struct GRPCDispatch* dispatch,
                                            const ProtobufCMethodDescriptor* descriptor,
                                            h2o_req_t* request);
```

Called once per accepted request, after the method is resolved and before the
invocation exists. **Return `0` to allow.** A gRPC status from `1` to `16` is used
directly as the status of the rejection, so returning `GRPC_STATUS_UNAUTHENTICATED` or
`GRPC_STATUS_PERMISSION_DENIED` produces the right trailer. The value indexes a table of
status strings without a bounds check: any other value is undefined behaviour. The full `h2o_req_t` is
available, so the `authorization` header can be inspected with the header helpers from
[H2OCore.md](H2OCore.md).

## Invocation Lifecycle and Replies

```c
void HoldGRPCInvocation(struct GRPCInvocation* invocation);
void ReleaseGRPCInvocation(struct GRPCInvocation* invocation);

int TransmitGRPCPing(struct GRPCInvocation* invocation);
int TransmitGRPCReply(struct GRPCInvocation* invocation, const ProtobufCMessage* message, uint8_t flags);
int TransmitGRPCStatus(struct GRPCInvocation* invocation, int status, const char* message);
```

The invocation is reference counted, starting at `1` for the request itself, and is
also tied to the H2O request pool so that a connection dropped by the peer disposes of
it. `invocation->closure` is free for the application.

**Take a reference whenever the answer is not produced inline.** A handler that parks
the invocation — waiting on a database, another RPC, a device — must call
`HoldGRPCInvocation()` before returning and `ReleaseGRPCInvocation()` when it is
finally done; otherwise the invocation can be destroyed while the deferred work still
points at it.

Replies:

- `TransmitGRPCReply()` packs and queues one response message. `flags` are gRPC frame
  flags; a reply is compressed only when `flags` has `GRPC_FLAG_COMPRESSED` and
  `GRPC_IV_FLAG_OUTPUT_GZIP` is set. Call it more than once for a server stream. Returns
  `0`, `-ENOMEM`, `-EIO` (deflate failed) or `-EINVAL`.
- `TransmitGRPCStatus()` ends the call with a gRPC status (`0` to `16`, otherwise
  `-EINVAL`) and optional message, which is percent-encoded into the request pool. It sets
  `GRPC_IV_FLAG_IGNORING`; replies or another status queued afterwards are not sent. See
  `GRPC_IV_FLAG_SENDING` above for whether the status lands in headers or trailers.
- `TransmitGRPCPing()` queues a gRPC message frame of length zero. The peer sees one more
  message with every field at its default, not an HTTP/2 PING; use it only where the
  protocol tolerates that.

Replies are queued per invocation and handed to H2O one at a time; the queue is unbounded.
All three return `-EINVAL` once `GRPC_IV_FLAG_ACTIVE` is clear, so answering an invocation
whose peer already disconnected is safe rather than fatal.

## Rules

- Handlers run on the H2O loop thread — the ring thread when the loop is a `FastUVLoop` on
  the ring; do not block them. Nothing in the module is locked.
- Every accepted call must eventually reach `TransmitGRPCStatus()`, otherwise the
  client waits for trailers that never come.
- Do not retain the `h2o_req_t*` beyond the invocation — it belongs to H2O and dies
  with the request pool.

## Lifecycle

`gRPCServer` creates nothing on the ring; it runs inside H2O callbacks on the H2O loop.
`HandleGRPCDispatchRequest()` allocates a `GRPCInvocation` for every accepted request and ties
it to the request pool, whose disposal holds the first reference. When H2O disposes of the
request — after the final reply, a stream reset or a closed connection — it clears
`GRPC_IV_FLAG_ACTIVE`, delivers `GRPC_IV_REASON_RELEASED` and drops that reference; the last
`ReleaseGRPCInvocation()` frees the queued replies, the buffers and the invocation. An
invocation kept alive by `HoldGRPCInvocation()` after that is only a pointer.

Invocations end only through H2O request disposal. Shut down the `H2OCore` and drain its loop
before the ring (see [H2OCore.md](H2OCore.md#lifecycle)).
