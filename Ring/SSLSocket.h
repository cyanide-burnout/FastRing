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

#define SSL_OPTION_VERIFY_MASK    (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE)  // 0x07
#define SSL_OPTION_OP_MASK        (SSL_OP_ENABLE_KTLS)                                                          // 0x08

#define SSL_FLAG_ENTER   POLLPRI  //
#define SSL_FLAG_READ    POLLIN   // Read requested
#define SSL_FLAG_WRITE   POLLOUT  // Write requested
#define SSL_FLAG_REMOVE  POLLERR  // Connection removed
#define SSL_FLAG_ACTIVE  POLLHUP  // Connection established

#define SSL_EVENT_FAILED        0
#define SSL_EVENT_GREETED       1
#define SSL_EVENT_DRAINED       2
#define SSL_EVENT_RECEIVED      3
#define SSL_EVENT_CONNECTED     4
#define SSL_EVENT_DISCONNECTED  5

#define IterateSSLSocketData(connection, buffer, size, length, result, code)    \
  do                                                                            \
  {                                                                             \
    result  = SSL_read(connection, (uint8_t*)buffer + length, size - length);   \
    length += (result > 0) * result;                                            \
    code;                                                                       \
  }                                                                             \
  while ((result > 0) &&                                                        \
         (SSL_pending(connection) > 0));

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
