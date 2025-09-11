#ifndef GRPCCLIENT_H
#define GRPCCLIENT_H

#include <protobuf-c/protobuf-c.h>

#include "Fetch.h"
#include "gRPC.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define GRPCCLIENT_REASON_FRAME   0
#define GRPCCLIENT_REASON_STATUS  1

typedef int (*HandleGRPCEventFunction)(void* closure, struct FetchTransmission* transmission, int reason, int parameter, char* data, size_t length);

struct GRPCMethod
{
  CURLU* location;
  struct curl_slist* headers;

  long type;
  long timeout;
  char resolution;
};

struct GRPCFrame
{
  struct FetchTransmission* transmission;  //
  struct GRPCFrame* next;                  //
  size_t size;                             // Size of allocation

  char* data;                              // Pointer to data (will be set to buffer by default, use NULL to indicate end of stream)
  size_t length;                           // Length of data

  char buffer[0];                          //
};

struct GRPCMethod* CreateGRPCMethod(const char* location, const char* package, const char* service, const char* name, const char* token, long timeout, char resolution);
void ReleaseGRPCMethod(struct GRPCMethod* method);

struct FetchTransmission* MakeGRPCCall(struct Fetch* fetch, struct GRPCMethod* method, HandleGRPCEventFunction function, void* closure);

struct GRPCFrame* AllocateGRPCFrame(struct FetchTransmission* transmission, size_t length);
void TransmitGRPCFrame(struct GRPCFrame* frame);

int TransmitGRPCMessage(struct FetchTransmission* transmission, const ProtobufCMessage* message, int final);

#ifdef __cplusplus
}
#endif

#endif