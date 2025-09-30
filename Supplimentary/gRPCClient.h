#ifndef GRPCCLIENT_H
#define GRPCCLIENT_H

#include <protobuf-c/protobuf-c.h>

#include "Fetch.h"
#include "gRPC.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Transport

#define GRPCCLIENT_REASON_FRAME   0
#define GRPCCLIENT_REASON_STATUS  1

struct GRPCFrame;
struct GRPCTransmission;

typedef int (*HandleGRPCEventFunction)(void* closure, struct GRPCTransmission* transmission, int reason, int parameter, char* data, size_t length);

struct GRPCMethod
{
  CURLU* location;             //
  struct curl_slist* headers;  // HTTP headers

  long type;                   // CURLOPT_HTTP_VERSION
  long timeout;                //
  char resolution;             // 'S' for seconds, 'm' for milliseconds, 0 for undefined

  int count;                   // Reference count
};

struct GRPCFrame
{
  struct GRPCTransmission* transmission;  //
  struct GRPCFrame* next;                 //
  size_t size;                            // Size of allocation

  char* data;                             // Pointer to data (set to buffer by default, use NULL to indicate end of stream)
  size_t length;                          // Length of data

  char buffer[0];                         //
};

struct GRPCBuffer
{
  uint8_t* buffer;
  size_t length;
  size_t size; 
};

struct GRPCTransmission
{
  struct FetchTransmission super;

  HandleGRPCEventFunction function;
  void* closure;

  int status;                 // gRPC status
  char* message;              //

  struct GRPCBuffer inbound;  // Inbound
  struct GRPCBuffer scratch;  //

  struct GRPCFrame* heap;     //
  struct GRPCFrame* head;     // Outbound
  struct GRPCFrame* tail;     //
};

struct GRPCMethod* CreateGRPCMethod(const char* location, const char* package, const char* service, const char* name, const char* token, long timeout, char resolution);
void ReleaseGRPCMethod(struct GRPCMethod* method);
void HoldGRPCMethod(struct GRPCMethod* method);

struct GRPCTransmission* MakeGRPCTransmission(struct Fetch* fetch, struct GRPCMethod* method, HandleGRPCEventFunction function, void* closure);
void CancelGRPCTransmission(struct GRPCTransmission* transmission);

struct GRPCFrame* AllocateGRPCFrame(struct GRPCTransmission* transmission, size_t length);
void TransmitGRPCFrame(struct GRPCFrame* frame);

int TransmitGRPCMessage(struct GRPCTransmission* transmission, const ProtobufCMessage* message, int final);

// Service

struct GRPCService;

typedef void (*HandleGRPCErrorFunction)(void* closure, struct GRPCService* service, const char* method, int status, const char* message);

struct GRPCService
{
  ProtobufCService super;

  struct Fetch* fetch;
  CURLU* location;

  long type;
  long timeout;
  char resolution;
  struct curl_slist* headers;

  int count;
  void* closure;
  HandleGRPCErrorFunction function;

  struct GRPCMethod methods[0];
};

struct GRPCCall
{
  struct GRPCTransmission super;

  const char* method;
  const ProtobufCMessageDescriptor* descriptor;

  ProtobufCClosure function;
  void* closure;
};

ProtobufCService* CreateGRPCService(struct Fetch* fetch, const ProtobufCServiceDescriptor* descriptor, const char* location, const char* token, long timeout, char resolution, HandleGRPCErrorFunction function, void* closure);

#ifdef __cplusplus
}
#endif

#endif
