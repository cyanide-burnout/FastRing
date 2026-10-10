# LWSCore API Reference (Deprecated)

Header: `Ring/LWSCore.h`

`LWSCore` is the legacy WebSocket adapter based on `libwebsockets`.
In this repository it is deprecated; prefer `CURLWSCore`.

## Event Loop Binding

`LWSCore` binds to whichever loop adapter is compiled in, and exposes it under a
neutral set of names:

| Neutral name | GLib build | libuv build |
| --- | --- | --- |
| `struct LWSLoop` | `FastGLoop` | `FastUVLoop` |
| `CreateLWSLoop` | `CreateFastGLoop` | `CreateFastUVLoop` |
| `TouchLWSLoop` | `TouchFastGLoop` | `TouchFastUVLoop` |
| `StopLWSLoop` | `StopFastGLoop` | no-op |
| `ReleaseLWSLoop` | `ReleaseFastGLoop` | `ReleaseFastUVLoop` |

`USE_LWS_EVENTLOOP` selects the loop: `LWS_SERVER_OPTION_GLIB` (the default) or
`LWS_SERVER_OPTION_LIBUV`. It is also passed to libwebsockets as a context option.

## Core Options

The `option` argument of `CreateLWSCore()` packs two fields:

- `LWS_OPTION_MINIMAL_SSL_VERSION_MASK` (`0x0fff`) - the minimum TLS version, given as
  an OpenSSL version constant such as `SSL3_VERSION` or `TLS1_2_VERSION`.
- `LWS_OPTION_IGNORE_CERTIFICATE` (`1 << 12`) - skip peer certificate validation by
  installing a verify callback that accepts any chain.

Both fields are applied to the client `SSL_CTX` when libwebsockets creates it, which needs
an OpenSSL build of libwebsockets. `function` and `closure` of `CreateLWSCore()` are called
with that `SSL_CTX` at the same point. `depth` is the `smd_queue_depth` of the context, used
with libwebsockets 4.2 and newer.

Example: `CreateLWSCore(loop, LWS_OPTION_IGNORE_CERTIFICATE | TLS1_2_VERSION, ...)`.

## API

```c
void SetLWSReportHandler(int level, LWSReportFunction function);

struct LWSCore* CreateLWSCore(
  struct LWSLoop* loop,
  int option,
  int depth,
  LWSCreateFunction function,
  void* closure);

void ReleaseLWSCore(struct LWSCore* core);

struct LWSSession* CreateLWSSessionFromURL(
  struct LWSCore* core,
  const char* location,
  const char* protocols,
  LWSHandleFunction function,
  void* closure);

struct LWSSession* CreateLWSSessionFromAddress(
  struct LWSCore* core,
  struct sockaddr* address,
  int secure,
  const char* host,
  const char* path,
  const char* protocols,
  LWSHandleFunction function,
  void* closure);

void ReleaseLWSSession(struct LWSSession* session);

struct LWSMessage* AllocateLWSMessage(struct LWSSession* session, size_t length, enum lws_write_protocol protocol);
void TransmitLWSMessage(struct LWSMessage* message);
```

## Sessions and Messages

- Every event with a session attached goes to its `function`, and the return value goes back
  to libwebsockets; `LWS_CALLBACK_CLIENT_WRITEABLE` is consumed by the module while its send
  queue is not empty. `LWS_CALLBACK_WSI_DESTROY` is the last event of a connection;
  `session->instance` is cleared before it is delivered.
- A message from `AllocateLWSMessage()` has `LWS_PRE` bytes of headroom in `buffer`, `data`
  starting after it and `length` preset; there is no trailing zero. A message with
  `data = NULL` is a close marker. A message belongs to the caller until
  `TransmitLWSMessage()`, then to the session. Transmit only while the connection exists:
  after `LWS_CALLBACK_WSI_DESTROY`, or when the connection failed to start, the session has
  no instance to write on.
- `Create*Session()` returns a session even when the connection cannot be started; it then
  has no instance. `CreateLWSSessionFromAddress()` passes `secure` to libwebsockets as
  `ssl_connection`, so `LCCSCF_*` flags may be given.
- `SetLWSReportHandler()` is process-wide.

## Lifecycle

`LWSCore` creates nothing on the ring itself: libwebsockets runs on the foreign loop it is
given, and the loop adapter (`FastGLoop` or `FastUVLoop`) owns the ring objects. A session
belongs to the caller and is freed only by `ReleaseLWSSession()`, which may be called from
`LWS_CALLBACK_WSI_DESTROY` or after it. Called earlier, it detaches the session from the
connection, which libwebsockets closes the next time it calls the protocol for it; no further
event is delivered.

Release order: every session, then `ReleaseLWSCore()`, then the loop, then the ring. With
libuv, `ReleaseLWSCore()` only calls `lws_cancel_service()` and leaves the context allocated.
Everything runs on the ring thread; in the GLib build libwebsockets callbacks run inside the
GLib fiber on that thread.
