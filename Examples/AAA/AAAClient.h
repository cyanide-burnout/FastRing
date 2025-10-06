#ifndef AAACLIENT_H
#define AAACLIENT_H

#include "RADIUS.h"
#include "FastSocket.h"
#include "FastBuffer.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct AAAMessage
{
  int count;
  int length;
  void* client;

  const char* secret;
  struct sockaddr* address;

  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;
};

struct AAAClient
{
  int count;                        // Maximum retries count
  int interval;                     // Retry interval
  uint8_t identifier;               // Current message identifier
  struct msghdr message;

  struct FastRing* ring;
  struct FastSocket* socket;
  struct FastBufferPool* pool;
  struct FastRingBufferProvider* provider;

  struct AAAMessage messages[UINT8_MAX + 1];
};

uint8_t GetAAAClientMessageID(struct AAAClient* client);
void SubmitAAAClientMessage(struct AAAClient* client, struct FastBuffer* buffer, struct sockaddr* address, const char* secret);

struct AAAClient* CreateAAAClient(struct FastRing* ring, int count, int interval);
void ReleaseAAAClient(struct AAAClient* client);

#ifdef __cplusplus
}
#endif

#endif
