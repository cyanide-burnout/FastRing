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
| `SSL_EVENT_FAILED` (0) | Handshake or I/O failure | First entry of the OpenSSL error queue (`ERR_get_error()`, may be `0`), truncated to `int` | `NULL` |
| `SSL_EVENT_GREETED` (1) | Peer certificate verification | verify condition | `X509_STORE_CTX*` |
| `SSL_EVENT_DRAINED` (2) | Send queue empty, ready for more data | `0` | `NULL` |
| `SSL_EVENT_RECEIVED` (3) | Plaintext available to read | `0` | `NULL` |
| `SSL_EVENT_CONNECTED` (4) | Handshake completed | `0` | `NULL` |
| `SSL_EVENT_DISCONNECTED` (5) | Transport error or close (a FastBIO `POLLERR` or `POLLHUP`), or `close_notify` received | errno, or `0` | `NULL` |

Neither `SSL_EVENT_FAILED` nor `SSL_EVENT_DISCONNECTED` releases anything; the owner calls
`ReleaseSSLSocket()`. Data written after them is buffered and never sent.

### The callback return value is an OpenSSL result

This is the central contract of the module and it is easy to get wrong:

- For **`SSL_EVENT_RECEIVED`**, return the value of the last `SSL_read()` you
  performed. It is fed straight into `SSL_get_error()`, and that is how the module
  learns whether to wait for more data (`SSL_ERROR_WANT_READ`), to report a failure
  (`SSL_EVENT_FAILED`), or to observe a clean shutdown (`SSL_ERROR_ZERO_RETURN` →
  `SSL_EVENT_DISCONNECTED`). Returning `0` from a handler that did read data makes the
  module report a failure (`SSL_get_error()` gives `SSL_ERROR_SYSCALL`), or a disconnect
  when `close_notify` has arrived.
- The handler must read until `SSL_read()` stops returning data: `SSL_EVENT_RECEIVED` is
  delivered again, within the same FastBIO event, for as long as ciphertext stays queued, so a
  handler that does not drain makes the ring thread spin.
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
`SSL_EVENT_RECEIVED` handler. `code` must consume the data and reset `length`: once
`buffer` is full, `SSL_read()` is called with a size of `0` and returns `0`.

## Writing Plaintext

```c
int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length);
```

`TransmitSSLSocketData()` does not lose data on back-pressure. Anything OpenSSL cannot
take right now — because the handshake has not finished, because earlier data is still
buffered, or because the transport is full — is appended to a growable buffer owned by the
socket and flushed later, so a caller may write before `SSL_EVENT_CONNECTED` and simply not
think about readiness.

The return value is that of the last `SSL_write()`, so a negative value does not by itself
mean that the data was lost:

- `0` — the data was only buffered (data pending already, or the session not active yet);
- `> 0` — everything went to OpenSSL; the value is the result of the last `SSL_write()`,
  not the total;
- `<= 0` from `SSL_write()`, normally `-1` — OpenSSL took part of the data or none and the
  rest was buffered. This is the usual back-pressure result. On a fatal error the data is
  buffered as well, and `SSL_EVENT_FAILED` or `SSL_EVENT_DISCONNECTED` is delivered;
- `-1` with `errno = ENOMEM` — the buffer could not grow, and the data was not taken.

It may be called from inside an event handler; the module suppresses the redundant transport
poke in that case.

A failed `SSL_write()` is repeated with **the same length**, as OpenSSL requires; the socket
is created with `SSL_MODE_ENABLE_PARTIAL_WRITE` and `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`, so
the retry may come from a relocated buffer and partial successes need no retry.

`SSL_EVENT_DRAINED` is delivered after FastBIO reports its batch in flight accepted, with no
plaintext buffered and nothing queued in FastBIO — the signal to produce more if you are
streaming. Zero-copy sends may still be waiting for their notifications.

## Lifecycle

```c
struct SSLSocket* CreateSSLSocket(...);
void ReleaseSSLSocket(struct SSLSocket* socket);
```

`CreateSSLSocket()` builds the `FastBIO`, creates the `SSL*` from `context`, and sets
`SSL_MODE_ENABLE_PARTIAL_WRITE`, `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`,
`SSL_OP_IGNORE_UNEXPECTED_EOF`, the `SSL_OPTION_OP_MASK` bits of `option`, and read-ahead.
`role` decides which side of the handshake is driven — `SSL_ROLE_SERVER` calls
`SSL_accept()`, `SSL_ROLE_CLIENT` calls `SSL_connect()` — and it starts from a
`FASTBIO_CTRL_TOUCH` queued at creation, on the next ring iteration. No explicit handshake
call exists; wait for `SSL_EVENT_CONNECTED`, which also switches FastBIO to asynchronous
receive.

On success `handle` belongs to the socket. On `NULL` it depends on where creation stopped:
when the object or the FastBIO could not be created, `handle` is still open; when `SSL_new()`
failed, the FastBIO has already been freed and closes `handle` itself.

`ReleaseSSLSocket()` detaches the handler, calls `SSL_shutdown()` (a `close_notify` is
queued if FastBIO accepts the write at that moment), `SSL_free()`, which frees the FastBIO,
and discards buffered plaintext. **Calling it from inside an event handler is safe**: the
module records the request and completes the teardown once the handler returns. After the
call, treat the pointer as dead and stop using the `SSL*` — including from a pending verify
callback.

## Ring Lifecycle

An `SSLSocket` creates nothing on the ring itself. It owns one `SSL*`, which owns one FastBIO
(installed as both read and write BIO), which owns `handle` and every ring operation; see
[FastBIO.md](FastBIO.md#lifecycle). The socket also owns a heap buffer for plaintext that
OpenSSL has not accepted yet. All session work, the `SSL_EVENT_*` callbacks included, runs in
the FastBIO event callback on the ring thread, and every call is made there too.

After `ReleaseSSLSocket()` the FastBIO engine lives on until its queued and in-flight
operations complete, then closes `handle`. Nothing reports that moment, so keep `provider`
and the pools until the loop has run those operations to completion.

If `ReleaseFastRing()` runs while the socket is alive, FastBIO delivers `POLLHUP`, which
arrives as `SSL_EVENT_DISCONNECTED` with `0`; releasing the socket from that callback is the
last safe point. A `ReleaseSSLSocket()` after `ReleaseFastRing()` frees a FastBIO whose ring is
gone. Release sockets before the ring.

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

