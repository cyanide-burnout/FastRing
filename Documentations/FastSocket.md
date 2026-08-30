# FastSocket API Reference

This document describes the public C API from `Ring/FastSocket.h`.

`FastSocket` provides asynchronous socket send/receive on top of `FastRing` and `FastBuffer`.

## Socket Event Callback

```c
typedef void (*HandleFastSocketEvent)(struct FastSocket* socket, int event, int parameter);
```

`event` uses `poll` constants:

| Event | Meaning | `parameter` |
| --- | --- | --- |
| `POLLIN` | Data is queued and ready to be taken | Total bytes currently queued |
| `POLLOUT` | Everything queued has been accepted by the kernel | `0` |
| `POLLERR` | Send or receive error | Positive errno, or `EPIPE` when the send path is broken |
| `POLLHUP` | Peer closed, or the socket is being torn down | Positive errno, or `0` |

`POLLIN` is delivered once per completed receive, with the whole queue length — not
per buffer. A handler that does not drain leaves the data queued; the next `POLLIN`
simply reports a larger number.

`POLLOUT` means *drained*, not merely writable: it fires when the last outstanding
batch completes and nothing is left to send. It is the signal to produce more data
when streaming.

## Modes

`mode` selects how outbound data is submitted. The constants alias `MSG_*` values so
they can be OR-ed into send flags directly.

| Mode | Meaning |
| --- | --- |
| `FASTSOCKET_MODE_REGULAR` | Ordinary `send` / `sendmsg` |
| `FASTSOCKET_MODE_ZERO_COPY` | `SEND_ZC` / `SENDMSG_ZC`. The payload buffer is pinned until the kernel's zerocopy notification arrives, which is exactly why transmits hold a `FastBuffer` reference |
| `FASTSOCKET_MODE_AUTO_CORK` | Adds `MSG_MORE`, letting the kernel coalesce successive small writes |
| `FASTSOCKET_MODE_FILE_IO` | Treats the handle as a file rather than a socket: the inbound side uses `read_multishot` instead of `recv`. Requires liburing >= 2.6 |

Zero-copy is not free: it is a win for payloads large enough to beat the pinning and
notification overhead, and a loss for small ones. It also interacts with kTLS — see
the kTLS note in the top-level `README.md`.

## Receive Mode

The `message` argument of `CreateFastSocket()` picks the inbound operation:

- `message == NULL` — `recv_multishot`. Cheapest; no peer address, no control data.
- `message != NULL` — `recvmsg_multishot`, with the header copied into the descriptor.
  Use this when the peer address or control messages (`SO_TIMESTAMPING`,
  `IPV6_PKTINFO`, kTLS record type) are needed. Parse completions with the
  `io_uring_recvmsg_*()` helpers over `GetFastSocketMessageHeader()`.
- `FASTSOCKET_MODE_FILE_IO` — `read_multishot`, ignoring `message`.

`flags` is passed to the receive operation.

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

`limit` bounds how many send SQEs may be batched before the queue is flushed. It is
clamped to half the submission queue size, which is also the default when `0` is
passed. Lower it to bound how much of the ring one socket may occupy.

The socket does not take ownership of `provider`, `inbound` or `outbound` — they are
usually shared between sockets and must outlive them, or at least be released
independently (see the pool reference counting in `Documentations/FastBuffer.md`).

## Receive API

```c
ssize_t ReceiveFastSocketData(struct FastSocket* socket, void* data, size_t size, int flags);
```

Copies out of the receive queue, releasing each buffer as it is fully consumed.

Returns:
- `> 0`: bytes copied — up to `size`, or everything queued if less is available
- `0`: nothing queued, or `MSG_WAITALL` was requested and the queue holds less than
  `size`. In the `MSG_WAITALL` case nothing is consumed, so the call can simply be
  retried on the next `POLLIN`
- `-EINVAL`: `socket`, `data` or `size` invalid

Only `MSG_WAITALL` is interpreted; other `flags` are ignored.

Helpers from header:

```c
struct msghdr* GetFastSocketMessageHeader(struct FastSocket* socket);
struct FastBuffer* ReceiveFastSocketBuffer(struct FastSocket* socket);
```

- `GetFastSocketMessageHeader()` returns the `struct msghdr` of the in-flight
  `recvmsg` submission, or `NULL` in any other receive mode. Pass it to
  `io_uring_recvmsg_validate()` and friends to walk a completion.
- `ReceiveFastSocketBuffer()` pops one buffered `FastBuffer` chunk **and transfers
  ownership to the caller**, who must call `ReleaseFastBuffer()` when done. This is
  the zero-copy read path: no data is copied, and the buffer may be kept for as long
  as needed.

The two receive styles are exclusive in practice. `ReceiveFastSocketData()` copies out
of the same queue that `ReceiveFastSocketBuffer()` hands over whole; mixing them on
one socket means tracking `socket->inbound.position` yourself.

Both are meant to be called from the `POLLIN` event, where `parameter` reports how
many bytes are buffered. Draining is the handler's responsibility: data left in the
queue stays there until the next event.

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

`TransmitFastSocketDescriptor()` takes ownership of the descriptor and the buffer on
success **and on failure** — every error path releases both itself, so the caller must
never release them after the call. The other two copy the payload into a buffer taken
from the outbound pool and own nothing of the caller's.

`TransmitFastSocketDescriptor()` is the low-level entry point: it takes a descriptor
whose SQE the caller has already prepared, plus the `FastBuffer` that owns the payload.

- The buffer reference is what keeps the payload alive until the send — including the
  deferred zerocopy notification — completes.
- `buffer == NULL` is only accepted for `IORING_OP_POLL_ADD` and `IORING_OP_URING_CMD`
  descriptors, which carry no payload; anything else returns `-EINVAL`.
- `IORING_RECVSEND_POLL_FIRST` is set automatically on send opcodes, so a send is not
  attempted before the socket is writable.
- Use it when the payload is already in a pool buffer and copying would be wasteful —
  `KCPAdapter` builds `send_zc` descriptors this way.

`TransmitFastSocketMessage()` and `TransmitFastSocketData()` are the copying
convenience wrappers: they take a buffer from the outbound pool, copy the payload in,
and submit. `TransmitFastSocketData()` also accepts a destination address for
unconnected sockets.

Ordering is preserved across all three: submissions are appended to the current
outbound batch in call order.

## How Sending Is Scheduled

Transmits are not submitted one by one. Descriptors accumulate into an outbound
**batch**, and a batch is submitted as a single linked range from the completion of the
previous one — so **at most one batch is in flight at a time**. That is the module's
flow control: a fast producer fills the next batch while the current one is on the
wire, instead of flooding the submission queue.

Consequences:

- A write issued while a batch is in flight is not submitted immediately. It leaves
  with the next batch, which is normally the next ring iteration.
- `limit` caps a batch; reaching it starts a new one.
- At creation the module submits a `POLLOUT` poll through the same path. That
  completion is what primes the pump and produces the first `POLLOUT` event, so a
  handler may start writing as soon as it sees it.
- When a batch completes and no further batch exists, the socket reports `POLLOUT` and,
  if data was queued meanwhile, schedules a flush.

Ordering is preserved throughout — batches are submitted in creation order and the
descriptors within a batch as a chain.

## Release

`ReleaseFastSocket()` marks the socket down, cancels the inbound multishot receive and
drops the socket's own reference. The object survives until every outstanding send and
the cancelled receive have completed, so:

- calling it from inside an event handler is safe — the handler is on the stack, and
  teardown finishes after it returns;
- after the call the `struct FastSocket*` must be treated as dead. Clear stored
  pointers, exactly as with `POLLHUP`;
- queued sends are not waited for.

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
