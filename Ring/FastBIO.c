#include "FastBIO.h"

#include <malloc.h>
#include <endian.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/tls.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define likely(condition)    __builtin_expect(!!(condition), 1)
#define unlikely(condition)  __builtin_expect(!!(condition), 0)

_Static_assert(sizeof(struct tls12_crypto_info_aes_ccm_128)       <= sizeof(union FastRingData), "tls12_crypto_info_aes_ccm_128 must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_aes_gcm_128)       <= sizeof(union FastRingData), "tls12_crypto_info_aes_gcm_128 must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_aes_gcm_256)       <= sizeof(union FastRingData), "tls12_crypto_info_aes_gcm_256 must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_chacha20_poly1305) <= sizeof(union FastRingData), "tls12_crypto_info_chacha20_poly1305 must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_sm4_ccm)           <= sizeof(union FastRingData), "tls12_crypto_info_sm4_ccm must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_sm4_gcm)           <= sizeof(union FastRingData), "tls12_crypto_info_sm4_gcm must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_aria_gcm_128)      <= sizeof(union FastRingData), "tls12_crypto_info_aria_gcm_128 must fit in FastRingData");
_Static_assert(sizeof(struct tls12_crypto_info_aria_gcm_256)      <= sizeof(union FastRingData), "tls12_crypto_info_aria_gcm_256 must fit in FastRingData");

// Supplementary

static int HandleReleaseCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if ((completion == NULL) ||
      (completion->res != 0) &&
      (completion->res != -EBADF))
  {
    // Error may occure during closing
    close(descriptor->submission.fd);
  }

  return 0;
}

static void FreeEngine(struct FastBIO* engine, int reason)
{
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;

  while (buffer = engine->inbound.tail)
  {
    engine->inbound.tail = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  if (descriptor = AllocateFastRingDescriptor(engine->ring, HandleReleaseCompletion, NULL))
  {
    io_uring_prep_close(&descriptor->submission, engine->handle);
    SubmitFastRingDescriptor(descriptor, 0);
  }
  else
  {
    // Error may occure during allocation
    close(engine->handle);
  }

  free(engine);
}

static void inline __attribute__((always_inline)) ReleaseEngine(struct FastBIO* engine, int reason)
{
  engine->count --;

  if (unlikely(engine->count == 0))
  {
    // Prevent inlining less used code
    FreeEngine(engine, reason);
  }
}

static void inline __attribute__((always_inline)) CallHandlerFunction(struct FastBIO* engine, int event, int parameter)
{
  if (likely(engine->function != NULL))
  {
    // Handler can be freed earlier then engine
    engine->function(engine, event, parameter);
  }
}

static void inline __attribute__((always_inline)) AppendInboundQueue(struct FastBIO* engine, struct FastBuffer* buffer)
{
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
}

// FastRing

static int HandleInboundCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  int length;
  uint8_t* data;
  struct msghdr* message;
  struct FastBIO* engine;
  struct FastBuffer* buffer;
  struct io_uring_recvmsg_out* output;

  engine = (struct FastBIO*)descriptor->closure;

  if (unlikely(completion == NULL))
  {
    engine->inbound.descriptor = NULL;
    CallHandlerFunction(engine, POLLHUP, 0);
    ReleaseEngine(engine, reason);
    return 0;
  }

  if (unlikely(completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    // That's required to solve a possible race condition when proceed io_uring_prep_cancel()
    return 0;
  }

  if (unlikely(completion->res <= 0))
  {
    AdvanceFastRingBuffer(engine->inbound.provider, completion, NULL, NULL);

    if ((completion->res == -EKEYEXPIRED) &&
        (engine->flags & FASTBIO_FLAG_KTLS_RECEIVE) &&
        (buffer = AllocateFastBuffer(engine->inbound.pool, 0, 0)))
    {
      buffer->reserved          = -completion->res;
      engine->inbound.condition = ~completion->flags;

      AppendInboundQueue(engine, buffer);
      CallHandlerFunction(engine, POLLIN, engine->inbound.length);

      engine->inbound.condition = 0;
      goto Continue;
    }

    if ((completion->res != -ENOBUFS) &&
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
  }

  if (likely((completion->res > 0) &&
             (data = GetFastRingBuffer(engine->inbound.provider, completion))))
  {
    AdvanceFastRingBuffer(engine->inbound.provider, completion, AllocateRingFastBuffer, engine->inbound.pool);

    message = &descriptor->data.socket.message;
    buffer  = FAST_BUFFER(data);

    if (unlikely((descriptor->submission.opcode != IORING_OP_RECVMSG) ||
                 !(output = io_uring_recvmsg_validate(buffer->data, completion->res, message)) ||
                 !(length = io_uring_recvmsg_payload_length(output, completion->res, message))))
    {
      ReleaseFastBuffer(buffer);
      CallHandlerFunction(engine, POLLERR, ENOMSG);
      goto Continue;
    }

    buffer->length             =  completion->res;
    engine->inbound.condition  = ~completion->flags;
    engine->inbound.length    += length;

    AppendInboundQueue(engine, buffer);
    CallHandlerFunction(engine, POLLIN, engine->inbound.length);

    engine->inbound.condition = 0;
  }

  Continue:

  if (unlikely(~completion->flags & IORING_CQE_F_MORE))
  {
    if (engine->inbound.descriptor == NULL)
    {
      // Socket could be closed by function()
      // at the same time with receive last packet
      ReleaseEngine(engine, reason);
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

  if (unlikely((completion != NULL) &&
               (completion->res < 0)))
  {
    // Error may occure during sending
    CallHandlerFunction(engine, POLLERR, -completion->res);
    goto Continue;
  }

  if (( descriptor->data.socket.number == 0ULL) &&
      (~descriptor->submission.flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) &&
      ( engine->outbound.condition   & POLLOUT))
  {
    // In case of TCP the kernel may occupy a buffer for much longer,
    // notify handler once about accepted buffer as soon as possible
    descriptor->data.socket.number ++;
    engine->outbound.condition     &= ~POLLOUT;
    CallHandlerFunction(engine, POLLOUT, 0);
  }

  Continue:

  if ((completion == NULL) ||
      (~completion->flags & IORING_CQE_F_MORE))
  {
    ReleaseFastBuffer(FAST_BUFFER(descriptor->data.socket.vector.iov_base));
    ReleaseEngine(engine, reason);
    return 0;
  }

  return 1;
}

static int HandleOptionCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastBIO* engine;
  struct FastBuffer* buffer;

  engine = (struct FastBIO*)descriptor->closure;

  if (likely(completion != NULL))
  {
    if (unlikely((completion->res < 0)          &&
                 (completion->res != -EEXIST)   &&
                 (completion->res != -EALREADY) &&
                 (descriptor->submission.level   == SOL_TCP) &&
                 (descriptor->submission.optname == TCP_ULP)))
    {
      engine->flags &= ~FASTBIO_FLAG_KTLS_AVAILABLE;
      CallHandlerFunction(engine, POLLERR, -completion->res);
      goto Failure;
    }

    if (unlikely((completion->res < 0)          &&
                 (completion->res != -EEXIST)   &&
                 (completion->res != -EALREADY) &&
                 (descriptor->submission.level   == SOL_TLS) &&
                 (descriptor->submission.optname >= TLS_TX)  &&
                 (descriptor->submission.optname <= TLS_RX)))
    {
      // linux/tls.h: TLS_TX = 1, TLS_RX = 2
      engine->flags &= ~(1 << descriptor->submission.optname);
      CallHandlerFunction(engine, POLLERR, -completion->res);
      goto Failure;
    }

    if (likely((descriptor->submission.level   == SOL_TLS) &&
               (descriptor->submission.optname == TLS_RX)  &&
               (~engine->flags & FASTBIO_FLAG_KTLS_RECEIVE)))
    {
      if (likely(buffer = AllocateFastBuffer(engine->inbound.pool, 0, 0)))
      {
        buffer->reserved = EAGAIN;
        AppendInboundQueue(engine, buffer);
        CallHandlerFunction(engine, POLLIN, engine->inbound.length);
        goto Continue;
      }

      CallHandlerFunction(engine, POLLERR, ENOMEM);
      goto Failure;
    }
  }

  Continue:

  if ((~descriptor->submission.flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) &&
      ( engine->outbound.condition   & POLLOUT))
  {
    engine->outbound.condition &= ~POLLOUT;
    CallHandlerFunction(engine, POLLOUT, 0);
  }

  Failure:

  ReleaseEngine(engine, reason);
  return 0;
}

static int HandleTouchCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastBIO* engine;

  engine = (struct FastBIO*)descriptor->closure;
  engine->outbound.condition &= ~POLLIN;

  CallHandlerFunction(engine, 0, 0);
  ReleaseEngine(engine, reason);

  return 0;
}

static void FlushOutboundQueue(void* closure, int reason)
{
  struct FastBIO* engine;

  engine = (struct FastBIO*)closure;

  SubmitFastRingDescriptorRange(engine->outbound.tail, engine->outbound.head);

  engine->outbound.condition |= POLLOUT;
  engine->outbound.count      = 0;
  engine->outbound.tail       = NULL;
  engine->outbound.head       = NULL;
}

// kTLS

#define BIO_CTRL_SET_KTLS                       72
#define BIO_CTRL_SET_KTLS_TX_SEND_CTRL_MSG      74
#define BIO_CTRL_CLEAR_KTLS_TX_CTRL_MSG         75
#define BIO_CTRL_SET_KTLS_TX_ZEROCOPY_SENDFILE  90

struct TLSSyntheticHeader  // kTLS RX strips the record header, OpenSSL still expects record framing
{
  uint8_t type;
  uint8_t version[2];
  uint16_t length;
} __attribute__((packed));

static const char option[] = "tls";

static int CheckKernelTLS()
{
  int handle;
  int result;

  result = -1;
  handle = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);

  if (handle >= 0)
  {
    result = setsockopt(handle, SOL_TCP, TCP_ULP, option, sizeof(option));

    if ((result < 0) &&
        ((errno == ENOTCONN) ||
         (errno == EEXIST)   ||
         (errno == EALREADY)))
    {
      // ULP exists, dummy socket is not connected
      result = 0;
    }

    close(handle);
  }

  return !result;
}

static socklen_t GetKernelTLSLength(struct tls_crypto_info* information)
{
  if (information != NULL)
  {
    switch (information->cipher_type)
    {
      case TLS_CIPHER_AES_CCM_128:        return sizeof(struct tls12_crypto_info_aes_ccm_128);
      case TLS_CIPHER_AES_GCM_128:        return sizeof(struct tls12_crypto_info_aes_gcm_128);
      case TLS_CIPHER_AES_GCM_256:        return sizeof(struct tls12_crypto_info_aes_gcm_256);
      case TLS_CIPHER_CHACHA20_POLY1305:  return sizeof(struct tls12_crypto_info_chacha20_poly1305);
      case TLS_CIPHER_SM4_CCM:            return sizeof(struct tls12_crypto_info_sm4_ccm);
      case TLS_CIPHER_SM4_GCM:            return sizeof(struct tls12_crypto_info_sm4_gcm);
      case TLS_CIPHER_ARIA_GCM_128:       return sizeof(struct tls12_crypto_info_aria_gcm_128);
      case TLS_CIPHER_ARIA_GCM_256:       return sizeof(struct tls12_crypto_info_aria_gcm_256);
    }
  }

  return 0;
}

static int SetKernelTLS(struct FastBIO* engine, int direction, struct tls_crypto_info* information)
{
  socklen_t length;
  struct FastRingDescriptor* descriptors[2];

  descriptors[0] = NULL;
  descriptors[1] = NULL;

  if (unlikely(!(length         = GetKernelTLSLength(information)) ||
               !(descriptors[0] = AllocateFastRingDescriptor(engine->ring, HandleOptionCompletion, engine)) ||
               !(descriptors[1] = AllocateFastRingDescriptor(engine->ring, HandleOptionCompletion, engine))))
  {
    engine->flags &= ~(FASTBIO_FLAG_KTLS_AVAILABLE | FASTBIO_FLAG_KTLS_SEND | FASTBIO_FLAG_KTLS_RECEIVE);
    ReleaseFastRingDescriptor(descriptors[0]);
    ReleaseFastRingDescriptor(descriptors[1]);
    return 0;
  }

  memcpy(&descriptors[1]->data, information, length);
  io_uring_prep_cmd_sock(&descriptors[0]->submission, SOCKET_URING_OP_SETSOCKOPT, engine->handle, SOL_TCP, TCP_ULP, (void*)option, sizeof(option));
  io_uring_prep_cmd_sock(&descriptors[1]->submission, SOCKET_URING_OP_SETSOCKOPT, engine->handle, SOL_TLS, TLS_RX - !!direction, &descriptors[1]->data, length);
  PrepareFastRingDescriptor(descriptors[0], 0);
  PrepareFastRingDescriptor(descriptors[1], 0);

  descriptors[0]->submission.flags |= IOSQE_IO_HARDLINK;
  descriptors[0]->next              = descriptors[1];
  descriptors[0]->linked            = 2;

  engine->count          += descriptors[0]->linked;
  engine->outbound.count += descriptors[0]->linked;

  if (engine->outbound.tail == NULL)
  {
    engine->outbound.tail = descriptors[0];
    engine->outbound.head = descriptors[1];
    SetFastRingFlushHandler(engine->ring, FlushOutboundQueue, engine);
  }
  else
  {
    engine->outbound.tail->linked            = engine->outbound.count;
    engine->outbound.head->submission.flags |= IOSQE_IO_HARDLINK;
    engine->outbound.head->next              = descriptors[0];
    engine->outbound.head                    = descriptors[1];
  }

  engine->flags |= FASTBIO_FLAG_KTLS_SEND   * !!direction;  // TX: following records must use kTLS send semantics
  engine->flags |= FASTBIO_FLAG_KTLS_FORMAT *  !direction;  // RX: OpenSSL expects ktls_read_record-compatible BIO_read format

  return 1;
}

// OpenSSL BIO

static int HandleBIORead(BIO* handle, char* destination, int size)
{
  int length;
  char* start;
  char* source;
  struct FastBIO* engine;
  struct FastBuffer* buffer;
  struct TLSSyntheticHeader* header;
  struct FastRingDescriptor* descriptor;
  struct io_uring_recvmsg_out* output;
  struct cmsghdr* control;
  struct msghdr* message;

  engine     = (struct FastBIO*)BIO_get_data(handle);
  descriptor = engine->inbound.descriptor;

  if (unlikely((engine->flags & FASTBIO_FLAG_KTLS_FORMAT) &&
               (size < SSL3_RT_HEADER_LENGTH)))
  {
    errno = EINVAL;
    return -1;
  }

  if (unlikely((descriptor == NULL) ||
               (descriptor->submission.opcode != IORING_OP_RECVMSG)))
  {
    BIO_clear_retry_flags(handle);
    errno = EBADF;
    return -1;
  }

  if (unlikely((buffer = engine->inbound.tail) &&
               (buffer->length == 0)))
  {
    errno                 = buffer->reserved;
    engine->flags        |= FASTBIO_FLAG_KTLS_RECEIVE * (buffer->reserved == EAGAIN);
    engine->inbound.tail  = buffer->next;
    ReleaseFastBuffer(buffer);
    BIO_set_retry_read(handle);
    return -1;
  }

  if (unlikely(engine->inbound.length == 0))
  {
    BIO_set_retry_read(handle);
    errno = EAGAIN;
    return -1;
  }

  message = &descriptor->data.socket.message;
  start   = destination;

  // kTLS record format

  if (unlikely(engine->flags & FASTBIO_FLAG_KTLS_FORMAT))
  {
    if (unlikely(~engine->flags & FASTBIO_FLAG_KTLS_RECEIVE))
    {
      if ((~engine->flags & FASTBIO_FLAG_KTLS_IGNORE) &&
          (engine->inbound.tail         != NULL)      &&
          (engine->inbound.tail->length != 0))
      {
        BIO_clear_retry_flags(handle);
        errno = EPROTO;
        return -1;
      }

      while ((buffer = engine->inbound.tail) &&
             (buffer->length != 0))
      {
        output = io_uring_recvmsg_validate(buffer->data, buffer->length, message);
        length = io_uring_recvmsg_payload_length(output, buffer->length, message);

        engine->inbound.position  = 0;
        engine->inbound.length   -= length;
        engine->inbound.tail      = buffer->next;
        ReleaseFastBuffer(buffer);
      }

      BIO_set_retry_read(handle);
      errno = EAGAIN;
      return -1;
    }

    buffer = engine->inbound.tail;
    output = io_uring_recvmsg_validate(buffer->data, buffer->length, message);
    length = io_uring_recvmsg_payload_length(output, buffer->length, message);
    source = (char*)io_uring_recvmsg_payload(output, message);

    if (unlikely(((output->flags & (MSG_EOR | MSG_CTRUNC)) != MSG_EOR) ||
                 ((length + SSL3_RT_HEADER_LENGTH) > size)))
    {
      errno = EMSGSIZE;
      return -1;
    }

    header       = (struct TLSSyntheticHeader*)destination;
    destination += SSL3_RT_HEADER_LENGTH;

    memset(header, 0, SSL3_RT_HEADER_LENGTH);
    memcpy(destination, source, length);

    if ((control = io_uring_recvmsg_cmsg_firsthdr(output, message)) &&
        (control->cmsg_level == SOL_TLS) &&
        (control->cmsg_type  == TLS_GET_RECORD_TYPE))
    {
      header->type       = *(uint8_t*)CMSG_DATA(control);
      header->version[0] = TLS1_2_VERSION_MAJOR;
      header->version[1] = TLS1_2_VERSION_MINOR;
      header->length     = htobe16(length);
    }

    engine->inbound.length -= length;
    engine->inbound.tail    = buffer->next;
    ReleaseFastBuffer(buffer);

    // This follows the original ktls_read_record(...) behavior 1:1:
    // https://github.com/openssl/openssl/blob/e483d93b393bec71327c3b31779e8dcf37b3f42b/include/internal/ktls.h#L400
    size = length + SSL3_RT_HEADER_LENGTH * (header->type != 0);

    BIO_clear_retry_flags(handle);
    return size;
  }

  // TLS stream format

  if (size > engine->inbound.length)
  {
    // Limit payload copy to available bytes
    size = engine->inbound.length;
  }

  while ((buffer = engine->inbound.tail) &&
         (buffer->length != 0) &&
         (size > 0))
  {
    output  = io_uring_recvmsg_validate(buffer->data, buffer->length, message);
    length  = io_uring_recvmsg_payload_length(output, buffer->length, message);
    source  = (char*)io_uring_recvmsg_payload(output, message);
    length -= engine->inbound.position;
    source += engine->inbound.position;

    if (size < length)
    {
      memcpy(destination, source, size);
      destination += size;

      engine->inbound.position += size;
      engine->inbound.length   -= size;
      break;
    }

    memcpy(destination, source, length);
    destination += length;
    size        -= length;

    engine->inbound.position  = 0;
    engine->inbound.length   -= length;
    engine->inbound.tail      = buffer->next;
    ReleaseFastBuffer(buffer);
  }

  size = destination - start;

  BIO_clear_retry_flags(handle);
  return size;
}

static int HandleBIOWrite(BIO* handle, const char* data, int length)
{
  uint32_t size;
  uint32_t value;
  struct FastBIO* engine;
  struct cmsghdr* control;
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;

  engine = (struct FastBIO*)BIO_get_data(handle);

  if ((~engine->flags & FASTBIO_FLAG_KTLS_SEND) &&
      (descriptor = engine->outbound.head)      &&
      (descriptor->function == HandleOutboundCompletion))
  {
    buffer = FAST_BUFFER(descriptor->data.socket.vector.iov_base);
    size   = descriptor->data.socket.vector.iov_len + length;

    if (size <= buffer->size)
    {
      memcpy(buffer->data + descriptor->data.socket.vector.iov_len, data, length);
      descriptor->data.socket.vector.iov_len = size;
      return length;
    }
  }

  if (unlikely(engine->outbound.condition & POLLOUT))
  {
    BIO_set_retry_write(handle);
    errno = EAGAIN;
    return -1;
  }

  if ((length > SSL3_RT_MAX_PLAIN_LENGTH) &&
      (engine->flags & FASTBIO_FLAG_KTLS_SEND) &&
      (engine->types[0] == 0))
  {
    // Keep kTLS TX FastBuffer allocation within TLS record payload size
    length = SSL3_RT_MAX_PLAIN_LENGTH;
  }

  size        = length + (__BIGGEST_ALIGNMENT__ + CMSG_SPACE(sizeof(uint8_t)) - 1) * (engine->types[0] != 0);
  value       = size % engine->outbound.granularity;
  size       += (engine->outbound.granularity - value) * (value != 0);
  buffer      = AllocateFastBuffer(engine->outbound.pool, size, 0);
  descriptor  = AllocateFastRingDescriptor(engine->ring, HandleOutboundCompletion, engine);

  if (unlikely((buffer     == NULL) ||
               (descriptor == NULL)))
  {
    ReleaseFastBuffer(buffer);
    ReleaseFastRingDescriptor(descriptor);
    BIO_set_retry_write(handle);
    errno = ENOMEM;
    return -1;
  }

  memcpy(buffer->data, data, length);
  memset(&descriptor->data.socket.message, 0, sizeof(struct msghdr));

  descriptor->data.socket.number             = 0ULL;
  descriptor->data.socket.vector.iov_base    = buffer->data;
  descriptor->data.socket.vector.iov_len     = length;
  descriptor->data.socket.message.msg_iov    = &descriptor->data.socket.vector;
  descriptor->data.socket.message.msg_iovlen = 1;

  if (unlikely(engine->types[0] != 0))
  {
    value = (length + __BIGGEST_ALIGNMENT__ - 1) & ~(__BIGGEST_ALIGNMENT__ - 1);
    descriptor->data.socket.message.msg_control    = buffer->data + value;
    descriptor->data.socket.message.msg_controllen = CMSG_SPACE(sizeof(uint8_t));

    memset(descriptor->data.socket.message.msg_control, 0, descriptor->data.socket.message.msg_controllen);

    control             = CMSG_FIRSTHDR(&descriptor->data.socket.message);
    control->cmsg_level = SOL_TLS;
    control->cmsg_type  = TLS_SET_RECORD_TYPE;
    control->cmsg_len   = CMSG_LEN(sizeof(uint8_t));

    *(uint8_t*)CMSG_DATA(control) = engine->types[0];
    engine->types[0]              = 0;

    descriptor->data.socket.message.msg_controllen = control->cmsg_len;
  }

  io_uring_prep_sendmsg_zc(&descriptor->submission, engine->handle, &descriptor->data.socket.message, 0);

  // Once TLS ULP is intended for this socket, do not use SENDMSG_ZC at all
  descriptor->submission.opcode -= (IORING_OP_SENDMSG_ZC - IORING_OP_SENDMSG) * !!(engine->flags & FASTBIO_FLAG_KTLS_AVAILABLE);
  descriptor->submission.ioprio |= IORING_RECVSEND_POLL_FIRST;

  PrepareFastRingDescriptor(descriptor, 0);

  engine->outbound.count ++;
  engine->count          ++;

  if (unlikely(engine->outbound.tail == NULL))
  {
    engine->outbound.tail = descriptor;
    engine->outbound.head = descriptor;
    SetFastRingFlushHandler(engine->ring, FlushOutboundQueue, engine);
  }
  else
  {
    if (likely((~engine->flags & FASTBIO_FLAG_KTLS_SEND) &&
               (engine->outbound.head->function == HandleOutboundCompletion)))
    {
      // Outbound queue can contain non-send descriptors
      // MSG_MORE is used only for the userspace TLS byte-stream path to improve TCP batching
      // Do not set it for kTLS TX: TLS ULP owns recordization/flush semantics
      engine->outbound.head->submission.msg_flags = MSG_MORE;
    }

    engine->outbound.tail->linked            = engine->outbound.count;
    engine->outbound.head->submission.flags |= IOSQE_IO_LINK;
    engine->outbound.head->next              = descriptor;
    engine->outbound.head                    = descriptor;
  }

  // Hold user-space transmission unless send completed
  engine->outbound.condition |= POLLOUT * (engine->outbound.count >= engine->outbound.limit);

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

    case BIO_CTRL_SET_KTLS:
      return ( engine->flags & FASTBIO_FLAG_KTLS_AVAILABLE)     &&
             ((engine->flags & FASTBIO_FLAG_KTLS_BIDIRECTIONAL) ||
              (argument1 != 0)) &&
             (SetKernelTLS(engine, argument1, (struct tls_crypto_info*)argument2));

    case BIO_CTRL_SET_KTLS_TX_SEND_CTRL_MSG:
      engine->types[0] = argument1;
      return 1;

    case BIO_CTRL_CLEAR_KTLS_TX_CTRL_MSG:
      engine->types[0] = 0;
      return 1;

    case BIO_CTRL_GET_KTLS_SEND:
      return !!(engine->flags & FASTBIO_FLAG_KTLS_SEND);

    case BIO_CTRL_GET_KTLS_RECV:
      return !!(engine->flags & FASTBIO_FLAG_KTLS_RECEIVE);

    case FASTBIO_CTRL_TOUCH:
      if (( engine->count >= 2) &&
          (~engine->outbound.condition & POLLIN) &&
          (descriptor = AllocateFastRingDescriptor(engine->ring, HandleTouchCompletion, engine)))
      {
        engine->count ++;
        engine->outbound.condition |= POLLIN;
        io_uring_prep_nop(&descriptor->submission);
        SubmitFastRingDescriptor(descriptor, 0);
      }
      return 0;
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

  if (engine = (struct FastBIO*)BIO_get_data(handle))
  {
    if (engine->inbound.condition & IORING_CQE_F_MORE)
    {
      // Preventing leakage in destruction inside a call HandleInboundCompletion() when last FastBuffer has been received
      engine->inbound.descriptor = NULL;
    }

    if ((descriptor = engine->inbound.descriptor) &&
        (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_PENDING))
    {
      // io_uring_prep_recvmsg_multishot() is waiting for submission
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_nop(&descriptor->submission);
      PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      engine->inbound.descriptor = NULL;
      engine->count --;
    }

    if (descriptor = engine->inbound.descriptor)
    {
      // io_uring_prep_recvmsg_multishot() is already in the io_uring
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      engine->inbound.descriptor = NULL;
    }

    engine->closure  = NULL;
    engine->function = NULL;

    ReleaseEngine(engine, -1);
    BIO_set_shutdown(handle, BIO_CLOSE);
  }

  return 1;
}

// API

static uint32_t flags = FASTBIO_FLAG_KTLS_ONCE;
static BIO_METHOD* method = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void __attribute__((destructor)) Finalize()
{
  if (method != NULL)
  {
    BIO_meth_free(method);
    method = NULL;
  }
}

BIO* CreateFastBIO(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, int handle, uint64_t options, uint32_t granularity, uint32_t limit, HandleFastBIOEvent function, void* closure)
{
  BIO* instance;
  struct FastBIO* engine;
  struct FastRingDescriptor* descriptor;

  pthread_mutex_lock(&lock);

  if ((method == NULL) &&
      (method =  BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "FastBIO")))
  {
    BIO_meth_set_write(method, HandleBIOWrite);
    BIO_meth_set_read(method, HandleBIORead);
    BIO_meth_set_ctrl(method, HandleBIOControl);
    BIO_meth_set_create(method, HandleBIOCreate);
    BIO_meth_set_destroy(method, HandleBIODestroy);
  }

  if ((options & SSL_OP_ENABLE_KTLS) &&
      (flags   & FASTBIO_FLAG_KTLS_ONCE))
  {
    flags &= ~FASTBIO_FLAG_KTLS_ONCE;
    flags |=  FASTBIO_FLAG_KTLS_AVAILABLE * CheckKernelTLS();
  }

  pthread_mutex_unlock(&lock);

  instance   = NULL;
  engine     = NULL;
  descriptor = NULL;

  if ((method != NULL) &&
      (instance   = BIO_new(method)) &&
      (engine     = (struct FastBIO*)calloc(1, sizeof(struct FastBIO))) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleInboundCompletion, engine)))
  {
    memset(&descriptor->data.socket.message, 0, sizeof(struct msghdr));

    engine->ring                 = ring;
    engine->flags                = flags;
    engine->handle               = handle;
    engine->closure              = closure;
    engine->function             = function;
    engine->count                = 2;  // IORING_OP_RECV + BIO
    engine->inbound.descriptor   = descriptor;
    engine->inbound.provider     = provider;
    engine->inbound.pool         = inbound;
    engine->outbound.granularity = granularity;
    engine->outbound.limit       = ring->ring.cq.ring_entries / 2;
    engine->outbound.pool        = outbound;

    descriptor->data.socket.message.msg_controllen = CMSG_SPACE(sizeof(uint8_t));

    if ((limit > 0) &&
        (limit < engine->outbound.limit))
    {
      // Use pre-defined limit for IORING_OP_SENDMSG SQEs
      engine->outbound.limit = limit;
    }

    if (~options & SSL_OP_ENABLE_KTLS)
    {
      // Disable kTLS for this BIO when zero-copy must remain available
      engine->flags &= ~FASTBIO_FLAG_KTLS_AVAILABLE;
    }

    io_uring_prep_recvmsg_multishot(&descriptor->submission, handle, &descriptor->data.socket.message, 0);
    PrepareFastRingBuffer(provider, &descriptor->submission);
    SubmitFastRingDescriptor(descriptor, 0);

    BIO_set_data(instance, engine);
    return instance;
  }

  ReleaseFastRingDescriptor(descriptor);
  BIO_free(instance);
  free(engine);
  return NULL;
}
