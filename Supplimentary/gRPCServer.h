#ifndef GRPCSERVER_H
#define GRPCSERVER_H

#include <protobuf-c/protobuf-c.h>

#include "H2OCore.h"
#include "gRPC.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define GRPC_IV_FLAG_ACTIVE        (1 << 0)
#define GRPC_IV_FLAG_SENDING       (1 << 1)
#define GRPC_IV_FLAG_IGNORING      (1 << 2)
#define GRPC_IV_FLAG_INPUT_GZIP    (1 << 3)
#define GRPC_IV_FLAG_OUTPUT_GZIP   (1 << 4)

#define GRPC_IV_REASON_CREATED   0
#define GRPC_IV_REASON_RECEIVED  1
#define GRPC_IV_REASON_FINISHED  2
#define GRPC_IV_REASON_FAILED    3
#define GRPC_IV_REASON_RELEASED  4

struct GRPCDispatch;
struct GRPCInvocation;

typedef int (*AuthorizeGRPCRequestFunction)(struct GRPCDispatch* dispatch, const ProtobufCMethodDescriptor* descriptor, h2o_req_t* request);
typedef void (*HandleGRPCRequestFunction)(struct GRPCInvocation* invocation, int reason, uint8_t* data, size_t length);

#ifdef GRPCSERVER_INTERNAL

struct GRPCBuffer
{
  uint8_t* buffer;
  size_t length;
  size_t size; 
};

struct GRPCReply
{
  struct GRPCReply* next;
  h2o_send_state_t state;
  h2o_iovec_t vector;
  size_t size;
  uint8_t data[0];
};

#endif

struct GRPCInvocation
{
  h2o_generator_t generator;
  h2o_req_t* request;
  void* closure;

  int count;
  uint32_t flags;
  struct GRPCDispatch* dispatch;
  const ProtobufCMethodDescriptor* descriptor;

  int status;
  h2o_iovec_t message;

#ifdef GRPCSERVER_INTERNAL
  struct GRPCReply* pool;
  struct GRPCReply* head;
  struct GRPCReply* tail;
  struct GRPCReply* flight;

  struct GRPCBuffer inbound;
  struct GRPCBuffer scratch;
#endif
};

struct GRPCDispatch
{
  const ProtobufCServiceDescriptor* descriptor;
  AuthorizeGRPCRequestFunction authorize;
  HandleGRPCRequestFunction handle;
  void* closure;
};

int HandleGRPCDispatchRequest(h2o_handler_t* handler, h2o_req_t* request);

void HoldGRPCInvocation(struct GRPCInvocation* invocation);
void ReleaseGRPCInvocation(struct GRPCInvocation* invocation);

int TransmitGRPCReply(struct GRPCInvocation* invocation, const ProtobufCMessage* message, uint8_t flags, h2o_send_state_t state);
void TransmitGRPCError(struct GRPCInvocation* invocation, int status, const char* message);

#ifdef __cplusplus
}
#endif

#endif
