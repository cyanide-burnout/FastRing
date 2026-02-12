# SSLSocket API Reference

Header: `Ring/SSLSocket.h`

`SSLSocket` is a TLS session wrapper on top of `FastBIO`.

## Roles and Events

- Roles: `SSL_ROLE_SERVER`, `SSL_ROLE_CLIENT`
- Events:
  - `SSL_EVENT_FAILED`
  - `SSL_EVENT_GREETED`
  - `SSL_EVENT_RECEIVED`
  - `SSL_EVENT_CONNECTED`
  - `SSL_EVENT_DISCONNECTED`

Callback:

```c
typedef int (*HandleSSLSocketEventFunction)(
  void* closure,
  SSL* connection,
  int event,
  int parameter1,
  void* parameter2);
```

## API

```c
struct SSLSocket* CreateSSLSocket(
  struct FastRing* ring,
  struct FastRingBufferProvider* provider,
  struct FastBufferPool* inbound,
  struct FastBufferPool* outbound,
  SSL_CTX* context,
  int handle,
  int role,
  int option,
  uint32_t granularity,
  uint32_t limit,
  HandleSSLSocketEventFunction function,
  void* closure);

int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length);
void ReleaseSSLSocket(struct SSLSocket* socket);
```

