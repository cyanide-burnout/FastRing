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

`USE_LWS_EVENTLOOP` resolves to the matching `LWS_SERVER_OPTION_*` value for the
selected loop.

## Core Options

The `option` argument of `CreateLWSCore()` packs two fields:

- `LWS_OPTION_MINIMAL_SSL_VERSION_MASK` (`0x0fff`) - the minimum TLS version, given as
  an OpenSSL version constant such as `SSL3_VERSION` or `TLS1_2_VERSION`.
- `LWS_OPTION_IGNORE_CERTIFICATE` (`1 << 12`) - skip peer certificate validation.

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

