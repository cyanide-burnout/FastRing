#ifndef FASTSOCKET_H
#define FASTSOCKET_H

#include "FastRing.h"
#include "FastBuffer.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FASTSOCKET_MODE_REGULAR    0
#define FASTSOCKET_MODE_ZERO_COPY  MSG_ZEROCOPY
#define FASTSOCKET_MODE_AUTO_CORK  MSG_MORE

#if (IO_URING_VERSION_MAJOR > 2) || (IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR >= 6)
#define FASTSOCKET_MODE_FILE_IO    MSG_DONTROUTE
#endif

struct FastSocket;

typedef void (*HandleFastSocketEvent)(struct FastSocket* socket, int event, int parameter);

struct FastSocketInboundQueue
{
  struct FastRingBufferProvider* provider;
  struct FastRingDescriptor* descriptor;
  struct FastBufferPool* pool;
  struct FastBuffer* head;
  struct FastBuffer* tail;
  size_t position;
  size_t length;
};

struct FastSocketOutboundBatch
{
  struct FastSocketOutboundBatch* next;
  struct FastRingDescriptor* head;
  struct FastRingDescriptor* tail;
  size_t count;
};

struct FastSocketOutboundQueue
{
  struct FastBufferPool* pool;
  struct FastSocketOutboundBatch* stack;
  struct FastSocketOutboundBatch* head;
  struct FastSocketOutboundBatch* tail;
  uint32_t condition;
  uint32_t limit;
  int mode;
};

struct FastSocket
{
  int handle;
  int options;
  struct FastRing* ring;
  struct FastSocketInboundQueue inbound;
  struct FastSocketOutboundQueue outbound;
  HandleFastSocketEvent function;
  void* closure;
  int count;
};

struct FastSocket* CreateFastSocket(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, struct msghdr* message, int flags, int mode, uint32_t limit, HandleFastSocketEvent function, void* closure);
ssize_t ReceiveFastSocketData(struct FastSocket* socket, void* data, size_t size, int flags);
int TransmitFastSocketDescriptor(struct FastSocket* socket, struct FastRingDescriptor* descriptor, struct FastBuffer* buffer);
int TransmitFastSocketMessage(struct FastSocket* socket, struct msghdr* message, int flags);
int TransmitFastSocketData(struct FastSocket* socket, struct sockaddr* address, socklen_t length, const void* data, size_t size, int flags);
void ReleaseFastSocket(struct FastSocket* socket);

inline __attribute__((always_inline)) struct msghdr* GetFastSocketMessageHeader(struct FastSocket* socket)
{
  return ((socket != NULL) &&
          (socket->inbound.descriptor != NULL) &&
          (socket->inbound.descriptor->submission.opcode == IORING_OP_RECVMSG)) ?
         &socket->inbound.descriptor->data.socket.message :
         NULL;
}

inline __attribute__((always_inline)) struct FastBuffer* ReceiveFastSocketBuffer(struct FastSocket* socket)
{
  struct FastBuffer* buffer;

  if ((socket != NULL) &&
      (buffer  = socket->inbound.tail))
  {
    socket->inbound.tail      = buffer->next;
    socket->inbound.length   -= buffer->length;
    socket->inbound.position  = 0;
    return buffer;
  }

  return NULL;
}

#ifdef __cplusplus
}
#endif

#endif
