#include "FastSocket.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define likely(condition)     __builtin_expect(!!(condition), 1)
#define unlikely(condition)   __builtin_expect(!!(condition), 0)

static void FreeSocketInstance(struct FastSocket* socket, int reason)
{
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;
  struct FastSocketOutboundBatch* batch;

  while (buffer = socket->inbound.tail)
  {
    socket->inbound.tail = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  while (batch = socket->outbound.stack)
  {
    socket->outbound.stack = batch->next;
    free(batch);
  }

  if (((reason == RING_REASON_COMPLETE) ||
       (reason == RING_REASON_INCOMPLETE)) &&
      (descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL)))
  {
    io_uring_prep_close(&descriptor->submission, socket->handle);
    SubmitFastRingDescriptor(descriptor, 0);
  }
  else
  {
    // FastRing might be under destruction, close socket synchronously
    close(socket->handle);
  }

  free(socket);
}

static inline void __attribute__((always_inline)) ReleaseSocketInstance(struct FastSocket* socket, int reason)
{
  socket->count --;

  if (unlikely(socket->count == 0))
  {
    // Prevent inlining less used code
    FreeSocketInstance(socket, reason);
  }
}

static inline void __attribute__((always_inline)) CallHandlerFunction(struct FastSocket* socket, int event, int parameter)
{
  if (likely(socket->function != NULL))
  {
    // Handler can be freed earlier then socket
    socket->function(socket, event, parameter);
  }
}

static inline void __attribute__((always_inline)) ReleaseOutboundBatch(struct FastSocketOutboundQueue* queue, struct FastSocketOutboundBatch* batch)
{
  batch->count = 0;
  batch->head  = NULL;
  batch->tail  = NULL;
  batch->next  = queue->stack;
  queue->stack = batch;
}

static inline struct FastSocketOutboundBatch* __attribute__((always_inline)) AllocateOutboundBatch(struct FastSocketOutboundQueue* queue)
{
  struct FastSocketOutboundBatch* batch;

  if (likely(batch = queue->stack))
  {
    queue->stack = batch->next;
    batch->next  = NULL;
    goto AppendQueue;
  }

  if (likely(batch = (struct FastSocketOutboundBatch*)calloc(1, sizeof(struct FastSocketOutboundBatch))))
  {
    AppendQueue:

    if (likely(queue->tail != NULL))
    {
      queue->head->next = batch;
      queue->head       = batch;
    }
    else
    {
      queue->tail = batch;
      queue->head = batch;
    }
  }

  return batch;
}

static int HandleInboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  uint8_t* data;
  struct FastSocket* socket;
  struct FastBuffer* buffer;

  socket = (struct FastSocket*)descriptor->closure;

  if (unlikely(completion == NULL))
  {
    socket->inbound.descriptor = NULL;
    CallHandlerFunction(socket, POLLHUP, 0);
    ReleaseSocketInstance(socket, reason);
    return 0;
  }

  if (unlikely(completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    // That's required to solve a possible race condition when proceed io_uring_prep_cancel()
    return 0;
  }

  if (unlikely(completion->res < 0))
  {
    AdvanceFastRingBuffer(socket->inbound.provider, completion, NULL, NULL);

    if ((completion->res != -ENOBUFS) &&
        (completion->res != -ECANCELED))
    {
      socket->outbound.condition |= POLLERR;

      if (completion->flags & IORING_CQE_F_MORE)
      {
        CallHandlerFunction(socket, POLLERR, -completion->res);
        return 1;
      }

      socket->inbound.descriptor = NULL;
      CallHandlerFunction(socket, POLLHUP, -completion->res);
      ReleaseSocketInstance(socket, reason);
      return 0;
    }
  }

  if (likely((completion->res >= 0) &&
             (data = GetFastRingBuffer(socket->inbound.provider, completion))))
  {
    AdvanceFastRingBuffer(socket->inbound.provider, completion, AllocateRingFastBuffer, socket->inbound.pool);

    buffer                  = FAST_BUFFER(data);
    buffer->length          = completion->res;
    socket->inbound.length += completion->res;

    if (unlikely(socket->inbound.tail == NULL))
    {
      socket->inbound.tail = buffer;
      socket->inbound.head = buffer;
    }
    else
    {
      socket->inbound.head->next = buffer;
      socket->inbound.head       = buffer;
    }

    CallHandlerFunction(socket, POLLIN, socket->inbound.length);
  }

  if (unlikely(~completion->flags & IORING_CQE_F_MORE))
  {
    if (socket->inbound.descriptor == NULL)
    {
      // Socket could be closed by function()
      // at the same time with receive last packet
      ReleaseSocketInstance(socket, reason);
      return 0;
    }

    // Eventually URing may release submission
    // Also this handles -ENOBUFS and -ECANCELED
    SubmitFastRingDescriptor(socket->inbound.descriptor, 0);
  }

  return 1;
}

static int HandleOutboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastSocketOutboundBatch* batch;
  struct FastSocket* socket;

  socket = (struct FastSocket*)descriptor->closure;

  if (unlikely((completion != NULL) &&
               (completion->res < 0)))
  {
    // Error may occure during sending or connecting
    CallHandlerFunction(socket, POLLERR, -completion->res);
    goto Continue;
  }

  if (unlikely((descriptor->submission.opcode == IORING_OP_POLL_ADD) &&
               (completion      != NULL) &&
               (completion->res >= POLLERR)))
  {
    // Error may occure during connecting (POLLERR, POLLHUP)
    CallHandlerFunction(socket, POLLERR, EPIPE);
    goto Continue;
  }

  Continue:

  if ((~descriptor->submission.flags & IOSQE_IO_LINK) &&
      ( socket->outbound.condition   & POLLOUT) &&
      ( descriptor->data.number == 0ULL))
  {
    // In case of TCP the kernel may occupy a buffer for much longer,
    // notify handler once about accepted buffer as soon as possible

    descriptor->data.number ++;

    if ( (batch  = socket->outbound.tail) &&
        ((batch != socket->outbound.head) ||
         (socket->outbound.condition & POLLHUP)))
    {
      socket->outbound.tail                  = batch->next;
      *((uintptr_t*)&socket->outbound.head) *= (uintptr_t)(socket->outbound.tail != NULL);
      SubmitFastRingDescriptorRange(batch->tail, batch->head);
      ReleaseOutboundBatch(&socket->outbound, batch);
    }
    else
    {
      socket->outbound.condition &= ~POLLOUT;
      CallHandlerFunction(socket, POLLOUT, 0);
    }
  }

  if (( completion == NULL) ||
      (~completion->flags & IORING_CQE_F_MORE))
  {
    switch (descriptor->submission.opcode)
    {
      case IORING_OP_SEND:
      case IORING_OP_SEND_ZC:
      case IORING_OP_WRITE:
      case IORING_OP_WRITE_FIXED:
        ReleaseFastBuffer(FAST_BUFFER(descriptor->submission.addr));
        break;

      case IORING_OP_SENDMSG:
      case IORING_OP_SENDMSG_ZC:
      case IORING_OP_WRITEV:
        ReleaseFastBuffer(FAST_BUFFER(descriptor->data.socket.vector.iov_base));
        break;
    }

    ReleaseSocketInstance(socket, reason);
    return 0;
  }

  return 1;
}

static void HandleOutboundFlush(struct FastRing* ring, void* closure)
{
  struct FastSocketOutboundBatch* batch;
  struct FastSocket* socket;

  socket                      = (struct FastSocket*)closure;
  socket->outbound.condition &= ~POLLIN;

  if (likely((~socket->outbound.condition & POLLOUT) &&
             (batch = socket->outbound.tail)))
  {
    socket->outbound.condition            |= POLLOUT;
    socket->outbound.tail                  = batch->next;
    *((uintptr_t*)&socket->outbound.head) *= (uintptr_t)(socket->outbound.tail != NULL);

    SubmitFastRingDescriptorRange(batch->tail, batch->head);
    ReleaseOutboundBatch(&socket->outbound, batch);
  }

  ReleaseSocketInstance(socket, 0);
}

struct FastSocket* CreateFastSocket(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, struct msghdr* message, int flags, int mode, uint32_t limit, HandleFastSocketEvent function, void* closure)
{
  struct FastRingDescriptor* descriptor;
  struct FastSocket* socket;

  if (socket = (struct FastSocket*)calloc(1, sizeof(struct FastSocket)))
  {
    socket->ring               = ring;
    socket->handle             = handle;
    socket->closure            = closure;
    socket->function           = function;
    socket->count              = 2;         // IORING_OP_RECV + socket
    socket->inbound.descriptor = AllocateFastRingDescriptor(ring, HandleInboundCompletion, socket);
    socket->inbound.provider   = provider;
    socket->inbound.pool       = inbound;
    socket->outbound.limit     = ring->ring.cq.ring_entries / 2;
    socket->outbound.pool      = outbound;
    socket->outbound.mode      = mode;

    descriptor = socket->inbound.descriptor;

    if (descriptor == NULL)
    {
      free(socket);
      return NULL;
    }

    if ((limit > 0) &&
        (limit < socket->outbound.limit))
    {
      // Use pre-defined limit for IORING_OP_SEND_ZC SQEs
      socket->outbound.limit = limit;
    }

#if (IO_URING_VERSION_MAJOR > 2) || (IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR >= 6)
    if (mode & MSG_DONTROUTE)
    {
      io_uring_prep_read_multishot(&descriptor->submission, handle, 0, -1, 0);
      goto Continue;
    }
#endif

    if (message == NULL)
    {
      // Socket address or control data are not required, make simple multi-short submission
      io_uring_prep_recv_multishot(&descriptor->submission, handle, NULL, 0, flags);
      goto Continue;
    }

    memcpy(&descriptor->data.socket.message, message, sizeof(struct msghdr));
    io_uring_prep_recvmsg_multishot(&descriptor->submission, handle, &descriptor->data.socket.message, flags);

    Continue:

    PrepareFastRingBuffer(socket->inbound.provider, &descriptor->submission);
    SubmitFastRingDescriptor(socket->inbound.descriptor, 0);

    if (descriptor = AllocateFastRingDescriptor(ring, NULL, NULL))
    {
      io_uring_prep_poll_add(&descriptor->submission, handle, POLLOUT | POLLHUP | POLLERR);
      TransmitFastSocketDescriptor(socket, descriptor, NULL);
    }
  }

  return socket;
}

ssize_t ReceiveFastSocketData(struct FastSocket* socket, void* data, size_t size, int flags)
{
  struct FastBuffer* buffer;
  ssize_t count;
  ssize_t rest;

  if (unlikely((socket == NULL) ||
               (data   == NULL) ||
               (size   == 0)))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  if (unlikely((socket->inbound.length == 0) ||
               (socket->inbound.length < size) &&
               (flags & MSG_WAITALL)))
  {
    // Insufficient length
    return 0;
  }

  size  = (socket->inbound.length < size) ? socket->inbound.length : size;
  count = size;

  while (count > 0)
  {
    buffer = socket->inbound.tail;
    rest   = buffer->length - socket->inbound.position;

    if (count < rest)
    {
      memcpy(data, buffer->data + socket->inbound.position, count);
      socket->inbound.position += count;
      socket->inbound.length   -= count;
      break;
    }

    memcpy(data, buffer->data + socket->inbound.position, rest);
    data  += rest;
    count -= rest;

    socket->inbound.position  = 0;
    socket->inbound.length   -= rest;
    socket->inbound.tail      = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  return size;
}

int TransmitFastSocketDescriptor(struct FastSocket* socket, struct FastRingDescriptor* descriptor, struct FastBuffer* buffer)
{
  struct FastSocketOutboundBatch* batch;

  if (unlikely((socket     == NULL) ||
               (descriptor == NULL) ||
               (buffer     == NULL) &&
               (descriptor->submission.opcode != IORING_OP_POLL_ADD) &&
               (descriptor->submission.opcode != IORING_OP_URING_CMD)))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  if (unlikely((socket->outbound.condition & POLLERR)))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -EPIPE;
  }

  if (unlikely(!((batch = socket->outbound.head) &&
                 (batch->count < socket->outbound.limit) ||
                 (batch = AllocateOutboundBatch(&socket->outbound)))))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -ENOMEM;
  }

  descriptor->data.number        = 0ULL;
  descriptor->function           = HandleOutboundCompletion;
  descriptor->closure            = socket;
  descriptor->submission.ioprio |= IORING_RECVSEND_POLL_FIRST *
    ((descriptor->submission.opcode == IORING_OP_SEND)    ||
     (descriptor->submission.opcode == IORING_OP_SEND_ZC) ||
     (descriptor->submission.opcode == IORING_OP_SENDMSG) ||
     (descriptor->submission.opcode == IORING_OP_SENDMSG_ZC));
  PrepareFastRingDescriptor(descriptor, 0);

  batch->count  ++;
  socket->count ++;

  if (unlikely(batch->tail == NULL))
  {
    batch->tail = descriptor;
    batch->head = descriptor;
  }
  else
  {
    batch->head->submission.flags     |= IOSQE_IO_LINK;
    batch->head->submission.msg_flags |= (socket->outbound.mode & MSG_MORE) *
      ((descriptor->submission.opcode == IORING_OP_SEND)    ||
       (descriptor->submission.opcode == IORING_OP_SEND_ZC) ||
       (descriptor->submission.opcode == IORING_OP_SENDMSG) ||
       (descriptor->submission.opcode == IORING_OP_SENDMSG_ZC));

    batch->tail->linked = batch->count;
    batch->head->next   = descriptor;
    batch->head         = descriptor;
  }

  if (unlikely(~socket->outbound.condition & POLLIN))
  {
    socket->count ++;
    socket->outbound.condition |= POLLIN;
    SetFastRingFlushHandler(socket->ring, HandleOutboundFlush, socket);
  }

  return 0;
}

int TransmitFastSocketMessage(struct FastSocket* socket, struct msghdr* message, int flags)
{
  struct FastRingDescriptor* descriptor;
  struct FastBuffer* buffer;
  struct iovec* vector;
  struct iovec* limit;
  uint8_t* pointer;
  size_t length;

  if (unlikely((socket  == NULL) ||
               (message == NULL)))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  length = 0;
  vector = message->msg_iov;
  limit  = message->msg_iov + message->msg_iovlen;

  while (vector < limit)
  {
    length += vector->iov_len;
    vector ++;
  }

  descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL);
  buffer     = AllocateFastBuffer(socket->outbound.pool, length + message->msg_controllen, 0);

  if (unlikely((descriptor == NULL) ||
               (buffer     == NULL)))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -ENOMEM;
  }

  pointer = buffer->data;
  vector  = message->msg_iov;

  while (vector < limit)
  {
    memcpy(pointer, vector->iov_base, vector->iov_len);
    pointer += vector->iov_len;
    vector  ++;
  }

  if (message->msg_controllen == 0)
  {
    io_uring_prep_send(&descriptor->submission, socket->handle, buffer->data, length, flags);

    descriptor->submission.opcode += (IORING_OP_SEND_ZC - IORING_OP_SEND) * !!(socket->outbound.mode & MSG_ZEROCOPY);

    if (message->msg_namelen != 0)
    {
      memcpy(&descriptor->data.socket.address, message->msg_name, message->msg_namelen);
      io_uring_prep_send_set_addr(&descriptor->submission, (struct sockaddr*)&descriptor->data.socket.address, message->msg_namelen);
    }
  }
  else
  {
    io_uring_prep_sendmsg(&descriptor->submission, socket->handle, &descriptor->data.socket.message, flags);
    memcpy(pointer, message->msg_control, message->msg_controllen);

    descriptor->submission.opcode                  += (IORING_OP_SENDMSG_ZC - IORING_OP_SENDMSG) * !!(socket->outbound.mode & MSG_ZEROCOPY);
    descriptor->data.socket.vector.iov_base         = buffer->data;
    descriptor->data.socket.vector.iov_len          = length;
    descriptor->data.socket.message.msg_iov         = &descriptor->data.socket.vector;
    descriptor->data.socket.message.msg_iovlen      = 1;
    descriptor->data.socket.message.msg_name        = NULL;
    descriptor->data.socket.message.msg_iovlen      = 0;
    descriptor->data.socket.message.msg_control     = pointer;
    descriptor->data.socket.message.msg_controllen  = message->msg_controllen;

    if (message->msg_namelen != 0)
    {
      memcpy(&descriptor->data.socket.address, message->msg_name, message->msg_namelen);
      descriptor->data.socket.message.msg_name   = &descriptor->data.socket.address;
      descriptor->data.socket.message.msg_iovlen = message->msg_namelen;
    }
  }

  return TransmitFastSocketDescriptor(socket, descriptor, buffer);
}

int TransmitFastSocketData(struct FastSocket* socket, struct sockaddr* address, socklen_t length, const void* data, size_t size, int flags)
{
  struct FastRingDescriptor* descriptor;
  struct FastBuffer* buffer;

  if (unlikely(socket == NULL))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL);
  buffer     = AllocateFastBuffer(socket->outbound.pool, size, 0);

  if (unlikely((descriptor == NULL) ||
               (buffer     == NULL)))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -ENOMEM;
  }

  memcpy(buffer->data, data, size);
  io_uring_prep_send(&descriptor->submission, socket->handle, buffer->data, size, flags);

  descriptor->submission.opcode += (IORING_OP_SEND_ZC - IORING_OP_SEND)  * !!(socket->outbound.mode & MSG_ZEROCOPY);
  descriptor->submission.opcode -= (IORING_OP_SEND    - IORING_OP_WRITE) * !!(socket->outbound.mode & MSG_DONTROUTE);
  descriptor->submission.off    -=                                         !!(socket->outbound.mode & MSG_DONTROUTE);

  if (length != 0)
  {
    memcpy(&descriptor->data.socket.address, address, length);
    io_uring_prep_send_set_addr(&descriptor->submission, (struct sockaddr*)&descriptor->data.socket.address, length);
  }

  return TransmitFastSocketDescriptor(socket, descriptor, buffer);
}

void ReleaseFastSocket(struct FastSocket* socket)
{
  struct FastRingDescriptor* descriptor;

  if (socket != NULL)
  {
    socket->outbound.condition |= POLLHUP;

    if ((descriptor = socket->inbound.descriptor) &&
        (descriptor->state == RING_DESC_STATE_PENDING))
    {
      io_uring_prep_nop(&descriptor->submission);
      descriptor->submission.user_data |= RING_DESC_OPTION_IGNORE;
      socket->inbound.descriptor        = NULL;
      socket->count                    --;
    }

    if (descriptor = socket->inbound.descriptor)
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      socket->inbound.descriptor = NULL;
    }

    socket->closure  = NULL;
    socket->function = NULL;

    ReleaseSocketInstance(socket, -1);
  }
}
