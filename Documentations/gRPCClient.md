# gRPCClient API Reference

Header: `Supplimentary/gRPCClient.h`

`gRPCClient` provides client-side gRPC transport and protobuf-c service wrapper on top of `Fetch`.

## Transport Layer API

```c
typedef int (*HandleGRPCEventFunction)(
  void* closure,
  struct GRPCTransmission* transmission,
  int reason,
  int parameter,
  char* data,
  size_t length);

struct GRPCMethod* CreateGRPCMethod(
  const char* location,
  const char* package,
  const char* service,
  const char* name,
  const char* token,
  long timeout,
  char resolution);

void ReleaseGRPCMethod(struct GRPCMethod* method);
void HoldGRPCMethod(struct GRPCMethod* method);

struct GRPCTransmission* MakeGRPCTransmission(
  struct Fetch* fetch,
  struct GRPCMethod* method,
  HandleGRPCEventFunction function,
  void* closure);

void CancelGRPCTransmission(struct GRPCTransmission* transmission);

struct GRPCFrame* AllocateGRPCFrame(struct GRPCTransmission* transmission, size_t length);
void TransmitGRPCFrame(struct GRPCFrame* frame);
int TransmitGRPCMessage(struct GRPCTransmission* transmission, const ProtobufCMessage* message, int final);
```

Reasons:
- `GRPCCLIENT_REASON_FRAME`
- `GRPCCLIENT_REASON_STATUS`

## ProtobufC Service Wrapper

```c
typedef void (*HandleGRPCErrorFunction)(
  void* closure,
  struct GRPCService* service,
  const char* method,
  int status,
  const char* message);

ProtobufCService* CreateGRPCService(
  struct Fetch* fetch,
  const ProtobufCServiceDescriptor* descriptor,
  const char* location,
  const char* token,
  long timeout,
  char resolution,
  HandleGRPCErrorFunction function,
  void* closure);
```

