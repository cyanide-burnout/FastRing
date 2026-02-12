# gRPCServer API Reference

Header: `Supplimentary/gRPCServer.h`

`gRPCServer` provides server-side gRPC dispatch on top of `H2OCore` and protobuf-c.

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
- `GRPC_IV_REASON_CREATED`
- `GRPC_IV_REASON_RECEIVED`
- `GRPC_IV_REASON_FINISHED`
- `GRPC_IV_REASON_FAILED`
- `GRPC_IV_REASON_RELEASED`

## Invocation Lifecycle and Replies

```c
void HoldGRPCInvocation(struct GRPCInvocation* invocation);
void ReleaseGRPCInvocation(struct GRPCInvocation* invocation);

int TransmitGRPCPing(struct GRPCInvocation* invocation);
int TransmitGRPCReply(struct GRPCInvocation* invocation, const ProtobufCMessage* message, uint8_t flags);
int TransmitGRPCStatus(struct GRPCInvocation* invocation, int status, const char* message);
```

Helper macro:

```c
#define GetGRPCServicePath(service) ...
```

