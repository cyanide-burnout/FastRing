#define _GNU_SOURCE

#include "KCPAdapter.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/net_tstamp.h>

#define INBOUND_COUNT   2048
#define INBOUND_LENGTH  2048
#define CONTROL_LENGTH  256

static void StoreTimeDifference(struct KCPAdapter* adapter)
{
  struct timespec real;
  struct timespec monotonic;

  clock_gettime(CLOCK_REALTIME, &real);
  clock_gettime(CLOCK_MONOTONIC, &monotonic);

  adapter->delta.tv_sec  = real.tv_sec  - monotonic.tv_sec;
  adapter->delta.tv_nsec = real.tv_nsec - monotonic.tv_nsec;

  if (adapter->delta.tv_nsec < 0)
  {
    adapter->delta.tv_nsec += 1000000000L;
    adapter->delta.tv_sec  -= 1;
  }
}

static void ApplyTimeDifference(struct KCPAdapter* adapter, struct timespec* time)
{
  time->tv_sec  -= adapter->delta.tv_sec;
  time->tv_nsec -= adapter->delta.tv_nsec;

  if (time->tv_nsec < 0)
  {
    time->tv_nsec += 1000000000L;
    time->tv_sec  -= 1;
  }
}

static void FillServicePoint(struct KCPPoint* point, struct in6_addr* address)
{
  if (IN6_IS_ADDR_V4MAPPED(address))
  {
    point->family    = AF_INET;
    point->v4.s_addr = address->__in6_u.__u6_addr32[3];
  }
  else
  {
    point->family = AF_INET6;
    memcpy(&point->v6, address, sizeof(struct in6_addr));
  }
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  struct KCPAdapter* adapter;

  adapter = (struct KCPAdapter*)descriptor->closure;

  StoreTimeDifference(adapter);
  FlushKCPService(adapter->service, KCP_FLUSH_SEND | KCP_FLUSH_CLEANUP);
}

static void HandleSocketEvent(struct FastSocket* socket, int event, int parameter)
{
  int length;
  uint8_t* data;
  struct KCPPoint point;
  struct timespec* time;
  struct cmsghdr* control;
  struct sockaddr* address;
  struct FastBuffer* buffer;
  struct KCPAdapter* adapter;
  struct in6_pktinfo* information;
  struct io_uring_recvmsg_out* output;
  struct KCPConversation* conversation;
  const struct KCPFormat* format;

  adapter = (struct KCPAdapter*)socket->closure;

  while (buffer = ReceiveFastSocketBuffer(socket))
  {
    output       = io_uring_recvmsg_validate(buffer->data, buffer->length, &adapter->message);
    control      = io_uring_recvmsg_cmsg_firsthdr(output, &adapter->message);
    address      = (struct sockaddr*)io_uring_recvmsg_name(output);
    length       = io_uring_recvmsg_payload_length(output, buffer->length, &adapter->message);
    data         = (uint8_t*)io_uring_recvmsg_payload(output, &adapter->message);
    time         = NULL;
    point.family = AF_UNSPEC;

    while (control != NULL)
    {
      if ((control->cmsg_level == SOL_SOCKET) &&
          (control->cmsg_type  == SCM_TIMESTAMPING))
      {
        time = (struct timespec*)CMSG_DATA(control);
        ApplyTimeDifference(adapter, time);
      }

      if ((control->cmsg_level == IPPROTO_IPV6) &&
          (control->cmsg_type  == IPV6_PKTINFO))
      {
        information = (struct in6_pktinfo*)CMSG_DATA(control);
        FillServicePoint(&point, &information->ipi6_addr);
      }

      control = io_uring_recvmsg_cmsg_nexthdr(output, &adapter->message, control);
    }

    if ((adapter->validate == NULL) &&
        (format = adapter->format)  ||
        (format = adapter->validate(adapter, address, data, length)))
    {
      if (HandleKCPPacket(adapter->service, format, &conversation, time, address, data, length, &point, (ReleaseKCPClosure)ReleaseFastBuffer, buffer) >= 0)
      {
        // Flush outbound queue immedieatly
        FlushKCPConversation(conversation, &conversation->time);
      }

      continue;
    }

    ReleaseFastBuffer(buffer);
  }
}

static uint8_t* AllocateServicePacket(void* closure, uint32_t size)
{
  struct KCPAdapter* adapter;
  struct FastBuffer* buffer;

  adapter = (struct KCPAdapter*)closure;
  buffer  = AllocateFastBuffer(adapter->outbound, size, 0);

  return (buffer != NULL) ? buffer->data : NULL;
}

static int TransmitServicePacket(void* closure, struct sockaddr* address, uint8_t* data, uint32_t size)
{
  struct FastRingDescriptor* descriptor;
  struct KCPAdapter* adapter;
  struct FastSocket* socket;
  struct FastBuffer* buffer;

  adapter = (struct KCPAdapter*)closure;
  socket  = adapter->socket;
  buffer  = HoldFastBuffer(FAST_BUFFER(data));

  if (descriptor = AllocateFastRingDescriptor(socket->ring, NULL, NULL))
  {
    io_uring_prep_send_zc(&descriptor->submission, socket->handle, data, size, 0, 0);

    switch (address->sa_family)
    {
      case AF_INET:
        memcpy(&descriptor->data.socket.address, address, sizeof(struct sockaddr_in));
        io_uring_prep_send_set_addr(&descriptor->submission, (struct sockaddr*)&descriptor->data.socket.address, sizeof(struct sockaddr_in));
        break;

      case AF_INET6:
        memcpy(&descriptor->data.socket.address, address, sizeof(struct sockaddr_in6));
        io_uring_prep_send_set_addr(&descriptor->submission, (struct sockaddr*)&descriptor->data.socket.address, sizeof(struct sockaddr_in6));
        break;
    }
  }

  return TransmitFastSocketDescriptor(socket, descriptor, buffer);
}

void ReleaseKCPAdapter(struct KCPAdapter* adapter)
{
  if (adapter != NULL)
  {
    ReleaseFastSocket(adapter->socket);
    ReleaseFastRingBufferProvider(adapter->provider, ReleaseRingFastBuffer);
    ReleaseFastBufferPool(adapter->outbound);
    ReleaseFastBufferPool(adapter->inbound);
    SetFastRingTimeout(NULL, adapter->timeout, -1, 0, NULL, NULL);
    free(adapter);
  }
}

struct KCPAdapter* CreateKCPAdapter(struct FastRing* ring, struct KCPService* service, const struct KCPFormat* format, ValidateKCPPacket validate, void* closure, int port)
{
  struct KCPAdapter* adapter;
  struct sockaddr_in6 address;
  int handle;
  int value;

  adapter = NULL;

  if ((ring    != NULL) &&
      (service != NULL) &&
      ((format   != NULL)  ||
       (validate != NULL)) &&
      (adapter = (struct KCPAdapter*)calloc(1, sizeof(struct KCPAdapter))))
  {
    memset(&address, 0, sizeof(struct sockaddr_in6));

    handle = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    value  = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;

    setsockopt(handle, SOL_SOCKET, SO_TIMESTAMPING, &value, sizeof(int));

    address.sin6_family = AF_INET6;
    address.sin6_port   = htons(port);
    value               = 1;

    setsockopt(handle, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value, sizeof(int));

    if ((handle < 0) ||
        (setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) < 0) ||
        (bind(handle, (struct sockaddr*)&address, sizeof(struct sockaddr_in6)) < 0))
    {
      close(handle);
      free(adapter);
      return NULL;
    }

    adapter->message.msg_namelen    = sizeof(struct sockaddr_in6);
    adapter->message.msg_controllen = CONTROL_LENGTH;

    adapter->service              = service;
    adapter->transmitter.closure  = adapter;
    adapter->transmitter.allocate = AllocateServicePacket;
    adapter->transmitter.transmit = TransmitServicePacket;
    adapter->transmitter.release  = ReleaseRingFastBuffer;
    service->transmitter          = &adapter->transmitter;

    adapter->format   = format;
    adapter->closure  = closure;
    adapter->validate = validate;

    adapter->inbound  = CreateFastBufferPool(ring);
    adapter->outbound = CreateFastBufferPool(ring);
    adapter->provider = CreateFastRingBufferProvider(ring, 0, INBOUND_COUNT, INBOUND_LENGTH, AllocateRingFastBuffer, adapter->inbound);
    adapter->socket   = CreateFastSocket(ring, adapter->provider, adapter->inbound, adapter->outbound, handle, &adapter->message, 0, FASTSOCKET_MODE_ZERO_COPY, 0, HandleSocketEvent, adapter);
    adapter->timeout  = SetFastRingTimeout(ring, NULL, service->congestion.interval, TIMEOUT_FLAG_REPEAT, HandleTimeoutEvent, adapter);

    StoreTimeDifference(adapter);
  }

  return adapter;
}
