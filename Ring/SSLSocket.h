#ifndef SSLSOCKET_H
#define SSLSOCKET_H

#include <stddef.h>
#include <stdint.h>

#include <openssl/ssl.h>

#include "FastBIO.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SSL_ROLE_SERVER  SSL_ERROR_WANT_ACCEPT
#define SSL_ROLE_CLIENT  SSL_ERROR_WANT_CONNECT

#define SSL_FLAG_ENTER   POLLPRI  // 
#define SSL_FLAG_READ    POLLIN   //  
#define SSL_FLAG_WRITE   POLLOUT  // POLLOUT requested
#define SSL_FLAG_REMOVE  POLLERR  // Connection removed
#define SSL_FLAG_ACTIVE  POLLHUP  // Connection established

#define SSL_EVENT_FAILED        0
#define SSL_EVENT_GREETED       1
#define SSL_EVENT_RECEIVED      2
#define SSL_EVENT_CONNECTED     3
#define SSL_EVENT_DISCONNECTED  4

typedef int (*HandleSSLSocketEventFunction)(void* closure, SSL* connection, int event, int parameter1, void* parameter2);

struct SSLSocket
{
  HandleSSLSocketEventFunction function;
  void* closure;

  SSL* connection;
  int role;

  uint32_t state;         // SSL_FLAG_*

  size_t size;            // Length of buffer          |
  size_t batch;           // Length of pending data    | Outbound data buffer
  size_t length;          // Length of available data  |
  uint8_t* buffer;        // Operational buffer        |
};

struct SSLSocket* CreateSSLSocket(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, SSL_CTX* context, int handle, int role, int option, uint32_t granularity, uint32_t limit, HandleSSLSocketEventFunction function, void* closure);
int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length);
void ReleaseSSLSocket(struct SSLSocket* socket);

#ifdef __cplusplus
}
#endif

#endif
