#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "ProtoBuf.h"
#include "FastUVLoop.h"
#include "gRPCServer.h"
#include "gRPCTest.pb-c.h"

atomic_int state = { 0 };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 1, memory_order_relaxed);
}

static int HandlePageRequest(h2o_handler_t* handler, h2o_req_t* request)
{
  if (h2o_memis(request->method.base, request->method.len, H2O_STRLIT("GET")))
  {
    request->res.status = 200;
    request->res.reason = "OK";

    h2o_add_header(&request->pool, &request->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
    h2o_send_inline(request, H2O_STRLIT("Test\n"));

    return 0;
  }

  return -1;
}

static int AuthorizeGRPCRequest(struct GRPCDispatch* dispatch, const ProtobufCMethodDescriptor* descriptor, h2o_req_t* request)
{
  const char* authorization;
  size_t length;

  if (authorization = GetH2OHeaderByIndex(&request->headers, H2O_TOKEN_AUTHORIZATION, &length))
  {
    printf("Authorization: %.*s\n", (int)length, authorization);
    return GRPC_STATUS_OK;
  }

  return GRPC_STATUS_PERMISSION_DENIED;
}

static void HandleGRPCRequest(struct GRPCInvocation* invocation, int reason, uint8_t* data, size_t length)
{
  ProtobufCAllocator* arena;
  Demo__EchoRequest* request;
  Demo__EchoReply reply;
  char buffer[2048];

  switch (reason)
  {
    case GRPC_IV_REASON_CREATED:
      printf("New requset (HTTP version %x)\n", invocation->request->version);
      break;

    case GRPC_IV_REASON_RECEIVED:
      arena   = CreateProtoBufArena(4096);
      request = demo__echo_request__unpack(arena, length, data);
      sprintf(buffer, "%s: %s", invocation->descriptor->name, request->text);
      demo__echo_request__free_unpacked(request, arena);

      printf("Prepared response: %s\n", buffer);

      demo__echo_reply__init(&reply);
      reply.text = buffer;
      TransmitGRPCReply(invocation, (ProtobufCMessage*)&reply, GRPC_FLAG_COMPRESSED);
      break;

    case GRPC_IV_REASON_FINISHED:
      TransmitGRPCStatus(invocation, GRPC_STATUS_OK, "Success");
      break;
  }
}

int main()
{
  struct sigaction action;

  struct FastRing* ring;
  struct FastUVLoop* loop;
  struct H2OCore* core;

  struct sockaddr_in address;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  uv_ip4_addr("0.0.0.0", 8080, &address);

  struct GRPCDispatch dispatch =
  {
    &demo__echoer__descriptor,  // ProtoBuf's service descriptor
    AuthorizeGRPCRequest,       // Optional
    HandleGRPCRequest,          // Mandatory
    NULL                        // Closure context
  };

  struct H2ORoute routes[] =
  {
    { "/",             0, 0,                              HandlePageRequest,         NULL      },
    { "/demo.Echoer/", 0, H2OCORE_ROUTE_OPTION_STREAMING, HandleGRPCDispatchRequest, &dispatch },
    { 0 }
  };

  printf("Started\n");

  ring = CreateFastRing(0);
  loop = CreateFastUVLoop(ring, 200);
  core = CreateH2OCore(loop->loop, (struct sockaddr*)&address, NULL, NULL, routes, 0);

  while ((atomic_load_explicit(&state, memory_order_relaxed) < 1) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  StopH2OCore(core);

  while ((atomic_load_explicit(&state, memory_order_relaxed) < 2) &&
         (uv_loop_alive(loop->loop)))
  {
    // Finalize all handlers first
    uv_run(loop->loop, UV_RUN_NOWAIT);
  }

  ReleaseH2OCore(core);
  ReleaseFastUVLoop(loop);
  ReleaseFastRing(ring);

  printf("Stopped\n");

  return 0;
}
