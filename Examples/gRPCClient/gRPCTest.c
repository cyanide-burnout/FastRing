#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "ProtoBuf.h"
#include "gRPCClient.h"
#include "gRPCTest.pb-c.h"

atomic_int state = { 0 };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 0, memory_order_relaxed);
}

// Example 1 - Direct gRP C handling

int HandleEvent(void* closure, struct FetchTransmission* transmission, int reason, int parameter, char* data, size_t length)
{
  ProtobufCAllocator* arena;
  struct Demo__EchoReply* reply;

  switch (reason)
  {
    case GRPCCLIENT_REASON_FRAME:
      arena = CreateProtoBufArena(length + 1024);
      reply = demo__echo_reply__unpack(arena, length, (uint8_t*)data);
      printf("Example 1 - message: %s\n", reply->text);
      demo__echo_reply__free_unpacked(reply, arena);
      break;

    case GRPCCLIENT_REASON_STATUS:
      printf("Example 1 - status: %d / %s\n", parameter, data);
      atomic_fetch_add_explicit(&state, 1, memory_order_relaxed);
      *(void**)closure = NULL;
      break;
  }

  return 0;
}

// Example 2 - Using protobuf-c's service

void HandleError(void* closure, ProtobufCService* service, const char* method, int status, const char* message)
{
  printf("Example 2 - status: %d / %s\n", status, message);
}

void HandleEchoReply(const Demo__EchoReply* reply, void* data)
{
  atomic_fetch_add_explicit(&state, 1, memory_order_relaxed);

  if (reply == NULL)
  {
    printf("Example 2 - an error occured\n");
    return;
  }

  printf("Example 2 - message: %s\n", reply->text);
}

int main()
{
  struct sigaction action;
  struct FastRing* ring;
  struct Fetch* fetch;

  struct Demo__EchoRequest request = DEMO__ECHO_REQUEST__INIT;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  printf("Started\n");

  ring  = CreateFastRing(0);
  fetch = CreateFetch(ring);

  request.text = (char*)"Hello World!";

  // Example 1

  struct GRPCMethod* method              = CreateGRPCMethod("http://localhost:50051", "demo", "Echoer", "UnaryEcho", NULL, 0, 0);
  struct FetchTransmission* transmission = MakeGRPCCall(fetch, method, HandleEvent, &transmission);
  TransmitGRPCMessage(transmission, (ProtobufCMessage*)&request, 1);

  // Example 2

  ProtobufCService* service = CreateGRPCService(fetch, &demo__echoer__descriptor, "http://localhost:50051", NULL, 0, 0, HandleError, NULL);
  demo__echoer__unary_echo(service, &request, HandleEchoReply, NULL);

  //

  while ((atomic_load_explicit(&state, memory_order_relaxed) < 2) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  // Example 1

  CancelFetchTransmission(transmission);
  ReleaseGRPCMethod(method);

  // Example 2

  protobuf_c_service_destroy(service);

  //

  ReleaseFetch(fetch);
  ReleaseFastRing(ring);

  printf("Stopped\n");

  return 0;
}
