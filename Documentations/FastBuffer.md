# FastBuffer API Reference

Header: `Ring/FastBuffer.h`

`FastBuffer` is a reference-counted buffer pool with optional `io_uring` fixed-buffer
registration. It is the allocator behind `FastSocket`, `FastBIO` and the FastRing
buffer providers: buffers are recycled through a lock-free stack instead of being
malloc'd per operation, and a buffer can outlive the call that produced it because it
is reference counted rather than owned by one place.

## Core Types

- `struct FastBufferPool` — shared pool, one per traffic direction by convention
- `struct FastBuffer` — individual buffer, payload in `buffer->data`

## API

```c
struct FastBufferPool* CreateFastBufferPool(struct FastRing* ring);
void ReleaseFastBufferPool(struct FastBufferPool* pool);

struct FastBuffer* AllocateFastBuffer(struct FastBufferPool* pool, uint32_t size, int option);
struct FastBuffer* HoldFastBuffer(struct FastBuffer* buffer);
void ReleaseFastBuffer(struct FastBuffer* buffer);
void TryRegisterFastBuffer(struct FastBuffer* buffer, int option);

void PrepareFastBuffer(struct FastRingDescriptor* descriptor, struct FastBuffer* buffer);

void* AllocateRingFastBuffer(size_t size, void* closure);
void ReleaseRingFastBuffer(void* buffer);
```

## Reference Counting

Both the buffer and the pool are counted, and they are linked:

- `AllocateFastBuffer()` returns a buffer with count `1` **and takes a reference on the
  pool**.
- `HoldFastBuffer()` adds a reference, `ReleaseFastBuffer()` drops one. At zero the
  buffer returns to the pool's free stack and the pool reference is dropped.
- `ReleaseFastBufferPool()` drops the pool's own reference. The pool and its cached
  buffers are freed only when the last live buffer has been released.

That last point is the useful part: **a pool may be released while buffers are still in
flight**. Ordering between `ReleaseFastBufferPool()` and outstanding I/O does not
matter — whichever finishes last does the cleanup.

## Buffer Fields

```c
struct FastBuffer
{
  int state;        // FAST_BUFFER_STATE_FREE / _ALLOCATED
  int index;        // registered buffer index, INT32_MIN, or negative errno
  int status;       // FAST_BUFFER_STATUS_*
  uint32_t size;    // capacity of data[]
  uint32_t length;  // payload length, maintained by the owner
  struct FastBufferPool* pool;
  struct FastBuffer* next;
  ...
  uint64_t magic;   // FAST_BUFFER_MAGIC
  uint8_t data[0];
};
```

Two fields belong to the **owner while the buffer is allocated**, not to the pool:

- `length` — how many bytes of `data` are meaningful. Allocation resets it to `0`.
- `next` — free for the holder to chain buffers into its own queue. `FastSocket` and
  `FastBIO` both build their inbound queues this way. The pool only uses `next` while
  the buffer sits on the free stack, and those two lifetimes never overlap.

`state` and `magic` are the pool's, and `tag` is the ABA counter for the lock-free
stack; do not write any of them.

## Recovering the Buffer from a Payload Pointer

```c
#define FAST_BUFFER(address)  ((struct FastBuffer*)(((uint8_t*)(address)) - offsetof(struct FastBuffer, data)))
```

Modules that pass `buffer->data` around use `FAST_BUFFER()` to get back to the
descriptor. `buffer->magic` holds `FAST_BUFFER_MAGIC`, and both `AllocateFastBuffer()`
and `ReleaseFastBuffer()` validate `magic` and `state` on every call, calling
`raise(SIGABRT)` on mismatch. A double release or a pointer that never came from a
pool therefore aborts at the point of misuse rather than corrupting the free stack
silently.

## Registration

- `FAST_BUFFER_REGISTER` (`1 << 0`) passed to `AllocateFastBuffer()` requests
  io_uring fixed-buffer registration. Registration is attempted **only when the buffer
  is freshly allocated** — a buffer taken from the free stack keeps whatever
  registration it already had, since re-registering an unchanged address is pointless.
- `TryRegisterFastBuffer()` performs the pending registration explicitly, driven by
  `buffer->status`:
  `FAST_BUFFER_STATUS_ADDED` (never registered — add),
  `FAST_BUFFER_STATUS_UPDATED` (address changed — update),
  `FAST_BUFFER_STATUS_UNCHANGED` (nothing to do).
- `buffer->index` is the registered index, `INT32_MIN` when the buffer is not
  registered, or a negative errno when registration failed.
- `PrepareFastBuffer()` sets `IORING_RECVSEND_FIXED_BUF` and `buf_index` on the SQE
  when `buffer->index >= 0`, and does nothing otherwise — so the same call site works
  registered or not.

## Sizing and Reuse

`AllocateFastBuffer(pool, size, option)` pops the most recently released buffer. If
that buffer is at least `size` bytes it is reused as is; if it is smaller it is freed
and a new one is allocated, because there is no aligned `realloc()`.

Consequences worth planning for:

- A pool used with one buffer size recycles perfectly.
- A pool used with wildly varying sizes churns: a large request can discard a small
  cached buffer, and the next small request may then reuse an oversized one. Use
  separate pools per size class when that matters.
- `buffer->size` is the capacity, `buffer->length` is an optional payload length the
  owner maintains. Allocation resets `length` to `0`.
- Buffers are aligned to `FAST_BUFFER_ALIGNMENT` (64), and `data` is aligned to
  `__BIGGEST_ALIGNMENT__` — both enforced by static assertions.

## Buffer Provider Glue

```c
void* AllocateRingFastBuffer(size_t size, void* closure);
void ReleaseRingFastBuffer(void* buffer);
```

These match `CreateRingBufferFunction` / `ReleaseRingBufferFunction` from the FastRing
Buffer Provider API, with the pool passed as `closure`:

```c
provider = CreateFastRingBufferProvider(ring, 0, count, length, AllocateRingFastBuffer, pool);
...
ReleaseFastRingBufferProvider(provider, ReleaseRingFastBuffer);
```

They return and accept `buffer->data`, converting through `FAST_BUFFER()` internally.

## Thread Safety

The free stack is lock-free on release and guarded by a short spinlock on allocation,
so buffers may be allocated and released from any thread. `buffer->length`, `next` and
the payload are the owner's business and are not synchronised.

The reference count is atomic, so `HoldFastBuffer()` from a producer thread and
`ReleaseFastBuffer()` from the ring thread are safe — which is exactly the pattern a
buffer handed to a transmit uses.

## Typical Patterns

**Receive, keep, release later.** The zero-copy read path: take the buffer out of a
socket's queue and hold it for as long as the payload is needed.

```c
while (buffer = ReceiveFastSocketBuffer(socket))
{
  ProcessInPlace(buffer->data, buffer->length);
  ReleaseFastBuffer(buffer);
}
```

**Build once, send with ownership.** Allocate from the outbound pool, fill, set
`length`, and hand both descriptor and buffer to the socket — the buffer reference is
what keeps the payload alive until the send, including a deferred zerocopy
notification, completes.

```c
buffer = AllocateFastBuffer(pool, size, FAST_BUFFER_REGISTER);
memcpy(buffer->data, payload, size);
buffer->length = size;

descriptor = AllocateFastRingDescriptor(ring, NULL, NULL);
io_uring_prep_send_zc(&descriptor->submission, handle, buffer->data, size, 0, 0);
PrepareFastBuffer(descriptor, buffer);

TransmitFastSocketDescriptor(socket, descriptor, buffer);   // takes ownership of both
```

**Share one payload across several sends.** `HoldFastBuffer()` once per consumer;
each transmit releases its own reference when done.

## Sizing Guidance

- One pool per direction and per size class. Mixing a 2 KB receive path and a 64 KB
  send path in one pool makes them evict each other.
- Match the pool's size to the buffer provider's when the pool backs a provider —
  `AllocateRingFastBuffer()` passes the provider's `length` straight through.
- `FAST_BUFFER_REGISTER` pays off for buffers that are reused many times; for a
  one-shot buffer the registration syscall costs more than it saves.
