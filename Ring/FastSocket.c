#include "FastSocket.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static inline void __attribute__((always_inline)) ReleaseSocketInstance(struct FastSocket* socket)
{
  struct FastSocketOutboundBatch* batch;
  struct FastBuffer* buffer;

  socket->count --;

  if (socket->count == 0)
  {
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

    close(socket->handle);
    free(socket);
  }
}

static inline void __attribute__((always_inline)) CallHandlerFunction(struct FastSocket* socket, int event, int parameter)
{
  if (socket->function != NULL)
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

  if (batch = queue->stack)
  {
    queue->stack = batch->next;
    batch->next  = NULL;
    goto AppendQueue;
  }

  if (batch = (struct FastSocketOutboundBatch*)calloc(1, sizeof(struct FastSocketOutboundBatch)))
  {
    AppendQueue:

    if ((queue->head != NULL) &&
        (queue->tail != NULL))
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

  if ((completion == NULL) ||
      (completion->res == -ECANCELED) &&     // Probably ui_uring has a bug:
      (socket->inbound.descriptor == NULL))  // When at least one of descriptors associated to the ring buffer group canceled, whole group will be canceled
  {
    socket->inbound.descriptor = NULL;
    CallHandlerFunction(socket, POLLHUP, 0);
    ReleaseSocketInstance(socket);
    return 0;
  }

  if ((completion->res < 0) &&
      (completion->res != -ENOBUFS) &&
      (completion->res != -ECANCELED))
  {
    socket->inbound.descriptor = NULL;
    CallHandlerFunction(socket, POLLHUP, -completion->res);
    ReleaseSocketInstance(socket);
    return 0;
  }

  if ((completion->res >= 0) &&
      (data = GetFastRingBuffer(socket->inbound.provider, completion)))
  {
    AdvanceFastRingBuffer(socket->inbound.provider, completion, AllocateRingFastBuffer, socket->inbound.pool);

    buffer                  = FAST_BUFFER(data);
    buffer->length          = completion->res;
    socket->inbound.length += completion->res;

    if (socket->inbound.tail == NULL)
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

  if (~completion->flags & IORING_CQE_F_MORE)
  {
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

  if ((completion != NULL) &&
      (completion->res < 0))
  {
    // Error may occure during sending or connecting
    CallHandlerFunction(socket, POLLERR, -completion->res);
  }

  if ((descriptor->submission.opcode == IORING_OP_POLL_ADD) &&
      (completion != NULL) &&
      (completion->res >= POLLERR))
  {
    // Error may occure during connecting (POLLERR, POLLHUP)
    CallHandlerFunction(socket, POLLERR, EPIPE);
  }

  if (( descriptor->data.number == 0) &&
      (~descriptor->submission.flags & IOSQE_IO_LINK) &&
      ( socket->outbound.condition   & POLLOUT))
  {
    // In case of TCP the kernel may occupy a buffer for much longer,
    // notify handler once about accepted buffer as soon as possible

    descriptor->data.number ++;

    if ((batch  = socket->outbound.tail) &&
        (batch != socket->outbound.head))
    {
      socket->outbound.tail = batch->next;
      SubmitFastRingDescriptorRange(batch->tail, batch->head);
      ReleaseOutboundBatch(&socket->outbound, batch);
    }
    else
    {
      socket->outbound.condition &= ~POLLOUT;
      CallHandlerFunction(socket, POLLOUT, 0);
    }
  }

  if ((completion == NULL) ||
      (~completion->flags & IORING_CQE_F_MORE))
  {
    switch (descriptor->submission.opcode)
    {
      case IORING_OP_SEND:
      case IORING_OP_SEND_ZC:
        ReleaseFastBuffer(FAST_BUFFER(descriptor->submission.addr));
        break;

      case IORING_OP_SENDMSG:
      case IORING_OP_SENDMSG_ZC:
        ReleaseFastBuffer(FAST_BUFFER(descriptor->data.socket.vector.iov_base));
        break;
    }

    ReleaseSocketInstance(socket);
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

  if (~socket->outbound.condition & POLLOUT)
  {
    batch                                  = socket->outbound.tail;
    socket->outbound.tail                  = batch->next;
    *((uintptr_t*)&socket->outbound.head) *= (socket->outbound.tail != NULL);
    socket->outbound.condition            |= POLLOUT;

    SubmitFastRingDescriptorRange(batch->tail, batch->head);
    ReleaseOutboundBatch(&socket->outbound, batch);
  }

  ReleaseSocketInstance(socket);
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
    socket->outbound.mode      = mode & FASTSOCKET_MODE_ZERO_COPY;

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

    if (message == NULL)
    {
      // Socket address or control data are not required, make simple multi-short submission
      io_uring_prep_recv_multishot(&descriptor->submission, handle, NULL, 0, flags);
    }
    else
    {
      memcpy(&descriptor->data.socket.message, message, sizeof(struct msghdr));
      io_uring_prep_recvmsg_multishot(&descriptor->submission, handle, &descriptor->data.socket.message, flags);
    }

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

  if ((socket == NULL) ||
      (data   == NULL) ||
      (size   == 0))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  if ((socket->inbound.length == 0) ||
      (socket->inbound.length < size) &&
      (flags & MSG_WAITALL))
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

  if ((socket     == NULL) ||
      (descriptor == NULL) ||
      (buffer     == NULL) &&
      (descriptor->submission.opcode != IORING_OP_POLL_ADD))
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  if (!((batch = socket->outbound.head) &&
        (batch->count < socket->outbound.limit) ||
        (batch = AllocateOutboundBatch(&socket->outbound))))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -ENOMEM;
  }

  descriptor->function              = HandleOutboundCompletion;
  descriptor->closure               = socket;
  descriptor->state                 = RING_DESC_STATE_PENDING;     // It' required to set these values manually
  descriptor->submission.user_data  = (uintptr_t)descriptor;       // due to use SubmitFastRingDescriptorRange()
  descriptor->submission.ioprio    |= IORING_RECVSEND_POLL_FIRST * (descriptor->submission.opcode != IORING_OP_POLL_ADD);
  descriptor->data.number           = 0;

  batch->count  ++;
  socket->count ++;

  if ((batch->head == NULL) &&
      (batch->tail == NULL))
  {
    batch->tail = descriptor;
    batch->head = descriptor;
  }
  else
  {
    batch->tail->linked            = batch->count;
    batch->head->next              = descriptor;
    batch->head->submission.flags |= IOSQE_IO_LINK;
    batch->head                    = descriptor;
  }

  if (~socket->outbound.condition & POLLIN)
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

  if ((socket  == NULL) ||
      (message == NULL))
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

  if ((descriptor == NULL) ||
      (buffer     == NULL))
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

    descriptor->submission.opcode += socket->outbound.mode * (IORING_OP_SEND_ZC - IORING_OP_SEND);

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

    descriptor->submission.opcode                  += socket->outbound.mode * (IORING_OP_SENDMSG_ZC - IORING_OP_SENDMSG);
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

  if (socket == NULL)
  {
    // Cannot proceed a call
    return -EINVAL;
  }

  descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL);
  buffer     = AllocateFastBuffer(socket->outbound.pool, size, 0);

  if ((descriptor == NULL) ||
      (buffer     == NULL))
  {
    ReleaseFastRingDescriptor(descriptor);
    ReleaseFastBuffer(buffer);
    return -ENOMEM;
  }

  memcpy(buffer->data, data, size);
  io_uring_prep_send(&descriptor->submission, socket->handle, buffer->data, size, flags);

  descriptor->submission.opcode += socket->outbound.mode * (IORING_OP_SEND_ZC - IORING_OP_SEND);

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
    if ((descriptor = socket->inbound.descriptor) &&
        (descriptor->state == RING_DESC_STATE_PENDING))
    {
      descriptor->submission.opcode  = IORING_OP_NOP;
      descriptor->function           = NULL;
      descriptor->closure            = NULL;
      socket->inbound.descriptor     = NULL;
      socket->count                 --;
    }

    if ((socket->inbound.descriptor != NULL) &&
        (descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL)))
    {
      io_uring_prep_cancel(&descriptor->submission, socket->inbound.descriptor, 0);
      SubmitFastRingDescriptor(descriptor, 0);
    }

    socket->closure            = NULL;
    socket->function           = NULL;
    socket->inbound.descriptor = NULL;

    ReleaseSocketInstance(socket);
  }
}
