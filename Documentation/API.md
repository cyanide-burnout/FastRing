# FastRing API Reference

Header: `Ring/FastRing.h`.

This document lists the public interface of the core engine. The model behind it, the ring
thread, descriptor states, references, cancellation and shutdown, is explained in
[LIFECYCLE.md](LIFECYCLE.md); the modules built on the core have their own documents, indexed in
[README.md](README.md).

- [Conventions](#conventions)
- [Ring](#ring)
- [Descriptors](#descriptors)
- [Flush Handlers](#flush-handlers)
- [Discard](#discard)
- [Poll](#poll)
- [Watch](#watch)
- [Timeout](#timeout)
- [Event](#event)
- [Buffer Provider](#buffer-provider)
- [Registered Files and Buffers](#registered-files-and-buffers)
- [Tracing](#tracing)
- [Limits and Constants](#limits-and-constants)

## Conventions

- Functions returning `int` return `0` or a positive value on success and a negative error code
  (`-EINVAL`, `-ENOMEM`, `-EBADF`, ...) on failure; `errno` is not used. Functions returning a
  pointer return `NULL` on failure.
- Callbacks run on the ring thread, inside `WaitForFastRing()`, or inside `ReleaseFastRing()`
  with `RING_REASON_RELEASED`.
- Which function may be called from which thread is listed in
  [LIFECYCLE.md](LIFECYCLE.md#thread-rules).
- In C++, `ATOMIC(type)` fields are `std::atomic<type>`, and `CreateSharedFastRing(length = 0)`
  returns a `std::shared_ptr<struct FastRing>` bound to `ReleaseFastRing()`.

## Ring

```c
struct FastRing* CreateFastRing(uint32_t length);
void ReleaseFastRing(struct FastRing* ring);
int WaitForFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask);
int IsFastRingThread(struct FastRing* ring);
```

`CreateFastRing(length)` creates a ring on the calling thread, which becomes the ring thread.

- `length` is the submission queue length. `0` takes `RLIMIT_NOFILE`; the value is capped at
  16384 and rounded up to a power of two. The completion queue is four times longer.
- Setup flags: `IORING_SETUP_SUBMIT_ALL`, `IORING_SETUP_COOP_TASKRUN`,
  `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_CQSIZE`.
- Registers a sparse file table of `RLIMIT_NOFILE / 2` entries (at most `1 << 20`) and leaves its
  upper half to the kernel for direct descriptors.
- Excludes the ring from `fork()`, turns off iowait accounting where supported, and probes the
  supported opcodes into `ring->probe`.
- Returns `NULL` when `io_uring` cannot be set up, among others on kernels older than 6.0, or
  when memory runs out.

`WaitForFastRing(ring, interval, mask)` runs one iteration of the loop: it moves queued
descriptors into the submission queue, submits, waits for at most `interval` milliseconds unless
an SQE or a CQE is ready already, handles all completions and runs the flushers. `mask` is the
signal mask during the wait (`io_uring_submit_and_wait_timeout()`), `NULL` for none. It returns
the number of submitted SQEs, `0` when the wait timed out, `-EINVAL` for a `NULL` or uninitialized
ring, or another negative error code of the submission, `-EINTR` among them. It must not be
called from a callback.

`ReleaseFastRing()` calls every armed flusher and every live descriptor that has a handler with
`RING_REASON_RELEASED`, frees all descriptors, flushers and registrations, and closes the ring.
It does not reap outstanding completions. See [Shutdown](LIFECYCLE.md#shutdown).

`IsFastRingThread()` returns `1` on the ring thread, `0` on another thread, and `-1` if the ring
has no recorded thread.

## Descriptors

```c
struct FastRingDescriptor* AllocateFastRingDescriptor(struct FastRing* ring,
                                                      HandleFastRingCompletionFunction function,
                                                      void* closure);
void PrepareFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last);
int ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor);

typedef int (*HandleFastRingCompletionFunction)(struct FastRingDescriptor* descriptor,
                                                struct io_uring_cqe* completion,
                                                int reason);
```

- `AllocateFastRingDescriptor()` returns a descriptor in `RING_DESC_STATE_ALLOCATED` with one
  reference, an `IORING_OP_NOP` SQE, `length` of one SQE, and the given handler and closure.
  `function` may be `NULL` for an operation whose completion needs no handling. Returns `NULL`
  when `ring` is `NULL`, when memory is exhausted, or while the ring is being released.
- `PrepareFastRingDescriptor()` computes the identifier, stores it with `option` in
  `submission.user_data`, and moves the descriptor to `RING_DESC_STATE_PENDING`. It does not
  queue it.
- `SubmitFastRingDescriptor()` prepares and queues one descriptor. The SQE reaches the kernel on
  the next `WaitForFastRing()`.
- `SubmitFastRingDescriptorRange()` queues a chain of prepared descriptors linked through `next`
  from `first` to `last`. `first->linked` is the number of SQEs the chain needs; the chain is
  submitted only when that many fit into the submission queue. See
  [Chains](LIFECYCLE.md#chains).
- `ReleaseFastRingDescriptor()` drops one reference and recycles the descriptor when the count
  reaches `0`. It returns the remaining count, or `-1` for `NULL`. The completion handler's
  return value does this for submitted operations; call it explicitly for a descriptor that was
  never submitted, for a reference taken by the owner, and for event receivers.

The completion handler returns `0` to drop one reference and non-zero when the descriptor stays
armed (re-submitted by the handler, or a multishot completion with `IORING_CQE_F_MORE`).

| `reason` | Value | `completion` |
| --- | --- | --- |
| `RING_REASON_COMPLETE` | 0 | The CQE, or `NULL` for a member of an `IOSQE_CQE_SKIP_SUCCESS` chain |
| `RING_REASON_INCOMPLETE` | 1 | Not produced since March 2024, see [LIFECYCLE.md](LIFECYCLE.md#arguments) |
| `RING_REASON_RELEASED` | 2 | `NULL`; the ring is being released, the return value is ignored |

### Fields

The owner may use these fields of `struct FastRingDescriptor`:

| Field | Use |
| --- | --- |
| `submission` | The SQE to prepare with `io_uring_prep_*()`; start every re-preparation with `io_uring_initialize_sqe()` |
| `length` | Bytes of `submission` copied into the ring; set it to twice an SQE for a 128-byte entry |
| `closure`, `function` | Handler and argument; may be changed while the owner holds the descriptor exclusively |
| `data` | 256 bytes for the owner (`pointer`, `number`, `socket`, raw `data`); Poll, Watch and Timeout use it themselves |
| `references` | Atomic reference count; see [References](LIFECYCLE.md#references-and-ownership) |
| `linked`, `next`, `previous` | Chain construction |
| `ring` | The ring of the descriptor |

`state`, `lock`, `identifier`, `tag` and `heap` belong to the ring.

### States and options

| Constant | Value | Meaning |
| --- | --- | --- |
| `RING_DESC_STATE_FREE` | 0 | On the free list |
| `RING_DESC_STATE_ALLOCATED` | 1 | Allocated, not prepared |
| `RING_DESC_STATE_PENDING` | 2 | Prepared; may be queued |
| `RING_DESC_STATE_LOCKED` | 3 | Claimed by a writer or the submission loop |
| `RING_DESC_STATE_SUBMITTED` | 4 | In the kernel |
| `RING_DESC_LOCK_HELD` | 1 | Bit of `lock`: a completion handler is running |
| `RING_DESC_LOCK_WAITER` | 2 | Unit of `lock` counting writers that wait for the handler |
| `RING_DESC_OPTION_IGNORE` | 256 | Housekeeping completion; built-in handlers skip the user callback |
| `RING_DESC_OPTION_USER1` | 128 | Free for the owner |
| `RING_DESC_OPTION_USER2` | 64 | Free for the owner |
| `RING_DESC_OPTION_MASK` | 448 | All option bits |

The `option` argument of `PrepareFastRingDescriptor()`, `SubmitFastRingDescriptor()` and
`SubmitFastRingEvent()` is masked with `RING_DESC_OPTION_MASK` and returned in
`completion->user_data`. Pass `0` for a normal submission.

### Conditions

Poll, Watch and Timeout keep a `condition` word in `descriptor->data` (`poll`, `watch`,
`timeout`): `RING_CONDITION_GUARD` (`1 << 16`) while the user callback runs,
`RING_CONDITION_UPDATE` (`1 << 17`) when the operation ended without being re-armed, and
`RING_CONDITION_REMOVE` (`1 << 18`) once removal was requested; `RING_CONDITION_MASK` covers all
three. They are what makes it safe to update or remove an operation from inside its own
callback; see [Reentrancy](LIFECYCLE.md#reentrancy).

## Flush Handlers

```c
typedef void (*HandleFastRingFlushFunction)(void* closure, int reason);

struct FastRingFlusher* SetFastRingFlushHandler(struct FastRing* ring, HandleFastRingFlushFunction function, void* closure);
int RemoveFastRingFlushHandler(struct FastRing* ring, struct FastRingFlusher* flusher);
```

A flusher is a **one-shot callback that runs at the end of the current loop iteration**, after
every CQE of this pass has been handled. It is the mechanism for "do this once more, but not
right now": batching work that several completions would otherwise each trigger, and getting off
a completion handler's stack before touching shared state.

That is the whole of it. A flusher is a phase of the loop, not a task queue, not a notification
and not a general cancellable job. A synchronous call from an arbitrary foreign thread is
`ThreadCall`, which supplies the wakeup, generation, reference count and join protocol such a
thing needs. A cross-ring handoff from a ring that is already making progress is
`SubmitFastRingEvent()`. Neither belongs here.

`SetFastRingFlushHandler()` returns the armed flusher, or `NULL` when `ring` is `NULL` or memory
is exhausted.

### When it runs

```text
WaitForFastRing()
  submit pending SQEs
  handle CQEs           <-- completion handlers run here, may arm flushers
  run flush handlers    <-- here, until none are left
```

The drain is a loop over the pending stack, so **a flusher armed from inside a flusher runs in the
same iteration**, not the next one. That is what lets a module re-arm itself to make repeated
progress: `DBusCore` dispatches one D-Bus message per pass and re-arms until the queue is empty,
all inside one `WaitForFastRing()` call. Re-arming unconditionally is therefore an infinite loop;
always stop on a completion condition.

Handlers are kept on a stack, so within one iteration they run in reverse arming order. Do not
depend on ordering between independent modules.

### One-shot, and the idempotent-arm idiom

Being called returns the flusher to the free list, so each `SetFastRingFlushHandler()` produces
exactly one call. Modules that want one pass per iteration keep the handle and check it:

```c
void TouchModule(struct Module* module)
{
  if (module->flusher == NULL)
  {
    // arm at most once per cycle
    module->flusher = SetFastRingFlushHandler(module->ring, HandleFlush, module);
  }
}

static void HandleFlush(void* closure, int reason)
{
  struct Module* module = (struct Module*)closure;

  module->flusher = NULL;          // clear first: the handler may re-arm below

  if (reason == RING_REASON_COMPLETE)
    MakeProgress(module);
}
```

`CURLWSCore`, `DBusCore`, `FastGLoop`, `FastUVLoop` and `Fetch` use exactly this shape.

### Reasons

- `RING_REASON_COMPLETE`: the normal end-of-iteration call.
- `RING_REASON_RELEASED`: `ReleaseFastRing()` is tearing the ring down. Both the pending and the
  free stacks are walked, so a still-armed flusher gets this call before its memory goes away.
  Release module state here; do not submit anything.

Always branch on `reason`: a handler that does its work unconditionally runs it again during
shutdown.

### Cancelling

`RemoveFastRingFlushHandler()` cancels an armed flusher by changing its state from pending to
free:

- `0`: cancelled, the handler will not be called;
- `-EPERM`: too late, the flusher is already running or has already run;
- `-EBADF`: `ring` or `flusher` is `NULL`.

A cancelled flusher is not recycled immediately; the drain loop notices the changed state, skips
the call and reclaims it.

**A handle stops being a handle once the flusher has run.** The object goes back to the free list
and the next `SetFastRingFlushHandler()` may hand it to somebody else, in which case its state is
`RING_FLUSH_STATE_PENDING` again. The cancellation looks at nothing but that state, so it succeeds
and quietly cancels the **new** owner's flusher, and returns `0` while doing it. Clear the handle
in the handler, as above, and cancel only handles you know are still armed. Within the ring
thread that is a matter of discipline rather than of timing, since nobody else can arm or run a
flusher meanwhile.

### Rules

- **The whole facility belongs to the ring thread**, arming included. Both calls are lock-free and
  will not corrupt anything if called from elsewhere, but arming from another thread does not
  wake the ring: nothing is submitted and no event is signalled, so the handler waits for the
  wait to expire or for an unrelated completion. And a handle cannot be owned by a foreign
  thread at all: the flusher may run and be recycled before that thread has stored the pointer,
  after which every use of it lands on somebody else's flusher.
- Do not block: everything after the flush phase, including the next wait, is stalled.
- Prefer a flusher over doing work directly in a completion handler when the work touches state
  that other completions in the same batch may also touch.

## Discard

```c
void DiscardFastRingDescriptor(struct FastRingDescriptor* descriptor, HandleFastRingCompletionFunction function);
```

Detaches a raw descriptor from its owner during teardown. It claims the descriptor the way the
submission loop does, replaces the handler with `function` (`NULL` drops it) and clears `closure`
under that claim, and then rewrites a
submission that has not reached the kernel as `IORING_OP_NOP`, submits `IORING_OP_ASYNC_CANCEL`
for an operation in the kernel, or releases a descriptor that was allocated and never submitted.
In any other state it does nothing. The references of the operation are accounted inside; do not
release them.

- Expects a descriptor carrying a single operation; not for event receivers, and not for a
  descriptor whose queued submission is an update of an earlier operation.
- A replacement handler only destroys what the discard itself cannot: whatever a late completion
  still delivers, a connection an accept returns after the release for example. It sees the NOP
  or the cancellation, both with `RING_DESC_OPTION_IGNORE`, and the operation's own completion if
  it arrives, and it uses nothing but the descriptor and the completion: the owner is gone, so
  there is no closure. Like any handler it returns non-zero while `IORING_CQE_F_MORE` is set; a
  `0` on a multishot completion would drop a reference the operation still needs.
- Callable from the ring thread, or from another thread that holds a reference of its own,
  taken while the descriptor was known to be alive and dropped after the call.

The semantics and the reasons not to do it by hand are in
[Cancellation](LIFECYCLE.md#raw-descriptors-discardfastringdescriptor).

## Poll

```c
int AddFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
int UpdateFastRingPoll(struct FastRing* ring, int handle, uint64_t flags);
int RemoveFastRingPoll(struct FastRing* ring, int handle);
void DestroyFastRingPoll(struct FastRing* ring, HandleFastRingPollFunction function, void* closure);
int SetFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
struct FastRingDescriptor* GetFastRingPollDescriptor(struct FastRing* ring, int handle);

typedef void (*HandleFastRingPollFunction)(int handle, uint32_t flags, void* closure, uint64_t options);
```

A poll is addressed by the file descriptor number; the ring keeps one entry per handle. The
callback receives the handle, the returned `poll` events, the closure, and the flags the poll is
armed with.

`flags` combines `poll` events in the low 32 bits with options in the high bits:

| Flag | Meaning |
| --- | --- |
| `RING_POLL_READ`, `RING_POLL_WRITE`, `RING_POLL_ERROR`, `RING_POLL_HANGUP` | `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP` |
| `RING_POLL_SHOT` | One-shot request. Without it the request is a kernel multishot poll (`IORING_POLL_ADD_MULTI`) |
| `RING_POLL_REPEAT` | Arm the request again after a callback that ended it, unless the poll was removed or the events contain `POLLERR` or `POLLHUP` |
| `RING_POLL_EDGE` | Documents edge triggering, which the kernel applies unless `IORING_POLL_ADD_LEVEL` is given (`io_poll_parse_events()`); it changes nothing in the default build |

Level-triggered polls (`IORING_POLL_ADD_LEVEL`) are compiled out because they do not work on
kernels up to 6.1 (liburing issue 829). Defining `USE_RING_LEVEL_TRIGGERING` makes every poll
level-triggered and `RING_POLL_EDGE` zero.

| Function | Returns |
| --- | --- |
| `AddFastRingPoll()` | `0`, or `-ENOMEM` (also for a negative handle or a `NULL` ring). An existing entry of the handle is overwritten, not removed |
| `UpdateFastRingPoll()` | `0`; `-EBADF` when the handle has no entry or is being removed; `-EBUSY` when a claim failed outside a callback |
| `RemoveFastRingPoll()` | `0`, or `-EBADF` when the handle has no entry |
| `SetFastRingPoll()` | Removes when `flags == 0`, updates an existing entry, adds a missing one |
| `DestroyFastRingPoll()` | Removes every entry whose function and closure both match |
| `GetFastRingPollDescriptor()` | The descriptor of the entry with a reference taken for the caller, or `NULL`; release it with `ReleaseFastRingDescriptor()` |

Removal is asynchronous; see [Poll and Watch](LIFECYCLE.md#poll-and-watch).

## Watch

```c
struct FastRingDescriptor* AddFastRingWatch(struct FastRing* ring, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);
void UpdateFastRingWatch(struct FastRingDescriptor* descriptor, uint32_t mask);
void RemoveFastRingWatch(struct FastRingDescriptor* descriptor);
struct FastRingDescriptor* SetFastRingWatch(struct FastRing* ring, struct FastRingDescriptor* descriptor, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);

typedef void (*HandleFastRingWatchFunction)(struct FastRingDescriptor* descriptor, int result);
```

A watch is addressed by its descriptor, so several watches may exist for one handle. `mask` is a
`poll` event mask; `flags` is masked with `IORING_POLL_ADD_MULTI | IORING_POLL_ADD_LEVEL` and
passed to the kernel as is, so a watch is one-shot by default and re-armed by its handler after
every callback. `result` is the returned event mask or a negative error code; the closure is
`descriptor->closure`.

- `AddFastRingWatch()` returns `NULL` for a negative handle, a zero mask, a `NULL` ring or
  function, or when no descriptor can be allocated.
- `UpdateFastRingWatch()` ignores a zero mask and an unchanged one.
- `RemoveFastRingWatch()` may be called from the callback; after it returns, the pointer must not
  be used.
- `SetFastRingWatch()` removes when `mask == 0` and returns `NULL`, updates when a descriptor is
  given, and adds otherwise.

## Timeout

```c
struct FastRingDescriptor* SetFastRingTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, int64_t interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingCertainTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timeval* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingPreciseTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timespec* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);

typedef void (*HandleFastRingTimeoutFunction)(struct FastRingDescriptor* descriptor);
```

The three functions differ only in how the interval is expressed: milliseconds, `struct timeval`,
`struct timespec`.

- `descriptor == NULL` creates and arms a timeout and returns it, or `NULL` when `ring` is `NULL`
  or no descriptor can be allocated.
- An existing `descriptor` with a valid interval changes the interval and re-arms; it returns
  `descriptor`. A timeout that has already fired is armed anew.
- An existing `descriptor` with a negative interval (`SetFastRingTimeout`) or a `NULL` one (the
  other two) removes the timeout and returns `NULL`, which is meant to be assigned back over the
  stored pointer.
- `function` and `closure` are used only at creation. The callback runs only on expiry; the
  closure is `descriptor->closure`.

`flags`:

| Flag | Meaning |
| --- | --- |
| `TIMEOUT_FLAG_REPEAT` | Fire every `interval` until removed. Equals `IORING_TIMEOUT_MULTISHOT` when the liburing headers define it, which needs Linux 6.4 at run time; otherwise bit 62 and a re-arm by the handler. See [Repeating timeouts](LIFECYCLE.md#repeating-timeouts) |
| `TIMEOUT_FLAG_OWNED` | Bit 63. The descriptor keeps a reference for the owner and survives its expiry; removal becomes mandatory. See [Ownership](LIFECYCLE.md#ownership-of-a-timeout-descriptor) |
| `IORING_TIMEOUT_ABS`, `IORING_TIMEOUT_BOOTTIME`, `IORING_TIMEOUT_REALTIME` | Passed to the kernel: absolute time, clock selection |

Without `TIMEOUT_FLAG_OWNED` a timeout that fired and was not re-armed is released after its
callback; the owner clears its pointer in the callback.

## Event

```c
struct FastRingDescriptor* CreateFastRingEvent(struct FastRing* target, HandleFastRingCompletionFunction function, void* closure);
int SubmitFastRingEvent(struct FastRing* source, struct FastRingDescriptor* event, uint32_t parameter, int option);
```

FastRing Event is ring-to-ring messaging over `IORING_OP_MSG_RING`.

- `CreateFastRingEvent(target, ...)` creates a receiver on the target ring: a prepared descriptor
  that is never submitted and stays `RING_DESC_STATE_PENDING`. Its handler runs on the target
  ring thread with the message as `completion`: `parameter` in `completion->res`, `option` in the
  option bits of `completion->user_data`. Return non-zero to keep the receiver for further
  messages, `0` to release it; or release it with `ReleaseFastRingDescriptor()`. Do not discard
  it. Returns `NULL` when no descriptor can be allocated.
- `SubmitFastRingEvent(source, event, ...)` is called on the source ring thread. It queues a
  temporary sending descriptor with `IORING_OP_MSG_RING` on `source`, which the next
  `WaitForFastRing(source)` submits and its completion releases. Returns `0`, or `-EINVAL` when
  `event` is `NULL` or no descriptor can be allocated.

See [Events Between Rings](LIFECYCLE.md#events-between-rings).

## Buffer Provider

```c
typedef void* (*CreateRingBufferFunction)(size_t length, void* closure);
typedef void (*ReleaseRingBufferFunction)(void* buffer);

struct FastRingBufferProvider* CreateFastRingBufferProvider(struct FastRing* ring, uint16_t group, uint16_t count, uint32_t length, CreateRingBufferFunction function, void* closure);
struct FastRingBufferProvider* HoldFastRingBufferProvider(struct FastRingBufferProvider* provider);
void ReleaseFastRingBufferProvider(struct FastRingBufferProvider* provider, ReleaseRingBufferFunction function);

void PrepareFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_sqe* submission);
uint8_t* GetFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion);
void AdvanceFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion, CreateRingBufferFunction function, void* closure);

uint16_t GetFastRingBufferGroup(struct FastRing* ring);
```

A provider is a ring-mapped buffer ring (`io_uring_register_buf_ring()`, Linux 5.19) filled with
buffers of one length.

- `CreateFastRingBufferProvider()` registers the ring and fills every slot with
  `function(length, closure)`. `group == 0` takes a new group id from `GetFastRingBufferGroup()`;
  `count == 0` takes the number of CQ entries; the count is capped at 32768 and rounded up to a
  power of two. Returns `NULL` on allocation or registration failure. `FastBuffer` supplies a
  ready pair of functions, `AllocateRingFastBuffer()` and `ReleaseRingFastBuffer()`.
- `PrepareFastRingBuffer()` sets `IOSQE_BUFFER_SELECT` and the group on an SQE.
- `GetFastRingBuffer()` maps the buffer id of a completion to the buffer address, or returns
  `NULL` when the completion carries no buffer.
- `AdvanceFastRingBuffer()` gives the slot back to the kernel. With `function == NULL` the same
  buffer is reused; with a `function` a new buffer takes the slot and the old one passes to the
  caller. When `function` returns `NULL` the slot stays out of the ring for good, so the provider
  has one buffer less. Completions without a buffer are ignored.
- `HoldFastRingBufferProvider()` takes a reference for a receiver whose operations select
  buffers from the provider, and returns it. `FastSocket` and `FastBIO` hold their provider
  until their last receive completion.
- `ReleaseFastRingBufferProvider()` drops a reference. When the last one goes, the group is
  unregistered and `function` of that call is called for every buffer in the provider; `NULL`
  leaves the buffers to the caller. Every holder therefore passes the function that matches the
  buffers — `FastSocket` and `FastBIO` pass `ReleaseRingFastBuffer()`, as they work with
  FastBuffer-backed providers only.
- `GetFastRingBufferGroup()` returns the next group id of the ring, starting at 1.

## Registered Files and Buffers

```c
int AddFastRingRegisteredFile(struct FastRing* ring, int handle);
void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle);
int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length);
int UpdateFastRingRegisteredBuffer(struct FastRing* ring, int index, void* address, size_t length);
```

- `AddFastRingRegisteredFile()` returns the fixed-file index for `IOSQE_FIXED_FILE`. Registrations
  are reference counted per handle: the same handle returns the same index. Errors: `-EBADF` for a
  negative handle or a `NULL` ring, `-ENOMEM`, `-EOVERFLOW` when the lower half of the table is
  full, or the kernel's error.
- `RemoveFastRingRegisteredFile()` drops one registration and clears the slot with the last one.
- `AddFastRingRegisteredBuffer()` registers a fixed buffer and returns its index. The first call
  registers a sparse table of 16384 entries. Errors: `-EINVAL` for a `NULL` address or ring,
  `-ENOMEM`, `-EOVERFLOW` when the table is full, or the kernel's error.
- `UpdateFastRingRegisteredBuffer()` replaces the buffer at `index` and returns `index`; with
  `address == NULL` it unregisters the slot and returns `INT32_MIN`, and the slot is free again
  even if the kernel refused, as it does off the ring thread. A refused replacement leaves the
  slot as it was. `-EINVAL` for a bad index or a ring without a table.

All four are ring-thread calls: the kernel refuses registration from other threads.

## Tracing

```c
typedef void (*TraceFastRingFunction)(int action, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason, void* closure);

struct FastRingTrace
{
  void* closure;
  TraceFastRingFunction function;
};
```

Setting `ring->trace` makes the ring report every completion handled
(`RING_TRACE_ACTION_HANDLE`, before the handler, stale completions included) and every descriptor
recycled by a completion (`RING_TRACE_ACTION_RELEASE`). There is no setter function: assign the
field on the ring thread. It is a debug facility and adds a branch on the completion path.

## Limits and Constants

| Constant | Value | Meaning |
| --- | --- | --- |
| Submission queue | 16384 | Maximum `length` of `CreateFastRing()` (kernel `IORING_MAX_ENTRIES`) |
| Completion queue | 4 × SQ | `RING_COMPLETION_RATIO` |
| Fixed-file table | `RLIMIT_NOFILE / 2`, at most `1 << 20` | Lower half for `AddFastRingRegisteredFile()`, upper half for the kernel |
| Fixed-buffer table | 16384 | `AddFastRingRegisteredBuffer()` |
| Buffer ring | 32768 | Maximum `count` of a provider |
| `RING_DESC_ALIGNMENT` | 512 | Descriptor alignment and size class |
| `sizeof(union FastRingData)` | 256 | Owner storage in a descriptor |
| `RING_FLUSH_ALIGNMENT` | 64 | Flusher alignment |
| `RING_DATA_UNDEFINED` | `LIBURING_UDATA_TIMEOUT - 1` | `user_data` at or above it is not a descriptor |
| `RING_DATA_ADDRESS_MASK` | | Extracts the descriptor address from `user_data` |
| `RING_DESC_INTEGRITY_MASK` | `0x0fff00000000003f` | Tag bits of `user_data` |
| `RING_POLL_FLAGS_SHIFT` | 32 | Position of the poll options in `flags` |
| `RING_TRACE_ACTION_HANDLE`, `RING_TRACE_ACTION_RELEASE` | 0, 1 | Trace actions |

A descriptor holds the bookkeeping, a copy of the SQE with room for `IORING_SETUP_SQE128`, and
`data`; with the `malloc` header it takes about one 512-byte block. On aarch64 the library calls
`prctl(PR_SET_TAGGED_ADDR_CTRL, 0)` at load time, which turns the tagged address ABI off for the
process.
