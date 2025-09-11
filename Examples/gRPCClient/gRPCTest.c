#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "ProtoBuf.h"
#include "gRPCClient.h"
#include "gRPCTest.pb-c.h"

#define STATE_RUNNING  -1

atomic_int state = { STATE_RUNNING };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 0, memory_order_relaxed);
}

int HandleEvent(void* closure, struct FetchTransmission* transmission, int reason, int parameter, char* data, size_t length)
{
  ProtobufCAllocator* arena;
  struct Demo__EchoReply* reply;

  switch (reason)
  {
    case GRPCCLIENT_REASON_FRAME:
      arena = CreateProtoBufArena(length + 1024);
      reply = demo__echo_reply__unpack(arena, length, (uint8_t*)data);
      printf("Message: %s\n", reply->text);
      demo__echo_reply__free_unpacked(reply, arena);
      break;

    case GRPCCLIENT_REASON_STATUS:
      printf("Status: %d / %s\n", parameter, data);
      atomic_store_explicit(&state, 0, memory_order_relaxed);
      *(void**)closure = NULL;
      break;
  }

  return 0;
}

int main()
{
  struct sigaction action;
  struct FastRing* ring;
  struct Fetch* fetch;
  struct GRPCFrame* frame;
  struct GRPCMethod* method;
  struct Demo__EchoRequest request;
  struct FetchTransmission* transmission;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  printf("Started\n");

  ring         = CreateFastRing(0);
  fetch        = CreateFetch(ring);
  method       = CreateGRPCMethod("http://localhost:50051", "demo", "Echoer", "UnaryEcho", NULL, 0, 0);
  transmission = MakeGRPCCall(fetch, method, HandleEvent, &transmission);

  demo__echo_request__init(&request);
  request.text  = (char*)"Hello World!";
  frame         = AllocateGRPCFrame(transmission, demo__echo_request__get_packed_size(&request));
  frame->length = demo__echo_request__pack(&request, frame->data);
  TransmitGRPCFrame(frame);

  frame       = AllocateGRPCFrame(transmission, 0);
  frame->data = NULL;  // End of stream
  TransmitGRPCFrame(frame);

  while ((atomic_load_explicit(&state, memory_order_relaxed) == STATE_RUNNING) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));


  CancelFetchTransmission(transmission);
  ReleaseGRPCMethod(method);
  ReleaseFetch(fetch);
  ReleaseFastRing(ring);

  printf("Stopped\n");

  return 0;
}
