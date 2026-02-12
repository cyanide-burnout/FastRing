# LWSCore API Reference (Deprecated)

Header: `Ring/LWSCore.h`

`LWSCore` is the legacy WebSocket adapter based on `libwebsockets`.
In this repository it is deprecated; prefer `CURLWSCore`.

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

