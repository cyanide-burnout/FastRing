#ifndef FASTBIO_H
#define FASTBIO_H

#include "FastRing.h"
#include "FastBuffer.h"

#include <openssl/bio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FASTBIO_CTRL_TOUCH  99

struct FastBIO;

typedef void (*HandleFastBIOEvent)(struct FastBIO* engine, int event, int parameter);

struct FastBIOInboundQueue
{
  struct FastRingBufferProvider* provider;
  struct FastRingDescriptor* descriptor;
  struct FastBufferPool* pool;
  struct FastBuffer* head;
  struct FastBuffer* tail;
  uint32_t condition;
  size_t position;
  size_t length;
};

struct FastBIOOutboundQueue
{
  struct FastBufferPool* pool;
  struct FastRingDescriptor* head;
  struct FastRingDescriptor* tail;
  uint32_t granularity;
  uint32_t condition;
  uint32_t limit;
  size_t count;
};

struct FastBIO
{
  struct FastRing* ring;
  struct FastBIOOutboundQueue outbound;
  struct FastBIOInboundQueue inbound;
  HandleFastBIOEvent function;
  void* closure;
  size_t count;
  int handle;
};

BIO* CreateFastBIO(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, uint32_t granularity, uint32_t limit, HandleFastBIOEvent function, void* closure);

#ifdef __cplusplus
}
#endif

#endif
