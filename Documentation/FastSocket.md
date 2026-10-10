# FastSocket API Reference

Header: `Ring/FastSocket.h`

`FastSocket` provides asynchronous socket send/receive on top of `FastRing` and `FastBuffer`:
a multishot receive into provided buffers, and sends batched into linked chains, one batch
in flight at a time. The socket owns its file descriptor.

## Socket Event Callback

```c
typedef void (*HandleFastSocketEvent)(struct FastSocket* socket, int event, int parameter);
```

`event` uses `poll` constants:

| Event | Meaning | `parameter` |
| --- | --- | --- |
| `POLLIN` | Data is queued and ready to be taken | Total bytes currently queued |
| `POLLOUT` | The batch in flight has been accepted by the kernel; writing may continue | `0` |
| `POLLERR` | Send or receive error; the socket is latched as failed | Positive errno; `EPIPE` when the initial writability poll reported `POLLERR` or `POLLHUP` (a failed connect); `EIO` for an unrecoverable short write |
| `POLLHUP` | The receive path has ended: the peer closed, a fatal receive error, or the ring is being released | `0`, or the positive errno of the receive error |

`POLLIN` is delivered once per completed receive, with the whole queue length — not
per buffer. A handler that does not drain leaves the data queued; the next `POLLIN`
simply reports a larger number.

`POLLOUT` is delivered on the first completion of the last descriptor of the batch in
flight, when nothing but the currently open batch waits behind it; that batch is flushed at
the end of the iteration. For zero-copy sends the buffer notification may still be
pending. It is the signal to produce more data when streaming. An older batch that is
already closed is submitted directly, without an event.

`POLLHUP` does not release the socket; the owner still calls `ReleaseFastSocket()`.
`ReleaseFastSocket()` itself produces no event: it detaches the handler first.

## Modes

`mode` selects how outbound data is submitted. The constants alias `MSG_*` values, but they
are interpreted only as mode bits.

| Mode | Meaning |
| --- | --- |
| `FASTSOCKET_MODE_REGULAR` | Ordinary `send` / `sendmsg` |
| `FASTSOCKET_MODE_ZERO_COPY` | `SEND_ZC` / `SENDMSG_ZC`. The payload buffer is pinned until the kernel's zerocopy notification arrives, which is exactly why transmits hold a `FastBuffer` reference |
| `FASTSOCKET_MODE_AUTO_CORK` | Sets `MSG_MORE` on every send of a batch except its last, letting the kernel coalesce successive small writes |
| `FASTSOCKET_MODE_FILE_IO` | Treats the handle as a file rather than a socket: the inbound side uses `read_multishot` instead of `recv`, and `TransmitFastSocketData()` writes with `IORING_OP_WRITE` at the current file position. Requires liburing >= 2.6 (Linux 6.7 for `read_multishot`). **On anything but a regular file this mode requires `limit = 1`** — see below |

`FASTSOCKET_MODE_FILE_IO` must not be combined with `FASTSOCKET_MODE_ZERO_COPY`: the two
adjust the opcode of the same SQE and together produce a wrong one. `TransmitFastSocketMessage()`
always uses `send` / `sendmsg`, also in file mode.

Zero-copy is not free: it is a win for payloads large enough to beat the pinning and
notification overhead, and a loss for small ones. It also interacts with kTLS — see
the kTLS section of [FastBIO.md](FastBIO.md#ktls).

## Receive Mode

The `message` argument of `CreateFastSocket()` picks the inbound operation:

- `message == NULL` — `recv_multishot`. Cheapest; no peer address, no control data.
- `message != NULL` — `recvmsg_multishot`, with the header copied into the descriptor.
  Use this when the peer address or control messages (`SO_TIMESTAMPING`,
  `IPV6_PKTINFO`, kTLS record type) are needed. Each received buffer then holds the raw
  multishot layout (`struct io_uring_recvmsg_out`, name, control, payload), and its length
  counts all of it: take buffers with `ReceiveFastSocketBuffer()` and parse each with the
  `io_uring_recvmsg_*()` helpers. The copying API and the `FILE*` bridge return the raw
  layout.
- `FASTSOCKET_MODE_FILE_IO` — `read_multishot`, ignoring `message`.

`flags` is passed to the receive operation. Buffers are selected from `provider`, which must
be built with `AllocateRingFastBuffer()` over a `FastBufferPool` (see
[FastBuffer.md](FastBuffer.md#buffer-provider-glue)): every received buffer is converted
with `FAST_BUFFER()`. The receive is re-armed whenever the kernel ends the multishot without
a fatal error (`-ENOBUFS`, `-ECANCELED` from outside, or a plain end).

## Creation

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
- submits the inbound multishot receive and queues a `POLLOUT | POLLHUP | POLLERR` poll;
- tracks outbound batches and write readiness internally;
- on success takes ownership of `handle` and closes it when the object is freed;
- returns `NULL` only when the object or its receive descriptor cannot be allocated; the
  caller then still owns `handle`. `provider` must not be `NULL`.

`limit` bounds how many send SQEs may be batched before the queue is flushed. It is
clamped to half the submission queue size, which is also the default when `0` is
passed. Lower it to bound how much of the ring one socket may occupy. With
`FASTSOCKET_MODE_FILE_IO` it also decides whether short writes are recoverable — see
[Short writes in `FASTSOCKET_MODE_FILE_IO`](#short-writes-in-fastsocket_mode_file_io).

The socket does not take ownership of `provider`, `inbound` or `outbound` — they are
usually shared between sockets. It holds a reference to `provider`
(`HoldFastRingBufferProvider()`) until its last receive completion has been handled, and the
pools stay alive while their buffers do (see the pool reference counting in
[FastBuffer.md](FastBuffer.md)), so all three may be released independently of the socket;
see [Lifecycle](#lifecycle).

## Receive API

```c
ssize_t ReceiveFastSocketData(struct FastSocket* socket, void* data, size_t size, int flags);
```

Copies out of the receive queue, releasing each buffer as it is fully consumed.

Returns:
- `> 0`: bytes copied — up to `size`, or everything queued if less is available
- `-EAGAIN`: nothing queued, or `MSG_WAITALL` was requested and the queue holds less
  than `size`. Nothing is consumed in either case, so the call can simply be retried on
  the next `POLLIN`
- `-EINVAL`: `socket`, `data` or `size` invalid

An end of stream is not reported here — a closed peer arrives as `POLLHUP` through the
event handler, and `0` would only be mistaken for EOF by a caller shaped like `read()`.

Only `MSG_WAITALL` is interpreted; other `flags` are ignored.

Helpers from header:

```c
struct msghdr* GetFastSocketMessageHeader(struct FastSocket* socket);
struct FastBuffer* ReceiveFastSocketBuffer(struct FastSocket* socket);
```

- `GetFastSocketMessageHeader()` returns the `struct msghdr` of the in-flight
  `recvmsg` submission, or `NULL` in any other receive mode and as soon as the receive
  path has ended, while buffers may still be queued. Pass it to
  `io_uring_recvmsg_validate()` and friends to walk a completion; a caller that parses
  buffers after `POLLHUP` keeps its own copy of the header.
- `ReceiveFastSocketBuffer()` pops one buffered `FastBuffer` chunk **and transfers
  ownership to the caller**, who must call `ReleaseFastBuffer()` when done. This is
  the zero-copy read path: no data is copied, and the buffer may be kept for as long
  as needed.

The two receive styles are exclusive in practice. `ReceiveFastSocketData()` copies out
of the same queue that `ReceiveFastSocketBuffer()` hands over whole; mixing them on
one socket means reading `socket->inbound.position`, the offset already copied out of the
first buffer, before `ReceiveFastSocketBuffer()` resets it and returns the whole buffer.

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
- `-EAGAIN`: a descriptor, a buffer or a batch could not be allocated. They are allocated
  on demand and recycled, so this means memory is exhausted or the ring is being
  released; it is not a fixed pool running dry
- `-EPIPE`: socket in error state (`POLLERR` observed)

`POLLERR` is latched, not merely reported. A receive error other than `-ENOBUFS` and
`-ECANCELED` (also one the multishot survives), a failed send, a connect that came back
with `POLLERR`/`POLLHUP`, and an unrecoverable short write all mark the socket as failed,
and every transmit after that returns `-EPIPE`. The event handler still receives the
original error code as `parameter`, but a caller that ignores the event cannot keep
writing into a socket that will never deliver.

`TransmitFastSocketDescriptor()` takes ownership of the descriptor and the buffer
reference on success **and on failure** — every error path releases both itself, so the
caller must never release them after the call. The other two copy the payload into a
buffer taken from the outbound pool and own nothing of the caller's.

`TransmitFastSocketDescriptor()` is the low-level entry point: it takes a descriptor
whose SQE the caller has already prepared, plus the `FastBuffer` that owns the payload.

- The buffer reference is what keeps the payload alive until the send — including the
  deferred zerocopy notification — completes.
- The buffer to release is found again from the SQE, not from the argument: `addr` for
  `SEND` and `SEND_ZC`, `data.socket.vector.iov_base` for `SENDMSG`, `SENDMSG_ZC`,
  `WRITE`, `WRITE_FIXED` and `WRITEV`. The payload must therefore start at
  `buffer->data`. Other opcodes do not release a buffer.
- `buffer == NULL` is only accepted for `IORING_OP_POLL_ADD` and `IORING_OP_URING_CMD`
  descriptors, which carry no payload; anything else returns `-EINVAL`.
- The descriptor's `function`, `closure` and `data.number` are replaced.
- `IORING_RECVSEND_POLL_FIRST` and `MSG_WAITALL` are set automatically on send opcodes,
  so a send is not attempted before the socket is writable and is not completed short.
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
- When the batch in flight is accepted and no closed batch waits behind it, the socket
  reports `POLLOUT` and, if data was queued meanwhile, schedules a flush.

Ordering is preserved throughout — batches are submitted in creation order and the
descriptors within a batch as a chain.

A zero-copy send that fails with `-ENOBUFS`, together with the `-ECANCELED` links behind it,
is collected into a retry batch placed at the front of the queue and sent again, before
anything queued later.

### Short writes in `FASTSOCKET_MODE_FILE_IO`

Send opcodes carry `MSG_WAITALL`, so the kernel keeps retrying until the whole buffer
has left. `IORING_OP_WRITE`, which this mode uses, has no such flag, and the kernel
retries a partial write only for regular files and block devices. On a pipe, a FIFO,
a tty or any other character device a write may therefore complete with fewer bytes
than requested.

Such a write is resubmitted for the remainder, but only while nothing has been queued
behind it — that is, when the descriptor is the tail of its batch. Otherwise the
following buffers are already linked into the same chain and have been handed to the
kernel, the remainder would land after them, and the result would be a stream with a
hole rather than a truncated one. That case is not recoverable and is reported to the
handler as `POLLERR` with `EIO`.

**A batch of one descriptor makes every write a tail, so pass `limit = 1` whenever
this mode is used with anything but a regular file.** The cost is one submission per
buffer instead of a chain, which is normally irrelevant for such descriptors.

The distinction matters in practice: a TUN device is packet-oriented and never reports
a partial write, so `/dev/net/tun` is unaffected either way. A pipe or a tty is a byte
stream and is affected.

## Release

`ReleaseFastSocket()` marks the socket down, detaches the event handler, ends the inbound
multishot receive and drops the socket's own reference. The object survives until every
outstanding send and the cancelled receive have completed, so:

- calling it from inside an event handler is safe — the handler is on the stack, and
  teardown finishes after it returns;
- after the call the `struct FastSocket*` must be treated as dead; no event is delivered
  for it any more;
- the call does not block, and it does not cancel sends: queued batches are still
  submitted and in-flight sends complete, and only then is the handle closed.

## Lifecycle

Objects on the ring, all owned by the socket:

- one receive descriptor: multishot `recv`, `recvmsg` or `read` with buffers from
  `provider`, re-armed whenever the kernel ends it without a fatal error;
- one descriptor per transmit, plus the `POLLOUT` poll queued at creation; each send
  holds the `FastBuffer` reference passed with it;
- at most one flusher at a time, which submits the oldest batch at the end of an
  iteration;
- at the end, one `IORING_OP_CLOSE` descriptor for `handle`.

The socket counts its own references: one for the owner, one for the receive path, one
per outbound descriptor and one per scheduled flush. The count is a plain integer, so the
socket belongs to the ring thread.

Creation submits the receive and queues the poll; from then on the socket owns `handle`.
A received buffer moves from the provider into the socket's queue, and the provider slot is
refilled from `inbound`. `ReceiveFastSocketBuffer()` hands a buffer to the caller;
`ReceiveFastSocketData()` releases buffers as it empties them. A transmitted buffer
reference is dropped by the last completion of its descriptor, which for zero-copy is the
notification.

`POLLHUP` ends only the receive path. `ReleaseFastSocket()` drops the owner's reference
and ends the receive path if it is still running: a receive not yet submitted is rewritten
as a NOP, one in the kernel is cancelled, and one whose final completion is being handled
right now is left to that handler. When the last reference goes, the queued inbound
buffers are released, the batch structures are freed, `IORING_OP_CLOSE` is submitted for
`handle` (or `close()` is called when no descriptor can be allocated), and the object is
freed. Nothing reports that moment.

Every receive completion, the one of the cancelled receive included, returns its buffer to
`provider`; the socket's reference keeps the provider registered until then, and the pools
live as long as their buffers. The provider and the pools may therefore be released right
after the sockets. The ring may not: run the loop until the operations have completed, then
release the ring.

If `ReleaseFastRing()` runs while a socket is alive, its handlers are called with
`RING_REASON_RELEASED`: an attached event handler receives `POLLHUP` with `0`, outbound
handlers only release their buffers — no `POLLOUT`, nothing submitted or scheduled — and a
queued close calls `close()`. The object is freed
during the teardown only if its owner reference is gone by then; otherwise it leaks, and a
`ReleaseFastSocket()` afterwards uses the freed ring. Release sockets before the ring.

Every function, the `FILE*` bridge included, is called on the ring thread.

## `FILE*` Bridge

```c
FILE* GetFastSocketStream(struct FastSocket* socket, int own);
```

- Wraps socket as `FILE*` via `fopencookie`.
- stream read uses `ReceiveFastSocketData` and tells the two states of an empty queue
  apart. While the socket is still receiving, the read gives `errno = EAGAIN` and raises
  the error flag: call `clearerr()` and read again on the next `POLLIN`. Once the receive
  path has finished — a closed peer or a fatal receive error — it reports a real end of
  file, so `feof()` becomes true and `fgets()`, `fread()` and friends terminate on their
  own.
- stream write uses `TransmitFastSocketData`, so both of its states reach the stream as
  well. A failed allocation gives `errno = EAGAIN` and clears the same way as on the read
  side; a socket latched as failed gives `errno = EPIPE`, and that one does not go away,
  since no further write on it will ever be delivered.
- if `own != 0`, `fclose(stream)` will call `ReleaseFastSocket()`. Otherwise the stream
  must not be used after the socket has been released.

## Minimal Pattern

```c
struct FastRing* ring = CreateFastRing(0);
// create pools, a provider over the inbound pool, and the FastSocket

while (running && (WaitForFastRing(ring, 1000, NULL) >= 0));

ReleaseFastSocket(socket);

// a few more iterations let the cancelled receive and the queued sends complete
for (int count = 0; count < 10; count ++)
  WaitForFastRing(ring, 10, NULL);

ReleaseFastRingBufferProvider(provider, ReleaseRingFastBuffer);
ReleaseFastBufferPool(inbound);
ReleaseFastBufferPool(outbound);
ReleaseFastRing(ring);
```
