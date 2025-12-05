#ifndef KCPADAPTER_H
#define KCPADAPTER_H

#include "FastSocket.h"
#include "KCPService.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct KCPAdapter;

typedef const struct KCPFormat* (*ValidateKCPPacket)(struct KCPAdapter* adapter, struct sockaddr* address, uint8_t* data, int length);

struct KCPAdapter
{
  struct msghdr message;
  struct timespec delta;
  struct FastSocket* socket;
  struct FastBufferPool* inbound;
  struct FastBufferPool* outbound;
  struct FastRingDescriptor* timeout;
  struct FastRingBufferProvider* provider;

  struct KCPService* service;
  struct KCPTransmitter transmitter;
  const struct KCPFormat* format;

  ValidateKCPPacket validate;
  void* closure;
};

void ReleaseKCPAdapter(struct KCPAdapter* adapter);
struct KCPAdapter* CreateKCPAdapter(struct FastRing* ring, struct KCPService* service, const struct KCPFormat* format, ValidateKCPPacket validate, void* closure, int port);

#ifdef __cplusplus
}
#endif

#endif
