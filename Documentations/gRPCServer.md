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
- `GRPC_IV_REASON_RECEIVED` (1) - `data`/`length` carry one decoded request message
- `GRPC_IV_REASON_FINISHED` (2) - the client half-closed, no further messages
- `GRPC_IV_REASON_FAILED` (3) - the stream failed or was reset
- `GRPC_IV_REASON_RELEASED` (4) - the invocation is being destroyed, release
  module state here

## Invocation Flags

`invocation->flags` reports the state of the stream:

- `GRPC_IV_FLAG_ACTIVE` (`1 << 0`) - the stream is live; cleared before
  `GRPC_IV_REASON_RELEASED`. `TransmitGRPCPing()`, `TransmitGRPCReply()` and
  `TransmitGRPCStatus()` are no-ops once it is clear
- `GRPC_IV_FLAG_SENDING` (`1 << 1`) - a reply body has already been queued, so the
  gRPC status is emitted as HTTP trailers rather than headers
- `GRPC_IV_FLAG_IGNORING` (`1 << 2`) - inbound data is being discarded (the call was
  already answered or rejected)
- `GRPC_IV_FLAG_INPUT_GZIP` (`1 << 3`) - the peer sends gzip-compressed messages
- `GRPC_IV_FLAG_OUTPUT_GZIP` (`1 << 4`) - replies are gzip-compressed

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

Before any invocation is created, the dispatch rejects anything that is not gRPC: the
method must be `POST`, `content-type` must be `application/grpc`, and `te` must be
`trailers`. The path is split at the last `/` into service and method, and the method
is resolved against the descriptor. An unknown method is answered with
`grpc-status: GRPC_STATUS_UNIMPLEMENTED` and no invocation is created.

### Authorization

```c
typedef int (*AuthorizeGRPCRequestFunction)(struct GRPCDispatch* dispatch,
                                            const ProtobufCMethodDescriptor* descriptor,
                                            h2o_req_t* request);
```

Called once per accepted request, after the method is resolved and before the
invocation exists. **Return `0` to allow.** Any non-zero return is used directly as
the gRPC status of the rejection, so returning `GRPC_STATUS_UNAUTHENTICATED` or
`GRPC_STATUS_PERMISSION_DENIED` produces the right trailer. The full `h2o_req_t` is
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
  flags; compression is applied according to `GRPC_IV_FLAG_OUTPUT_GZIP`. Call it more
  than once for a server stream.
- `TransmitGRPCStatus()` ends the call with a gRPC status and optional message. After
  it, the stream is finished — see `GRPC_IV_FLAG_SENDING` above for whether the status
  lands in headers or trailers.
- `TransmitGRPCPing()` emits an empty frame, useful to keep a long-lived stream alive.

All three are no-ops once `GRPC_IV_FLAG_ACTIVE` is clear, so answering an invocation
whose peer already disconnected is safe rather than fatal.

## Rules

- Handlers run in the ring thread through `H2OCore`; do not block them.
- Every accepted call must eventually reach `TransmitGRPCStatus()`, otherwise the
  client waits for trailers that never come.
- Do not retain the `h2o_req_t*` beyond the invocation — it belongs to H2O and dies
  with the request pool.

