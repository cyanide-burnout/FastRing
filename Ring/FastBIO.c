#include "FastBIO.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// Non-blocked I/O:
// https://gist.github.com/darrenjs/4645f115d10aa4b5cebf57483ec82eca
// https://github.com/stepheny/openssl-dtls-custom-bio/tree/master
// https://github.com/jiangwenyuan/nuster/blob/master/src/ssl_sock.c
// https://famellee.wordpress.com/2013/02/20/use-openssl-with-io-completion-port-and-certificate-signing/

// Supplementary

static void FreeEngine(struct FastBIO* engine, int reason)
{
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;

  while (buffer = engine->inbound.tail)
  {
    engine->inbound.tail = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  if (((reason == RING_REASON_COMPLETE) ||
       (reason == RING_REASON_INCOMPLETE)) &&
      (descriptor = AllocateFastRingDescriptor(engine->ring, NULL, NULL)))
  {
    io_uring_prep_close(&descriptor->submission, engine->handle);
    SubmitFastRingDescriptor(descriptor, 0);
  }
  else
  {
    // FastRing might be under destruction, close socket synchronously
    close(engine->handle);
  }

  free(engine);
}

static void inline __attribute__((always_inline)) ReleaseEngine(struct FastBIO* engine, int reason)
{
  engine->count --;

  if (engine->count == 0)
  {
    // Prevent inlining less used code
    FreeEngine(engine, reason);
  }
}

static void inline __attribute__((always_inline)) CallHandlerFunction(struct FastBIO* engine, int event, int parameter)
{
  if (engine->function != NULL)
  {
    // Handler can be freed earlier then engine
    engine->function(engine, event, parameter);
  }
}

// FastRing

static int HandleInboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  uint8_t* data;
  struct FastBIO* engine;
  struct FastBuffer* buffer;

  engine = (struct FastBIO*)descriptor->closure;

  if ((completion == NULL) ||
      (completion->res == -ECANCELED) &&
      (engine->inbound.descriptor == NULL))
  {
    engine->inbound.descriptor = NULL;
    CallHandlerFunction(engine, POLLHUP, 0);
    ReleaseEngine(engine, reason);
    return 0;
  }

  if ((completion->res <= 0) &&
      (completion->res != -ENOBUFS) &&
      (completion->res != -ECANCELED))
  {
    if (completion->flags & IORING_CQE_F_MORE)
    {
      CallHandlerFunction(engine, POLLERR, -completion->res);
      return 1;
    }

    engine->inbound.descriptor = NULL;
    CallHandlerFunction(engine, POLLHUP, -completion->res);
    ReleaseEngine(engine, reason);
    return 0;
  }

  if ((completion->res > 0) &&
      (data = GetFastRingBuffer(engine->inbound.provider, completion)))
  {
    AdvanceFastRingBuffer(engine->inbound.provider, completion, AllocateRingFastBuffer, engine->inbound.pool);

    buffer                  = FAST_BUFFER(data);
    buffer->length          = completion->res;
    engine->inbound.length += completion->res;

    if (engine->inbound.tail == NULL)
    {
      engine->inbound.tail = buffer;
      engine->inbound.head = buffer;
    }
    else
    {
      engine->inbound.head->next = buffer;
      engine->inbound.head       = buffer;
    }

    CallHandlerFunction(engine, POLLIN, engine->inbound.length);
  }

  if (~completion->flags & IORING_CQE_F_MORE)
  {
    if (engine->inbound.descriptor == NULL)
    {
      // Socket could be closed by CallHandlerFunction()
      // at the same time with receive last packet
      return 0;
    }

    // Eventually URing may release submission
    // Also this handles -ENOBUFS and -ECANCELED
    SubmitFastRingDescriptor(engine->inbound.descriptor, 0);
  }

  return 1;
}

static int HandleOutboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastBIO* engine;

  engine = (struct FastBIO*)descriptor->closure;

  if ((completion != NULL) &&
      (completion->res < 0))
  {
    // Error may occure during sending
    CallHandlerFunction(engine, POLLERR, -completion->res);
    goto Continue;
  }

  if (( descriptor->data.number == 0) &&
      (~descriptor->submission.flags & IOSQE_IO_LINK) &&
      ( engine->outbound.condition   & POLLOUT))
  {
    // In case of TCP the kernel may occupy a buffer for much longer,
    // notify handler once about accepted buffer as soon as possible
    descriptor->data.number    ++;
    engine->outbound.condition &= ~POLLOUT;
    CallHandlerFunction(engine, POLLOUT, 0);
  }

  Continue:

  if ((completion == NULL) ||
      (~completion->flags & IORING_CQE_F_MORE))
  {
    ReleaseFastBuffer(FAST_BUFFER(descriptor->submission.addr));
    ReleaseEngine(engine, reason);
    return 0;
  }

  return 1;
}

static int HandleTouchCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastBIO* engine;

  engine = (struct FastBIO*)descriptor->closure;

  CallHandlerFunction(engine, 0, 0);
  ReleaseEngine(engine, reason);

  return 0;
}

static void FlushOutboundQueue(struct FastRing* ring, void* closure)
{
  struct FastBIO* engine;

  engine = (struct FastBIO*)closure;

  SubmitFastRingDescriptorRange(engine->outbound.tail, engine->outbound.head);

  engine->outbound.condition |= POLLOUT;
  engine->outbound.count      = 0;
  engine->outbound.tail       = NULL;
  engine->outbound.head       = NULL;
}

// OpenSSL BIO

static BIO_METHOD* method = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; 

static int HandleBIORead(BIO* handle, char* data, int length)
{
  int rest;
  int count;
  struct FastBIO* engine;
  struct FastBuffer* buffer;

  engine = (struct FastBIO*)BIO_get_data(handle);

  if (engine->inbound.length == 0)
  {
    BIO_set_retry_read(handle);
    return -1;
  }

  length = (length < engine->inbound.length) ? length : engine->inbound.length;
  count  = length;

  while (count > 0)
  {
    buffer = engine->inbound.tail;
    rest   = buffer->length - engine->inbound.position;

    if (count < rest)
    {
      memcpy(data, buffer->data + engine->inbound.position, count);
      engine->inbound.position += count;
      engine->inbound.length   -= count;
      break;
    }

    memcpy(data, buffer->data + engine->inbound.position, rest);
    data  += rest;
    count -= rest;

    engine->inbound.position  = 0;
    engine->inbound.length   -= rest;
    engine->inbound.tail      = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  BIO_clear_retry_flags(handle);
  return length;
}

static int HandleBIOWrite(BIO* handle, const char* data, int length)
{
  int size;
  struct FastBIO* engine;
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;

  engine = (struct FastBIO*)BIO_get_data(handle);

  if (descriptor = engine->outbound.head)
  {
    buffer = FAST_BUFFER(descriptor->submission.addr);
    size   = descriptor->submission.len + length;

    if (size <= buffer->size)
    {
      memcpy(buffer->data + descriptor->submission.len, data, length);
      descriptor->submission.len  = size;
      return length;
    }
  }

  if (engine->outbound.condition & POLLOUT)
  {
    BIO_set_retry_write(handle);
    return -1;
  }

  size       = length % engine->outbound.granularity;
  size       = length + (size > 0) * (engine->outbound.granularity - size);
  buffer     = AllocateFastBuffer(engine->outbound.pool, size, 0);
  descriptor = AllocateFastRingDescriptor(engine->ring, HandleOutboundCompletion, engine);

  if ((buffer     == NULL) ||
      (descriptor == NULL))
  {
    ReleaseFastBuffer(buffer);
    ReleaseFastRingDescriptor(descriptor);
    BIO_set_retry_write(handle);
    return -1;
  }

  memcpy(buffer->data, data, length);
  io_uring_prep_send_zc(&descriptor->submission, engine->handle, buffer->data, length, 0, IORING_RECVSEND_POLL_FIRST);

  descriptor->state                = RING_DESC_STATE_PENDING;  // It' required to set these values manually
  descriptor->submission.user_data = (uintptr_t)descriptor;    // due to use SubmitFastRingDescriptorRange()
  descriptor->data.number          = 0;                        //

  engine->outbound.count ++;
  engine->count          ++;

  if (engine->outbound.tail == NULL)
  {
    engine->outbound.tail = descriptor;
    engine->outbound.head = descriptor;
    SetFastRingFlushHandler(engine->ring, FlushOutboundQueue, engine);
  }
  else
  {
    engine->outbound.tail->linked                = engine->outbound.count;
    engine->outbound.head->next                  = descriptor;
    engine->outbound.head->submission.flags     |= IOSQE_IO_LINK;
    engine->outbound.head->submission.msg_flags  = MSG_WAITALL;
    engine->outbound.head                        = descriptor;
  }

  // Hold user-space transmission unless send completed
  engine->outbound.condition |= POLLOUT * (engine->outbound.count == engine->outbound.limit);

  BIO_clear_retry_flags(handle);
  return length;
}

static long HandleBIOControl(BIO* handle, int command, long argument1, void* argument2)
{
  struct FastBIO* engine;
  struct FastRingDescriptor* descriptor;

  engine = (struct FastBIO*)BIO_get_data(handle);

  switch (command)
  {
    case BIO_CTRL_DUP:
    case BIO_CTRL_FLUSH:
      return 1;

    case FASTBIO_CTRL_TOUCH:
      if ((engine->count == 2) &&
          (descriptor = AllocateFastRingDescriptor(engine->ring, HandleTouchCompletion, engine)))
      {
        engine->count ++;
        io_uring_prep_nop(&descriptor->submission);
        SubmitFastRingDescriptor(descriptor, 0);
      }
  }

  return 0;
}

static int HandleBIOCreate(BIO* handle)
{
  BIO_set_init(handle, 1);
  BIO_set_data(handle, NULL);
  BIO_clear_flags(handle, ~0);
  return 1;
}

static int HandleBIODestroy(BIO* handle)
{
  struct FastBIO* engine;
  struct FastRingDescriptor* descriptor;

  engine = (struct FastBIO*)BIO_get_data(handle);

  if ((descriptor = engine->inbound.descriptor) &&
      (descriptor->state == RING_DESC_STATE_PENDING))
  {
    descriptor->submission.opcode  = IORING_OP_NOP;
    descriptor->function           = NULL;
    descriptor->closure            = NULL;
    engine->inbound.descriptor     = NULL;
    engine->count                 --;
  }

  if ((engine->inbound.descriptor != NULL) &&
      (descriptor = AllocateFastRingDescriptor(engine->ring, NULL, NULL)))
  {
    io_uring_prep_cancel(&descriptor->submission, engine->inbound.descriptor, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  engine->closure            = NULL;
  engine->function           = NULL;
  engine->inbound.descriptor = NULL;

  ReleaseEngine(engine, -1);

  BIO_set_shutdown(handle, BIO_CLOSE);
  return 1;
}

static void __attribute__((destructor)) Finalize()
{
  if (method != NULL)
  {
    BIO_meth_free(method);
    method = NULL;
  }
}

// API

BIO* CreateFastBIO(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, uint32_t granularity, uint32_t limit, HandleFastBIOEvent function, void* closure)
{
  BIO* instance;
  struct FastBIO* engine;

  pthread_mutex_lock(&lock);

  if (method == NULL)
  {
    method = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "FastBIO");
    BIO_meth_set_write(method, HandleBIOWrite);
    BIO_meth_set_read(method, HandleBIORead);
    BIO_meth_set_ctrl(method, HandleBIOControl);
    BIO_meth_set_create(method, HandleBIOCreate);
    BIO_meth_set_destroy(method, HandleBIODestroy);
  }

  pthread_mutex_unlock(&lock);

  instance = BIO_new(method);
  engine   = (struct FastBIO*)calloc(1, sizeof(struct FastBIO));

  engine->ring                 = ring;
  engine->handle               = handle;
  engine->closure              = closure;
  engine->function             = function;
  engine->count                = 2;         // IORING_OP_RECV + BIO
  engine->inbound.descriptor   = AllocateFastRingDescriptor(ring, HandleInboundCompletion, engine);
  engine->inbound.provider     = provider;
  engine->inbound.pool         = inbound;
  engine->outbound.granularity = granularity;
  engine->outbound.limit       = ring->ring.cq.ring_entries / 2;
  engine->outbound.pool        = outbound;

  if ((limit > 0) &&
      (limit < engine->outbound.limit))
  {
    // Use pre-defined limit for IORING_OP_SEND_ZC SQEs
    engine->outbound.limit = limit;
  }

  io_uring_prep_recv_multishot(&engine->inbound.descriptor->submission, handle, NULL, 0, 0);
  PrepareFastRingBuffer(engine->inbound.provider, &engine->inbound.descriptor->submission);
  SubmitFastRingDescriptor(engine->inbound.descriptor, 0);

  BIO_set_data(instance, engine);
  return instance;
}
