# FastRing API Reference

This document describes the public C API from `Ring/FastRing.h`.

Ownership, reference counting, reentrancy and shutdown rules are documented
separately in `Documentations/Lifecycle.md`.

## Conventions

- Integer return codes use `errno` style:
  - success: `0` or positive value
  - error: negative code (`-EINVAL`, `-ENOMEM`, `-EBADF`, ...)
- `FastRingDescriptor` objects are reference-counted.
- Asynchronous completions are delivered while `WaitForFastRing()` is running.
- Callbacks run in the ring-processing thread.

## Threading Model

- `CreateFastRing()` captures the processing thread id.
- `WaitForFastRing()` should run in the owner thread loop.
- The ring is created with `IORING_SETUP_SINGLE_ISSUER`: only the owner thread may
  enter the kernel through it.

```c
int IsFastRingThread(struct FastRing* ring);
```

Returns `1` when called from the ring thread, `0` from any other thread, and `-1` if
the ring has no recorded thread. See `Documentations/Lifecycle.md` for the full table
of which calls are allowed from which thread.

## Lifecycle

```c
struct FastRing* CreateFastRing(uint32_t length);
void ReleaseFastRing(struct FastRing* ring);
int WaitForFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask);
```

- `CreateFastRing(length)`:
  - `length == 0` means auto-size from `RLIMIT_NOFILE`.
  - queue length is rounded to power-of-two and capped at `16384`.
  - the completion queue is sized at 4x the submission queue.
  - registers a sparse file table of `RLIMIT_NOFILE / 2` entries (capped at `1 << 20`).
  - returns `NULL` on init failure.
- `ReleaseFastRing()`:
  - releases ring resources, descriptors, flush handlers, registered file/buffer metadata.
  - calls every live handler with `RING_REASON_RELEASED` first; see `Documentations/Lifecycle.md`.
- `WaitForFastRing(interval_ms, mask)`:
  - submits pending SQEs, handles CQEs, optionally waits.
  - `interval` is milliseconds.
  - `mask` is passed to `io_uring_submit_and_wait_timeout`.
  - returns `0` on timeout (`-ETIME` is normalized), negative on error.
  - does not wait when there is at least one SQE or CQE ready to be processed.

In C++, `CreateSharedFastRing(length = 0)` returns a
`std::shared_ptr<struct FastRing>` bound to `ReleaseFastRing`.

## Descriptor API

```c
struct FastRingDescriptor* AllocateFastRingDescriptor(
  struct FastRing* ring,
  HandleFastRingCompletionFunction function,
  void* closure);

void PrepareFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last);
int ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor);
```

- `AllocateFastRingDescriptor()` initializes descriptor with `IORING_OP_NOP`, refcount `1`.
- `PrepareFastRingDescriptor()` generates `user_data` and marks descriptor pending.
- `SubmitFastRingDescriptor()` prepares and enqueues one descriptor.
- `SubmitFastRingDescriptorRange()` enqueues a prepared chain.
- `ReleaseFastRingDescriptor()` decrements references and recycles when count reaches `0`.
  It returns the remaining reference count, or `-1` for a `NULL` descriptor.
  Normally the release is performed automatically by the return value of the
  completion handler; call it explicitly only in the cases described in
  `Documentations/Lifecycle.md`.

To abandon a descriptor whose operation may already be in flight, use the
[Discard API](#discard-api) rather than releasing it by hand.

### Descriptor states

`descriptor->state` holds one of `RING_DESC_STATE_FREE`, `RING_DESC_STATE_ALLOCATED`,
`RING_DESC_STATE_PENDING`, `RING_DESC_STATE_LOCKED`, `RING_DESC_STATE_SUBMITTED`.
Transitions and their meaning are described in `Documentations/Lifecycle.md`.

`descriptor->lock` sits next to it and covers what the state cannot: bit
`RING_DESC_LOCK_HELD` says that a completion handler is running, and the bits above it
count writers waiting to claim the state. The two words are the first two of the three
layers of exclusion a descriptor carries; how they are taken, in which order, and how the
per-API `condition` word relates to them is described in `Documentations/Lifecycle.md`.

### Submission options

The `option` argument of `PrepareFastRingDescriptor()`, `SubmitFastRingDescriptor()`
and `SubmitFastRingEvent()` is masked with `RING_DESC_OPTION_MASK` and encoded into
`user_data`, so it is returned in `completion->user_data`:

- `RING_DESC_OPTION_IGNORE` - marks a housekeeping completion (cancel, poll update,
  timeout update, or a NOP replacing a cancelled request). Built-in Poll, Watch and
  Timeout handlers do not invoke the user callback for such completions.
- `RING_DESC_OPTION_USER1`, `RING_DESC_OPTION_USER2` - available to modules to
  distinguish several submissions sharing one descriptor and one handler.

Pass `0` for a normal submission.

### Completion callback

```c
int (*HandleFastRingCompletionFunction)(
  struct FastRingDescriptor* descriptor,
  struct io_uring_cqe* completion,
  int reason);
```

Completion reasons:
- `RING_REASON_COMPLETE`
- `RING_REASON_INCOMPLETE` (chained SQE that will never run; no longer produced by the
  core, see `Documentations/Lifecycle.md`)
- `RING_REASON_RELEASED`

Return `0` to drop one reference, non-zero when the descriptor has been re-armed or a
multishot request is still active. `completion` is `NULL` for `RING_REASON_RELEASED`
and for chained descriptors completed through `IOSQE_CQE_SKIP_SUCCESS`.

### Condition flags

Poll, Watch and Timeout descriptors keep a `condition` word in `descriptor->data`
carrying `RING_CONDITION_GUARD` and `RING_CONDITION_REMOVE`, which is what makes it safe
to update or remove an operation from inside its own callback. `RING_CONDITION_UPDATE`
marks an operation that ended without being re-armed, so that an update arms it anew
instead of asking to change an entry the kernel no longer has; Poll and Timeout raise it,
Watch never does because its handler always re-arms. `RING_CONDITION_MASK` covers all
three. See `Documentations/Lifecycle.md`.

## Flush Handlers

```c
typedef void (*HandleFastRingFlushFunction)(void* closure, int reason);

struct FastRingFlusher* SetFastRingFlushHandler(
  struct FastRing* ring,
  HandleFastRingFlushFunction function,
  void* closure);

int RemoveFastRingFlushHandler(struct FastRing* ring, struct FastRingFlusher* flusher);
```

A flusher is a **one-shot callback that runs at the end of the current loop
iteration**, after every CQE of this pass has been handled. It is the mechanism for
"do this once more, but not right now": batching work that several completions would
otherwise each trigger, and getting off a completion handler's stack before touching
shared state.

That is the whole of it. A flusher is a phase of the loop, not a task queue, not a
notification and not a general cancellable job — anything beyond "apply what this pass
has accumulated, once, before the loop waits again" is a misuse of it. A synchronous
call from an arbitrary foreign thread is `ThreadCall`, which supplies the wakeup,
generation, reference count and join protocol such a thing needs. A cross-ring handoff
from a ring that is already making progress is `SubmitFastRingEvent()`. Neither belongs
here.

### When it runs

```
WaitForFastRing()
  submit pending SQEs
  handle CQEs           <-- completion handlers run here, may arm flushers
  run flush handlers    <-- here, until none are left
```

The drain is a `while` loop over the pending stack, so **a flusher armed from inside a
flusher runs in the same iteration**, not the next one. That is what lets a module
re-arm itself to make repeated progress — `DBusCore` dispatches one D-Bus message per
pass and re-arms until the queue is empty, all inside one `WaitForFastRing()` call.
Re-arming unconditionally is therefore an infinite loop: always stop on a completion
condition.

Handlers are kept on a stack, so within one iteration they run in reverse arming
order. Do not depend on ordering between independent modules.

### One-shot, and the idempotent-arm idiom

Being called returns the flusher to the free list, so each `SetFastRingFlushHandler()`
produces exactly one call. Modules that only want "one pass per iteration" keep the
handle and check it, which is why the pattern below appears throughout the codebase:

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

`CURLWSCore`, `DBusCore`, `FastGLoop`, `FastUVLoop` and `Fetch` all use exactly this
shape.

### Reasons

- `RING_REASON_COMPLETE` — the normal end-of-iteration call.
- `RING_REASON_RELEASED` — `ReleaseFastRing()` is tearing the ring down. Both the
  pending and the free stacks are walked, so a still-armed flusher gets this call
  before its memory goes away. Release module state here; do not submit anything.

Always branch on `reason`: a handler that does its work unconditionally will run it
again during shutdown.

### Cancelling

`RemoveFastRingFlushHandler()` cancels an armed flusher by changing its state from
pending to free:

- `0` — cancelled, the handler will not be called;
- `-EPERM` — too late, the flusher is already running or has already run;
- `-EBADF` — `ring` or `flusher` is `NULL`.

A cancelled flusher is not recycled immediately; the drain loop notices the changed
state, skips the call and reclaims it.

**A handle stops being a handle once the flusher has run.** The object goes back to the
free list and the next `SetFastRingFlushHandler()` may hand it to somebody else, in which
case its state is `RING_FLUSH_STATE_PENDING` again. The cancellation looks at nothing but
that state, so it succeeds and quietly cancels the **new** owner's flusher — and returns
`0` while doing it. Clear the handle in the handler, as above, and cancel only handles you
know are still armed. Within the ring thread that is a matter of discipline rather than of
timing, since nobody else can arm or run a flusher meanwhile.

### Rules

- **The whole facility belongs to the ring thread**, arming included. Both calls are
  lock-free and will not corrupt anything if called from elsewhere, but arming from
  another thread does not wake the ring: nothing is submitted and no event is signalled,
  so the handler waits for the wait to expire or for an unrelated completion to arrive.
  Use `ThreadCall` for a call from an arbitrary foreign thread. `SubmitFastRingEvent()`
  is useful for a cross-ring handoff, but only wakes its target after the source ring has
  submitted the queued `MSG_RING` SQE. And a handle cannot be owned by a foreign thread
  at all: the flusher may run and be recycled before that thread has finished storing the
  pointer, after which every use of it lands on somebody else's flusher. In the ring
  thread none of this arises, because "has it run yet" is answered by the handler itself
  clearing the field.
- Do not block: everything after the flush phase, including the next wait, is stalled.
- Prefer a flusher over doing work directly in a completion handler when the work
  touches state that other completions in the same batch may also touch.

## Discard API

```c
void DiscardFastRingDescriptor(struct FastRingDescriptor* descriptor);
```

Detaches a descriptor from its owner during teardown. It is the counterpart of an
explicit `ReleaseFastRingDescriptor()` for the case where the operation may already
have been handed to the kernel: the owner is going away, but the descriptor cannot be
recycled until io_uring is done with it.

What it does, in this order:

- claims the descriptor — a CAS for a submission still sitting in the pending queue, a
  wait for one the submission loop has already copied into the ring, and in either case
  a reservation that keeps a completion handler out until the claim is made;
- under that claim, clears `function` and `closure`, so no completion reaches the owner
  any more and neither the submission loop nor a handler observes a half-updated
  descriptor;
- then either
  - rewrites the still-queued submission in place as `IORING_OP_NOP`, when it has not
    reached the kernel yet, or
  - submits an `IORING_OP_ASYNC_CANCEL`, when the operation is already in flight. That
    covers timeouts as well: `io_try_cancel()` falls through to `io_timeout_cancel()`.

If neither claim succeeds the descriptor carries no operation at all. One that was
allocated but never submitted is released outright — no completion will ever arrive to
do it, and leaving it in `RING_DESC_STATE_ALLOCATED` would lose the slot for good. In
any other state the call does nothing.

**Hold a reference if you call it from another thread.** The claim covers both writers a
descriptor has: the submission loop, which takes the state with the same primitives, and
a running completion handler, which `HandleCompletedRingDescriptor()` now shuts out
through the descriptor lock. What the claim cannot cover is the pointer itself — a
completion that drops the last reference recycles the descriptor and hands it to the next
owner, and no claim taken afterwards means anything. On the ring thread that cannot
happen, because the same thread reaps completions; from anywhere else the caller has to
keep a reference of its own for the whole call.

Even so, the function earns its place: a plain check of `descriptor->state` followed by a
resubmission is a race against the submission loop, and resubmitting a descriptor that is
still queued links the pending list onto itself and stalls the ring for good.

Tearing down from another thread needs one thing more than this helper: a reference of
the caller's own, so the descriptor cannot be recycled mid-call. Shutting out a handler
that is already running used to be the caller's job as well — that is what the
`RING_CONDITION_GUARD` / `RING_CONDITION_REMOVE` protocol does for the Poll, Watch and
Timeout APIs — but the descriptor lock now does it for every descriptor; see
`Documentations/Lifecycle.md`.

Reference accounting inside the call is handled internally. The queued-submission path
consumes the reference the pending entry already holds; the cancellation path takes one
more, which the cancellation completion drops, while the original operation completes
with `-ECANCELED` and drops the last one. Do not add a `ReleaseFastRingDescriptor()` for
any of those.

A reference the **caller** took to keep the descriptor alive across the call — the one a
foreign thread has to hold, see above — is not among them and stays the caller's to drop.
Release it right after the call returns.

The function expects a descriptor carrying a **single** operation. If a queued
submission is not the descriptor's own operation but an update prepared for an earlier
one — the pattern `UpdateFastRingWatch()` uses — rewriting it as a `NOP` would leave
that earlier operation armed. Subsystems that keep updates on separate transient
descriptors, which is the usual arrangement, are unaffected.

## Poll API

```c
int AddFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
int UpdateFastRingPoll(struct FastRing* ring, int handle, uint64_t flags);
int RemoveFastRingPoll(struct FastRing* ring, int handle);
void DestroyFastRingPoll(struct FastRing* ring, HandleFastRingPollFunction function, void* closure);
int SetFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
struct FastRingDescriptor* GetFastRingPollDescriptor(struct FastRing* ring, int handle);
```

Poll callback:

```c
void (*HandleFastRingPollFunction)(int handle, uint32_t events, void* closure, uint64_t options);
```

Flag helpers:
- `RING_POLL_READ`, `RING_POLL_WRITE`, `RING_POLL_ERROR`, `RING_POLL_HANGUP`
- high-bit behavior: `RING_POLL_EDGE`, `RING_POLL_SHOT`
- `RING_POLL_REPEAT` - re-arm the request in userspace after each callback, unless it
  was removed, completed as a kernel multishot (`IORING_CQE_F_MORE`), or terminated by
  `POLLERR` / `POLLHUP`

`RING_POLL_EDGE` maps to `IORING_POLL_ADD_LEVEL` and is compiled out by default:
level triggering is broken on kernels up to 6.1 (liburing issue 829). Define
`USE_RING_LEVEL_TRIGGERING` to enable it.

Behavior notes:
- One poll registration per file descriptor. `AddFastRingPoll()` on a descriptor that
  already has one overwrites the table entry.
- `SetFastRingPoll()` combines the three: `flags == 0` removes, an existing
  registration is updated, and a missing one (`-EBADF`) is added.
- `UpdateFastRingPoll()` returns `-EBADF` when there is no registration, `-EBUSY`
  when the descriptor could not be locked.
- `DestroyFastRingPoll()` removes every registration matching both `function` and
  `closure`. Use it to tear down all descriptors owned by one object.
- `GetFastRingPollDescriptor()` returns `NULL` if the descriptor was recycled for a
  different purpose. What it does return comes with a reference taken on the caller's
  behalf, so the pointer stays valid even if the registration is removed a moment later —
  which is what makes the call usable from another thread at all. Release it with
  `ReleaseFastRingDescriptor()` when done, or it is a leak.
- Removal is asynchronous: a cancellation request is submitted and the descriptor is
  freed when it completes.

## Watch API

```c
struct FastRingDescriptor* AddFastRingWatch(struct FastRing* ring, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);
void UpdateFastRingWatch(struct FastRingDescriptor* descriptor, uint32_t mask);
void RemoveFastRingWatch(struct FastRingDescriptor* descriptor);
struct FastRingDescriptor* SetFastRingWatch(struct FastRing* ring, struct FastRingDescriptor* descriptor, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);
```

Watch callback:

```c
void (*HandleFastRingWatchFunction)(struct FastRingDescriptor* descriptor, int result);
```

- Unlike the Poll API, a watch is addressed by its descriptor, not by the file
  descriptor, so several watches may exist for one handle.
- `flags` is masked with `IORING_POLL_ADD_MULTI | IORING_POLL_ADD_LEVEL`.
- A non-multishot watch is re-armed automatically after each callback.
- `SetFastRingWatch()` removes when `mask == 0`, updates when a descriptor is given,
  and adds otherwise.

## Timeout API

```c
struct FastRingDescriptor* SetFastRingTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, int64_t interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingCertainTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timeval* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingPreciseTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timespec* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure);
```

Timeout callback:

```c
void (*HandleFastRingTimeoutFunction)(struct FastRingDescriptor* descriptor);
```

- The three functions differ only in how the interval is expressed: milliseconds,
  `struct timeval`, `struct timespec`.
- `TIMEOUT_FLAG_REPEAT` enables a repeating timeout. It maps to
  `IORING_TIMEOUT_MULTISHOT` where the kernel supports it, and to a userspace re-arm
  otherwise.
- `flags` is also passed to `io_uring_prep_timeout()`, so `IORING_TIMEOUT_ABS` and the
  clock selection flags apply.
- Create by passing `descriptor == NULL`; update by passing an existing descriptor;
  remove by passing a negative interval (`SetFastRingTimeout`) or `NULL` interval
  (the other two). Removal returns `NULL`, which is meant to be assigned back over
  the stored pointer.
- `function` and `closure` are only used at creation time.

### Ownership of a timeout descriptor

By default a terminal timeout that was not re-armed is released by its own completion: the
descriptor is gone by the time the callback returns, and the owner is expected to forget the
pointer inside that callback — every in-tree user does exactly that. A repeating timeout, and
one the callback re-arms through `SetFastRingTimeout()`, survive instead, because the new
operation takes a reference of its own. Nothing may touch such a descriptor from another
thread either way, since there is no telling from outside whether it still exists.

`TIMEOUT_FLAG_OWNED`, passed at creation, switches the model. The descriptor takes a
reference on the owner's behalf and therefore outlives its own completion, so the stored
pointer stays valid: the timeout can be re-armed through it after firing, and it can be
touched from another thread. In exchange the removal becomes mandatory — a negative or
`NULL` interval — and a descriptor that is never removed is a leak. Concurrent duplicate
removals are idempotent and join a running callback; after the first removal has returned,
however, the pointer is no longer a handle and must not be used again. By the time removal
returns no callback is running any more — except when it was issued from inside the callback
itself, which is still on the stack and only returns to the ring afterwards.

The flag lives above the 32 bits the kernel reads and never reaches an SQE.

## Event API

```c
struct FastRingDescriptor* CreateFastRingEvent(struct FastRing* target, HandleFastRingCompletionFunction function, void* closure);
int SubmitFastRingEvent(struct FastRing* source, struct FastRingDescriptor* event, uint32_t parameter, int option);
```

FastRing Event is **ring-to-ring messaging**. `CreateFastRingEvent(target, ...)` creates
the receiving descriptor on the target ring. The descriptor is not submitted as an SQE;
its ring and identifier describe where future `MSG_RING` requests have to deliver their
CQEs, and its handler therefore runs in the target ring thread.

`SubmitFastRingEvent(source, event, ...)` is called from the sending loop with a pointer
to that receiving descriptor. It allocates a temporary sending descriptor on the local
`source` ring, prepares its `IORING_OP_MSG_RING` SQE from the receiving descriptor and
submits it through the source. The sending descriptor is released by its own completion.
The kernel posts the message CQE to the target ring, waking it if necessary; `parameter`
arrives there as `completion->res`, and `option` in the target user data.

The receiving descriptor is not inherently persistent or one-shot. Its handler follows
the ordinary descriptor contract: return non-zero to keep using the same receiver for
further messages, or `0` to release it after this message. It may also be released
explicitly with `ReleaseFastRingDescriptor()` when its owner is done with it.

It stays in `RING_DESC_STATE_PENDING` for its whole life, and deliberately so:
`PrepareFastRingDescriptor()` hands out an identifier without queueing anything, which is
exactly what a receiver needs and exactly what that state describes. It is never submitted,
so it never reaches `RING_DESC_STATE_SUBMITTED` — that one would claim the kernel owns a
request, and there is no request. `DiscardFastRingDescriptor()` is therefore the wrong way
to dispose of it: it expects a descriptor that carries an operation, finds none to cancel
and leaves the last reference behind.

This is not a generic foreign-thread wakeup. Calling `SubmitFastRingEvent()` from an
arbitrary thread only places `MSG_RING` in the source ring's MPSC submission queue; it
cannot wake that source ring so that it submits the request. Use `ThreadCall` when the
caller is not itself operating a source ring.

- Returns `0`, or `-EINVAL` when the event is `NULL` or a descriptor cannot be
  allocated.

## Buffer Provider API

```c
struct FastRingBufferProvider* CreateFastRingBufferProvider(
  struct FastRing* ring,
  uint16_t group,
  uint16_t count,
  uint32_t length,
  CreateRingBufferFunction function,
  void* closure);

void ReleaseFastRingBufferProvider(struct FastRingBufferProvider* provider, ReleaseRingBufferFunction function);

void PrepareFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_sqe* submission);
uint8_t* GetFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion);
void AdvanceFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion, CreateRingBufferFunction function, void* closure);

uint16_t GetFastRingBufferGroup(struct FastRing* ring);
```

- `group == 0` allocates a group id through `GetFastRingBufferGroup()`.
- `count == 0` defaults to the number of CQ entries; the value is rounded up to a
  power of two.
- `PrepareFastRingBuffer()` sets `IOSQE_BUFFER_SELECT`.
- `GetFastRingBuffer()` maps CQE buffer id to address, `NULL` when the completion
  carries no buffer.
- `AdvanceFastRingBuffer()` returns consumed slot back to the ring. Passing a
  `function` installs a fresh buffer and transfers ownership of the old one to the
  caller; passing `NULL` recycles the same buffer immediately.
- `ReleaseFastRingBufferProvider()` calls `function` for every buffer; passing `NULL`
  leaves the buffers to the caller.

## Registered Files and Buffers

```c
int AddFastRingRegisteredFile(struct FastRing* ring, int handle);
void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle);
int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length);
int UpdateFastRingRegisteredBuffer(struct FastRing* ring, int index, void* address, size_t length);
```

- File registration returns fixed-file index (`>= 0`) or negative error
  (`-EBADF`, `-ENOMEM`, `-EOVERFLOW`). Registrations are reference counted per handle.
- Buffer registration returns buffer index (`>= 0`) or negative error.
- `UpdateFastRingRegisteredBuffer()` with `address == NULL` unregisters the slot and
  returns `INT32_MIN`, not the index.

## Tracing

```c
typedef void (*TraceFastRingFunction)(int action, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason, void* closure);

struct FastRingTrace
{
  void* closure;
  TraceFastRingFunction function;
};
```

Setting `ring->trace` makes the ring report every completion (`RING_TRACE_ACTION_HANDLE`)
and every descriptor recycle (`RING_TRACE_ACTION_RELEASE`). There is no setter
function: assign the field directly. This is a debug facility and adds a branch on the
completion hot path.

## Reserved user_data Values

`user_data` values at or above `RING_DATA_UNDEFINED` are not treated as descriptor
pointers and are skipped by the completion loop. `RING_DATA_ADDRESS_MASK` extracts the
descriptor address from `user_data`. Modules that submit raw SQEs outside the
descriptor API must keep their `user_data` in that reserved range.
