#define _GNU_SOURCE

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "FastSocket.h"

#define likely(condition)     __builtin_expect(!!(condition), 1)
#define unlikely(condition)   __builtin_expect(!!(condition), 0)

static int HandleReleaseCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if (completion == NULL)
  {
    // Close manually only if the queued close never ran
    close(descriptor->submission.fd);
  }

  return 0;
}

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

  if (descriptor = AllocateFastRingDescriptor(socket->ring, HandleReleaseCompletion, NULL))
  {
    io_uring_prep_close(&descriptor->submission, socket->handle);
    SubmitFastRingDescriptor(descriptor, 0);
  }
  else
  {
    // Allocation failed
    close(socket->handle);
  }

  free(socket);
}

static inline void __attribute__((always_inline)) ReleaseSocketInstance(struct FastSocket* socket, int reason)
{
  socket->count --;

  if (unlikely(socket->count == 0))
  {
    // Keep cold cleanup out of line
    FreeSocketInstance(socket, reason);
  }
}

static inline void __attribute__((always_inline)) CallHandlerFunction(struct FastSocket* socket, int event, int parameter)
{
  if (likely(socket->function != NULL))
  {
    // Handler may already be detached
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
    return batch;
  }

  return (struct FastSocketOutboundBatch*)calloc(1, sizeof(struct FastSocketOutboundBatch));
}

static inline struct FastSocketOutboundBatch* __attribute__((always_inline)) PrependOutboundBatch(struct FastSocketOutboundQueue* queue)
{
  struct FastSocketOutboundBatch* batch;

  if (likely(batch = AllocateOutboundBatch(queue)))
  {
    batch->next = queue->tail;
    queue->tail = batch;

    if (queue->head == NULL)
    {
      //
      queue->head = batch;
    }
  }

  return batch;
}

static inline struct FastSocketOutboundBatch* __attribute__((always_inline)) AppendOutboundBatch(struct FastSocketOutboundQueue* queue)
{
  struct FastSocketOutboundBatch* batch;

  if (likely(batch = AllocateOutboundBatch(queue)))
  {
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
    // Ignore the CQE racing with io_uring_prep_cancel()
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

  if (unlikely((completion->res == 0) &&
               (~completion->flags & IORING_CQE_F_BUFFER)))
  {
    socket->inbound.descriptor = NULL;
    CallHandlerFunction(socket, POLLHUP, 0);
    ReleaseSocketInstance(socket, reason);
    return 0;
  }

  if (likely((completion->res >= 0) &&
             (data = GetFastRingBuffer(socket->inbound.provider, completion))))
  {
    AdvanceFastRingBuffer(socket->inbound.provider, completion, AllocateRingFastBuffer, socket->inbound.pool);

    buffer                     = FAST_BUFFER(data);
    buffer->length             =  completion->res;
    socket->inbound.length    +=  completion->res;
    socket->inbound.condition  = ~completion->flags;

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

    socket->inbound.condition = 0;
  }

  if (unlikely(~completion->flags & IORING_CQE_F_MORE))
  {
    if (socket->inbound.descriptor == NULL)
    {
      // Handler may close the socket on the final receive
      ReleaseSocketInstance(socket, reason);
      return 0;
    }

    // Rearm a terminated multishot receive
    SubmitFastRingDescriptor(socket->inbound.descriptor, 0);
  }

  return 1;
}

static void HandleOutboundFlush(void* closure, int reason)
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

static inline void __attribute__((always_inline)) ScheduleOutboundFlush(struct FastSocket* socket)
{
  socket->count ++;
  socket->outbound.condition |= POLLIN;

  if (unlikely(SetFastRingFlushHandler(socket->ring, HandleOutboundFlush, socket) == NULL))
  {
    //
    HandleOutboundFlush(socket, RING_REASON_COMPLETE);
  }
}

static int HandleOutboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastSocketOutboundBatch* batch;
  struct FastSocket* socket;

  socket = (struct FastSocket*)descriptor->closure;

  if (unlikely((completion != NULL) &&
               (completion->res < 0)))
  {
    batch = NULL;

    if (((descriptor->submission.opcode == IORING_OP_SEND_ZC)     ||
         (descriptor->submission.opcode == IORING_OP_SENDMSG_ZC)) &&
        ((completion->res == -ENOBUFS)   ||
         (completion->res == -ECANCELED) &&
         (batch = socket->outbound.tail) &&
         (batch->head == NULL)))
    {
      if (completion->res == -ENOBUFS)
      {
        batch = PrependOutboundBatch(&socket->outbound);

        if (unlikely(batch == NULL))
        {
          // Batch allocation failed
          goto Error;
        }

        // Keep new data behind the retry batch
        batch->count = socket->outbound.limit;
        batch->tail  = descriptor;
      }

      if (~descriptor->submission.flags & IOSQE_IO_LINK)
      {
        // The unlinked descriptor closes the failed batch
        batch->head = descriptor;
      }

      // Cancelled ZC requests still notify without F_MORE on older kernels
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      PrepareFastRingDescriptor(descriptor, 0);
      socket->count ++;

      if (descriptor->submission.opcode == IORING_OP_SEND_ZC)  HoldFastBuffer(FAST_BUFFER(descriptor->submission.addr));
      else                                                     HoldFastBuffer(FAST_BUFFER(descriptor->data.socket.vector.iov_base));

      descriptor->linked  = 0;
      descriptor          = batch->tail;
      descriptor->linked ++;
      return 1;
    }

    Error:

    // Latch send failures so later transmits return EPIPE
    socket->outbound.condition |= POLLERR;
    CallHandlerFunction(socket, POLLERR, -completion->res);
    goto Continue;
  }

  if (unlikely((descriptor->submission.opcode == IORING_OP_POLL_ADD) &&
               (completion      != NULL) &&
               (completion->res >= POLLERR)))
  {
    // Connecting failed
    socket->outbound.condition |= POLLERR;
    CallHandlerFunction(socket, POLLERR, EPIPE);
    goto Continue;
  }

  if (unlikely((completion      != NULL) &&
               (completion->res >= 0)    &&
               (completion->res <  descriptor->submission.len) &&
               ((descriptor->submission.opcode == IORING_OP_WRITE) ||
                (descriptor->submission.opcode == IORING_OP_WRITE_FIXED))))
  {
    // WRITE has no MSG_WAITALL; only files retry short writes

    if ((completion->res > 0) &&
        (~descriptor->submission.flags & IOSQE_IO_LINK))
    {
      // Only an unlinked tail can be retried without reordering
      // Advance explicit offsets; ~0ULL means kernel-managed position
      descriptor->submission.addr += completion->res;
      descriptor->submission.len  -= completion->res;
      descriptor->submission.off  += completion->res * (descriptor->submission.off != ~0ULL);
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    // Retrying a partial linked write would reorder the stream
    socket->outbound.condition |= POLLERR;
    CallHandlerFunction(socket, POLLERR, EIO);
  }

  Continue:

  if ((~descriptor->submission.flags & IOSQE_IO_LINK) &&
      ( socket->outbound.condition   & POLLOUT) &&
      ( descriptor->data.number == 0ULL))
  {
    // Report acceptance before delayed ZC buffer release

    descriptor->data.number ++;
    batch = socket->outbound.tail;

    if ((batch       != NULL) &&
        (batch->head != NULL))
    {
      // Restore the retry tail's one-shot marker
      batch->head->data.number = 0ULL;
    }

    if ((batch       != NULL) &&
        (batch->head != NULL) &&
        ((batch      != socket->outbound.head) ||
         (socket->outbound.condition & POLLHUP)))
    {
      socket->outbound.tail                  = batch->next;
      *((uintptr_t*)&socket->outbound.head) *= (uintptr_t)(socket->outbound.tail != NULL);
      SubmitFastRingDescriptorRange(batch->tail, batch->head);
      ReleaseOutboundBatch(&socket->outbound, batch);
      goto Release;
    }

    if ((batch       == NULL) ||
        (batch->head != NULL))
    {
      socket->outbound.condition &= ~POLLOUT;

      if (unlikely(( socket->outbound.tail      != NULL) &&
                   (~socket->outbound.condition &  POLLIN)))
      {
        //
        ScheduleOutboundFlush(socket);
      }

      CallHandlerFunction(socket, POLLOUT, 0);
    }
  }

  Release:

  if (( completion == NULL) ||
      (~completion->flags & IORING_CQE_F_MORE))
  {
    switch (descriptor->submission.opcode)
    {
      case IORING_OP_SEND:
      case IORING_OP_SEND_ZC:
        ReleaseFastBuffer(FAST_BUFFER(descriptor->submission.addr));
        break;

      case IORING_OP_WRITE:
      case IORING_OP_WRITE_FIXED:
        // A short write may advance addr; release the saved base
        ReleaseFastBuffer(FAST_BUFFER(descriptor->data.socket.vector.iov_base));
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
    socket->outbound.limit     = ring->ring.sq.ring_entries / 2;
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
      // Use the configured SEND_ZC limit
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
      // No address or control data: use plain multishot receive
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
  size_t count;
  size_t rest;

  if (unlikely((socket == NULL) ||
               (data   == NULL) ||
               (size   == 0)))
  {
    // Invalid call
    return -EINVAL;
  }

  if (unlikely((socket->inbound.length == 0) ||
               (socket->inbound.length < size) &&
               (flags & MSG_WAITALL)))
  {
    // No complete buffer yet; peer closure is reported as POLLHUP
    return -EAGAIN;
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
    data   = (uint8_t*)data + rest;
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
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
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
                 (batch = AppendOutboundBatch(&socket->outbound)))))
  {
    // Batch exhaustion is transient
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -EAGAIN;
  }

  descriptor->data.number = 0ULL;
  descriptor->function    = HandleOutboundCompletion;
  descriptor->closure     = socket;

  if ((descriptor->submission.opcode == IORING_OP_SEND)    ||
      (descriptor->submission.opcode == IORING_OP_SEND_ZC) ||
      (descriptor->submission.opcode == IORING_OP_SENDMSG) ||
      (descriptor->submission.opcode == IORING_OP_SENDMSG_ZC))
  {
    // Prevent partial linked sends from reordering the stream
    descriptor->submission.ioprio    |= IORING_RECVSEND_POLL_FIRST;
    descriptor->submission.msg_flags |= MSG_WAITALL;
  }

  if ((descriptor->submission.opcode == IORING_OP_WRITE) ||
      (descriptor->submission.opcode == IORING_OP_WRITE_FIXED))
  {
    // Preserve the base while a short write advances addr
    descriptor->data.socket.vector.iov_base = (void*)descriptor->submission.addr;
  }

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
    ScheduleOutboundFlush(socket);
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
               (message == NULL) ||
               (message->msg_iovlen     != 0)    &&
               (message->msg_iov        == NULL) ||
               (message->msg_controllen != 0)    &&
               (message->msg_control    == NULL) ||
               (message->msg_namelen    != 0) &&
               ((message->msg_name      == NULL) ||
                (message->msg_namelen   >  sizeof(struct sockaddr_storage)))))
  {
    // Invalid call
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
    // Pool exhaustion is transient
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -EAGAIN;
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
    descriptor->data.socket.message.msg_namelen     = 0;
    descriptor->data.socket.message.msg_control     = pointer;
    descriptor->data.socket.message.msg_controllen  = message->msg_controllen;

    if (message->msg_namelen != 0)
    {
      memcpy(&descriptor->data.socket.address, message->msg_name, message->msg_namelen);
      descriptor->data.socket.message.msg_name    = &descriptor->data.socket.address;
      descriptor->data.socket.message.msg_namelen = message->msg_namelen;
    }
  }

  return TransmitFastSocketDescriptor(socket, descriptor, buffer);
}

int TransmitFastSocketData(struct FastSocket* socket, struct sockaddr* address, socklen_t length, const void* data, size_t size, int flags)
{
  struct FastRingDescriptor* descriptor;
  struct FastBuffer* buffer;

  if (unlikely((socket   == NULL) ||
               (size     != 0)    &&
               (data     == NULL) ||
               (length   != 0)    &&
               ((address == NULL) ||
                (length   > sizeof(struct sockaddr_storage)))))
  {
    // Invalid call
    return -EINVAL;
  }

  descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL);
  buffer     = AllocateFastBuffer(socket->outbound.pool, size, 0);

  if (unlikely((descriptor == NULL) ||
               (buffer     == NULL)))
  {
    // Pool exhaustion is transient
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -EAGAIN;
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

    if (socket->inbound.condition & IORING_CQE_F_MORE)
    {
      // Handler may destroy the socket on the final buffer
      socket->inbound.descriptor = NULL;
    }

    if ((descriptor = socket->inbound.descriptor) &&
        (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_PENDING))
    {
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_nop(&descriptor->submission);
      PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      socket->inbound.descriptor = NULL;
      socket->count --;
    }

    if (descriptor = socket->inbound.descriptor)
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      socket->inbound.descriptor = NULL;
    }

    socket->closure  = NULL;
    socket->function = NULL;

    ReleaseSocketInstance(socket, -1);
  }
}

static ssize_t HandleStreamRead(void* cookie, char* data, size_t size)
{
  struct FastSocket* socket;
  int result;

  socket = (struct FastSocket*)cookie;
  result = ReceiveFastSocketData(socket, data, size, 0);

  if (unlikely((result == -EAGAIN) &&
               (socket->inbound.descriptor == NULL)))
  {
    // A drained queue and terminated receive path are EOF
    return 0;
  }

  if (unlikely(result < 0))
  {
    errno = -result;
    return -1;
  }

  return result;
}

static ssize_t HandleStreamWrite(void* cookie, const char* data, size_t size)
{
  int result;

  result = TransmitFastSocketData((struct FastSocket*)cookie, NULL, 0, data, size, 0);

  if (unlikely(result < 0))
  {
    errno = -result;
    return -1;
  }

  return size;
}

static int HandleStreamClose(void* cookie)
{
  ReleaseFastSocket((struct FastSocket*)cookie);
  return 0;
}

FILE* GetFastSocketStream(struct FastSocket* socket, int own)
{
  cookie_io_functions_t functions;

  functions.seek  = NULL;
  functions.read  = HandleStreamRead;
  functions.write = HandleStreamWrite;
  functions.close = NULL;

  if (own)
  {
    // Drop fclose ownership
    functions.close = HandleStreamClose;
  }

  return fopencookie(socket, "a+", functions);
}
