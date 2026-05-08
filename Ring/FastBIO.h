#ifndef FASTBIO_H
#define FASTBIO_H

#include "FastRing.h"
#include "FastBuffer.h"

#include <openssl/bio.h>
#include <openssl/ssl3.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FASTBIO_BUFFER_SIZE  ((sizeof(struct io_uring_recvmsg_out) + CMSG_SPACE(sizeof(uint8_t)) + SSL3_RT_HEADER_LENGTH + SSL3_RT_MAX_PLAIN_LENGTH + __BIGGEST_ALIGNMENT__ - 1) & ~(__BIGGEST_ALIGNMENT__ - 1))

#define FASTBIO_CTRL_ENSURE  98
#define FASTBIO_CTRL_TOUCH   99

#define FASTBIO_FLAG_KTLS_AVAILABLE      (1U << 0)
#define FASTBIO_FLAG_KTLS_RECEIVE        (1U << 1)
#define FASTBIO_FLAG_KTLS_SEND           (1U << 2)
#define FASTBIO_FLAG_KTLS_ONCE           (1U << 3)
#define FASTBIO_FLAG_POLL_PROGRESS       (1U << 4)


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
  struct FastRingDescriptor* descriptor;
  struct FastBIOOutboundQueue outbound;
  struct FastBIOInboundQueue inbound;
  HandleFastBIOEvent function;
  uint32_t flags;
  void* closure;
  size_t count;
  int handle;
  int type;
};

BIO* CreateFastBIO(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, uint64_t options, uint32_t granularity, uint32_t limit, HandleFastBIOEvent function, void* closure);

#ifdef __cplusplus
}
#endif

#endif
