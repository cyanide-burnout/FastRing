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

#define FASTBIO_CTRL_TOUCH  99

#define FASTBIO_FLAG_KTLS_AVAILABLE      (1U << 0)
#define FASTBIO_FLAG_KTLS_SEND           (1U << 1)
#define FASTBIO_FLAG_KTLS_RECEIVE        (1U << 2)
#define FASTBIO_FLAG_KTLS_FORMAT         (1U << 3)
#define FASTBIO_FLAG_KTLS_BIDIRECTIONAL  (1U << 4)
#define FASTBIO_FLAG_KTLS_IGNORE         (1U << 5)
#define FASTBIO_FLAG_KTLS_ONCE           (1U << 6)

/*
  FASTBIO_FLAG_KTLS_BIDIRECTIONAL: kTLS RX support is intentionally not enabled by default.

  At the time this code was written, the OpenSSL 3.x record layer exposes kTLS RX as an internal record-method/backend switch. Once BIO_CTRL_SET_KTLS for
  the receive direction succeeds, OpenSSL expects the BIO to immediately behave like a kTLS-capable receive transport and to return kTLS-compatible records.

  This is not a good fit for a fully asynchronous BIO implementation. FastBIO may already have encrypted records prefetched in its userspace inbound queue
  when OpenSSL requests TLS_RX. OpenSSL 3.x does not provide a documented public API for a "pending kTLS RX" state, nor a way to drain already-prefetched data
  through the old userspace TLS path and switch the RX record backend later.

  FASTBIO_FLAG_KTLS_IGNORE controls that transition gap. If it is set, FastBIO drops already-prefetched pre-kTLS receive buffers while waiting for the
  asynchronous TLS_RX switch marker. If it is not set, such buffers are treated as a receive-phase violation and BIO_read() fails with EPROTO, forcing the
  upper TLS socket to tear the connection down instead of silently crossing mixed RX phases.

  For this reason, bidirectional kTLS is considered experimental. By default FastBIO enables only the stable TX-oriented kTLS path. Callers that want to test
  RX kTLS must set FASTBIO_FLAG_KTLS_BIDIRECTIONAL before the connection starts moving application data.
*/

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
  uint32_t types[2];
  uint32_t flags;
  void* closure;
  size_t count;
  int handle;
};

BIO* CreateFastBIO(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, uint64_t options, uint32_t granularity, uint32_t limit, HandleFastBIOEvent function, void* closure);

#ifdef __cplusplus
}
#endif

#endif
