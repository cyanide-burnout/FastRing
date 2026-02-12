# FastSocket API Reference

This document describes the public C API from `Ring/FastSocket.h`.

`FastSocket` provides asynchronous socket send/receive on top of `FastRing` and `FastBuffer`.

## Socket Event Callback

```c
typedef void (*HandleFastSocketEvent)(struct FastSocket* socket, int event, int parameter);
```

`event` uses `poll` constants:
- `POLLIN`: inbound bytes available, `parameter` is buffered byte count.
- `POLLOUT`: send queue drained/writable.
- `POLLERR`: I/O/connect/send error, `parameter` is positive errno value.
- `POLLHUP`: local/remote close or termination.

## Modes

- `FASTSOCKET_MODE_REGULAR`
- `FASTSOCKET_MODE_ZERO_COPY` (`MSG_ZEROCOPY`)
- `FASTSOCKET_MODE_AUTO_CORK` (`MSG_MORE`)
- `FASTSOCKET_MODE_FILE_IO` (`MSG_DONTROUTE`, enabled by liburing macro path)

## Lifecycle

```c
struct FastSocket* CreateFastSocket(
  struct FastRing* ring,
  struct FastRingBufferProvider* provider,
  struct FastBufferPool* inbound,
  struct FastBufferPool* outbound,
  int handle,
  struct msghdr* message,
  int flags,
  int mode,
  uint32_t limit,
  HandleFastSocketEvent function,
  void* closure);

void ReleaseFastSocket(struct FastSocket* socket);
```

Behavior:
- starts inbound multishot receive during creation.
- tracks outbound batches and write readiness internally.
- returns `NULL` on allocation/setup failure.

## Receive API

```c
ssize_t ReceiveFastSocketData(struct FastSocket* socket, void* data, size_t size, int flags);
```

Returns:
- `> 0`: bytes copied
- `0`: no data yet (or `MSG_WAITALL` cannot be satisfied)
- `< 0`: error (`-EINVAL`, etc.)

Helpers from header:

```c
struct msghdr* GetFastSocketMessageHeader(struct FastSocket* socket);
struct FastBuffer* ReceiveFastSocketBuffer(struct FastSocket* socket);
```

- `GetFastSocketMessageHeader()` returns recvmsg metadata when recvmsg mode is used.
- `ReceiveFastSocketBuffer()` pops one buffered `FastBuffer` chunk.

## Transmit API

```c
int TransmitFastSocketDescriptor(struct FastSocket* socket, struct FastRingDescriptor* descriptor, struct FastBuffer* buffer);
int TransmitFastSocketMessage(struct FastSocket* socket, struct msghdr* message, int flags);
int TransmitFastSocketData(struct FastSocket* socket, struct sockaddr* address, socklen_t length, const void* data, size_t size, int flags);
```

Common return codes:
- `0`: success
- `-EINVAL`: invalid arguments
- `-ENOMEM`: allocation failure
- `-EPIPE`: socket in error state (`POLLERR` observed)

`TransmitFastSocketDescriptor()` rules:
- pass a prepared descriptor and owning `FastBuffer` for normal send operations.
- `buffer == NULL` is allowed for internal poll/uring command style descriptors.

## `FILE*` Bridge

```c
FILE* GetFastSocketStream(struct FastSocket* socket, int own);
```

- Wraps socket as `FILE*` via `fopencookie`.
- stream read uses `ReceiveFastSocketData`.
- stream write uses `TransmitFastSocketData`.
- if `own != 0`, `fclose(stream)` will call `ReleaseFastSocket()`.

## Minimal Pattern

```c
struct FastRing* ring = CreateFastRing(0);
// create provider and pools
// create FastSocket

for (;;)
{
  int rc = WaitForFastRing(ring, 1000, NULL);
  if (rc < 0) break;
}

ReleaseFastRing(ring);
```
