# SSLSocket API Reference

Header: `Ring/SSLSocket.h`

`SSLSocket` is a TLS session wrapper on top of `FastBIO`. It owns an OpenSSL `SSL*`
whose BIO is a `FastBIO`, drives the handshake automatically, and reports session
events to the application.

The division of labour is deliberate: the module manages the connection state machine
and the outbound buffer, but **it never reads plaintext for you**. On
`SSL_EVENT_RECEIVED` the application calls `SSL_read()` itself, which is what keeps
the data path copy-free and lets the application decide where bytes land.

## Roles and Events

Roles: `SSL_ROLE_SERVER`, `SSL_ROLE_CLIENT`.

Events, with the meaning of `parameter1` and `parameter2`:

| Event | Meaning | `parameter1` | `parameter2` |
| --- | --- | --- | --- |
| `SSL_EVENT_FAILED` (0) | Handshake or I/O failure | OpenSSL error code | `NULL` |
| `SSL_EVENT_GREETED` (1) | Peer certificate verification | verify condition | `X509_STORE_CTX*` |
| `SSL_EVENT_DRAINED` (2) | Send queue empty, ready for more data | `0` | `NULL` |
| `SSL_EVENT_RECEIVED` (3) | Plaintext available to read | `0` | `NULL` |
| `SSL_EVENT_CONNECTED` (4) | Handshake completed | `0` | `NULL` |
| `SSL_EVENT_DISCONNECTED` (5) | Connection closed | errno or `0` | `NULL` |

### The callback return value is an OpenSSL result

This is the central contract of the module and it is easy to get wrong:

- For **`SSL_EVENT_RECEIVED`**, return the value of the last `SSL_read()` you
  performed. It is fed straight into `SSL_get_error()`, and that is how the module
  learns whether to wait for more data (`SSL_ERROR_WANT_READ`), to report a failure
  (`SSL_EVENT_FAILED`), or to observe a clean shutdown (`SSL_ERROR_ZERO_RETURN` →
  `SSL_EVENT_DISCONNECTED`). Returning `0` from a handler that did read data makes the
  module treat the session as closed.
- For **`SSL_EVENT_GREETED`**, return the verify result: `1` to accept the
  certificate, `0` to reject. It is delivered from the OpenSSL verify callback and the
  value goes back to OpenSSL unchanged.
- All other events ignore the return value; return `0`.

`IterateSSLSocketData()` exists precisely to produce the right value — its `result`
argument ends up holding the last `SSL_read()` return, ready to be returned.

## Options

The `option` argument of `CreateSSLSocket()` carries two independent masks:

- `SSL_OPTION_VERIFY_MASK` (`SSL_VERIFY_PEER`, `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`,
  `SSL_VERIFY_CLIENT_ONCE`) is passed to `SSL_set_verify()`. When any bit is set, the
  verify callback is installed and `SSL_EVENT_GREETED` is delivered per certificate.
- `SSL_OPTION_OP_MASK` (currently `SSL_OP_ENABLE_KTLS`) is passed to
  `SSL_set_options()` and forwarded to `CreateFastBIO()` as its `options` argument.

## Reading Plaintext

```c
IterateSSLSocketData(connection, buffer, size, length, result, code)
```

Convenience macro that drains an `SSL*` into `buffer` in a loop: it calls
`SSL_read()`, advances `length` by the number of bytes read, executes `code`, and
repeats while the last read succeeded and `SSL_pending()` is non-zero. `result`
receives the last `SSL_read()` return value. Intended for use inside an
`SSL_EVENT_RECEIVED` handler.

## Writing Plaintext

```c
int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length);
```

`TransmitSSLSocketData()` never fails on back-pressure. Anything OpenSSL cannot take
right now — because the handshake has not finished, or because the transport is full —
is appended to a growable buffer owned by the socket and flushed later, so a caller may
write before `SSL_EVENT_CONNECTED` and simply not think about readiness.

- returns a positive value when the data was handed to OpenSSL;
- returns `-1` only on allocation failure;
- may be called from inside an event handler; the module suppresses the redundant
  transport poke in that case.

A partial `SSL_write()` is retried with **the same pointer and the same length**, as
OpenSSL requires; the socket is created with `SSL_MODE_ENABLE_PARTIAL_WRITE` and
`SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` so the retry may come from a relocated buffer.

`SSL_EVENT_DRAINED` is delivered when the internal buffer and the transport queue are
both empty — the signal to produce more if you are streaming.

## Lifecycle

```c
struct SSLSocket* CreateSSLSocket(...);
void ReleaseSSLSocket(struct SSLSocket* socket);
```

`CreateSSLSocket()` builds the `FastBIO`, creates the `SSL*` from `context`, and sets
`SSL_OP_IGNORE_UNEXPECTED_EOF` plus read-ahead. `role` decides which side of the
handshake is driven — `SSL_ROLE_SERVER` calls `SSL_accept()`, `SSL_ROLE_CLIENT` calls
`SSL_connect()` — and it starts on its own as soon as the transport reports progress.
No explicit handshake call exists; wait for `SSL_EVENT_CONNECTED`.

`ReleaseSSLSocket()` performs `SSL_shutdown()` and frees everything. **Calling it from
inside an event handler is safe**: the module records the request and completes the
teardown once the handler returns. After the call, treat the pointer as dead and stop
using the `SSL*` — including from a pending verify callback.

## Internal State Flags

`socket->state` uses `poll` constants: `SSL_FLAG_ENTER` (`POLLPRI`, handler is on the
stack), `SSL_FLAG_READ` (`POLLIN`), `SSL_FLAG_WRITE` (`POLLOUT`), `SSL_FLAG_REMOVE`
(`POLLERR`, release requested), `SSL_FLAG_ACTIVE` (`POLLHUP`, connection established).
`ReleaseSSLSocket()` called from inside a handler is deferred through `SSL_FLAG_REMOVE`
until the handler returns.

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

