# Descriptor Lifecycle and Concurrency Rules

This document describes ownership, reference counting, reentrancy and thread rules
for `FastRing`. It covers the invariants that are enforced by `Ring/FastRing.c` but
are not visible from the function signatures in `Documentations/FastRing.md`.

Read this before writing a module that submits its own SQEs or keeps a
`struct FastRingDescriptor*` across callbacks.

## Descriptor States

A descriptor is always in exactly one of the states stored in `descriptor->state`:

| State | Meaning |
| --- | --- |
| `RING_DESC_STATE_FREE` | On the free stack, owned by the ring, must not be touched |
| `RING_DESC_STATE_ALLOCATED` | Returned by `AllocateFastRingDescriptor()`, owned by the caller, SQE may be filled in |
| `RING_DESC_STATE_PENDING` | Prepared and queued for submission, SQE may still be patched in place |
| `RING_DESC_STATE_LOCKED` | Transient, another party is mutating the descriptor right now |
| `RING_DESC_STATE_SUBMITTED` | The SQE has been copied into the ring, the kernel owns the request |

Normal transitions:

```
AllocateFastRingDescriptor()   FREE      -> ALLOCATED
PrepareFastRingDescriptor()    ALLOCATED -> PENDING
WaitForFastRing()              PENDING   -> LOCKED -> SUBMITTED
completion, refcount reaches 0 SUBMITTED -> FREE
```

`LOCKED` is never observed by a caller that follows the API: it exists only inside
the compare-and-swap sequences used to patch a descriptor that may be concurrently
picked up by the submitting thread.

### Why the state matters

`UpdateFastRingPoll()`, `UpdateFastRingWatch()` and the timeout update path all try
to patch the descriptor in place first (`PENDING`), and fall back to submitting a
separate `POLL_UPDATE` / `TIMEOUT_UPDATE` request only if the descriptor has already
reached `SUBMITTED`. This is why an update issued before the next `WaitForFastRing()`
costs nothing, while an update issued after it costs an extra SQE/CQE pair.

## Reference Counting

`descriptor->references` counts everything that may still dereference the descriptor:

- `AllocateFastRingDescriptor()` sets the count to `1`. This is the *owner reference*.
- Every in-flight SQE that carries `descriptor->identifier` holds one reference.
- Long-living objects (Poll and Watch descriptors) take an extra reference at arm
  time, so that the API owner and the kernel request are counted separately.

A reference is dropped when:

- the completion handler returns `0` for a CQE that belongs to the descriptor, or
- `ReleaseFastRingDescriptor()` is called explicitly.

When the count reaches `0`, the descriptor is cleared (`closure`, `function`,
`previous`, `identifier`) and pushed back on the free stack.

### Handler return value

```c
int (*HandleFastRingCompletionFunction)(struct FastRingDescriptor* descriptor,
                                        struct io_uring_cqe* completion,
                                        int reason);
```

- return `0` — the handler is done with this completion; one reference is dropped.
- return non-zero — the handler has re-armed the descriptor (it submitted a new SQE
  for the same descriptor, or a multishot request is still active); the reference is
  retained.

Returning non-zero without an actual outstanding request leaks the descriptor for
the lifetime of the ring. Returning `0` while the descriptor is still armed lets the
descriptor be recycled under an in-flight request; the identifier check below turns
that into a silent drop instead of memory corruption, but the request is lost.

### Identifier check

`descriptor->identifier` is derived from the descriptor address plus a generation
tag, and is recomputed by every `PrepareFastRingDescriptor()`. On completion the ring
verifies

```c
(completion->user_data & ~RING_DESC_OPTION_MASK) == descriptor->identifier
```

If it does not match, the CQE belongs to a previous generation of a recycled
descriptor. The ring ignores it and does not touch the descriptor. This is the
backstop for premature releases, not a supported pattern.

## Submission Options

Both `PrepareFastRingDescriptor()` and `SubmitFastRingDescriptor()` take an `option`
argument. It is masked with `RING_DESC_OPTION_MASK` and stored in the low bits of
`user_data`, so it is delivered back in `completion->user_data`:

| Option | Meaning |
| --- | --- |
| `RING_DESC_OPTION_IGNORE` | Housekeeping completion: cancel, poll update, timeout update, or a NOP that replaced a cancelled request. Built-in Poll/Watch/Timeout handlers skip the user callback for such CQEs |
| `RING_DESC_OPTION_USER1` | Free for the module, carried through unchanged |
| `RING_DESC_OPTION_USER2` | Free for the module, carried through unchanged |

`SubmitFastRingEvent()` takes the same `option` argument and ORs it into the
`user_data` delivered through `msg_ring`.

Use `RING_DESC_OPTION_USER1` / `RING_DESC_OPTION_USER2` to distinguish two SQEs that
share one descriptor and one handler, instead of allocating a second descriptor.

## Reentrancy: Calling the API from a Callback

All Poll, Watch and Timeout callbacks run inside `WaitForFastRing()` in the ring
thread. Calling the corresponding `Update*` / `Remove*` / `Set*` function from inside
its own callback is explicitly supported and is the intended way to change or cancel
an operation on an event.

The mechanism is the `condition` word carried in the descriptor's `data` union:

| Condition | Meaning |
| --- | --- |
| `RING_CONDITION_GUARD` | Set while the user callback is running |
| `RING_CONDITION_UPDATE` | Poll only: the operation ended without being re-armed, so nothing of it is left in the kernel |
| `RING_CONDITION_REMOVE` | Removal was requested; no further user callbacks will be issued |

Consequences a module author has to rely on:

- **Remove from inside the callback is safe.** `RemoveFastRingPoll()`,
  `RemoveFastRingWatch()` and `SetFastRingTimeout(..., -1, ...)` set
  `RING_CONDITION_REMOVE`. The handler observes it after the callback returns and
  suppresses the re-arm, so the operation is not resurrected.
- **Update from inside the callback does not double-arm.** The update path claims the
  descriptor and submits the new request itself, which leaves the state no longer
  submitted; the handler checks that on its way out and gives the descriptor up instead
  of re-arming it. Leaving the work to the handler instead would lose the update
  whenever the completion carries `POLLERR` or `POLLHUP`, since it ends the operation
  in that case rather than arming it again.
- **After remove, no callback is delivered.** A CQE that arrives for an already
  removed operation is consumed without calling the user function.
- **Remove is asynchronous.** The owner reference is dropped immediately, but the
  descriptor stays alive until the cancellation CQE arrives. The closure outlives the
  call by less than that: once the removal returns, no callback will be delivered and
  none is running any more, because a claim made off the ring thread waits a running
  handler out. The one exception is a removal issued from inside the callback itself,
  where that handler is still on the stack and still using the closure.

`RemoveFastRingWatch()` and the timeout removal path spin over `LockPendingRingDescriptor()`
/ `LockSubmittedRingDescriptor()` until one of them succeeds. Off the ring thread each claim
first reserves the descriptor and sleeps on a futex until a running handler returns, so the
spinning is over states held by other writers only, and those are released without waiting
for anything. On the ring thread the reservation is skipped: handlers run there alone, and
waiting would mean waiting for the caller itself.

### Abandoning a raw descriptor

The rules above cover descriptors owned by the Poll, Watch and Timeout APIs. A module
that allocates descriptors itself and keeps them in its own structure has to abandon
them during teardown, and the only supported way to do that is
`DiscardFastRingDescriptor()` — see `Documentations/FastRing.md`.

Doing it by hand is a trap, twice over:

- Reading `descriptor->state` and acting on the result is a race whenever the owner can
  be torn down from a thread other than the ring thread. The submission loop may take
  the descriptor between the read and the write, so the operation ends up in the kernel
  uncancelled while its submission copy is overwritten.
- Submitting a descriptor that is still queued corrupts the pending list. The queue is
  an MPSC list whose push assumes the node is not already in it; re-pushing the tail
  makes it point at itself, the submission loop stops advancing, and **no SQE is ever
  submitted by that ring again**. The failure is silent — completions keep being reaped
  and flushers keep running, so it looks like a hang rather than a crash.

`DiscardFastRingDescriptor()` claims the state with the same primitives the submission
loop uses, and detaches the handler only after that claim, so neither hazard applies.

**Hold a reference when calling it from another thread.** The claim covers both writers:
the submission loop, and a completion handler, which `HandleCompletedRingDescriptor()`
shuts out through the descriptor lock. It does not and cannot cover the pointer — a
completion that drops the last reference returns the descriptor to the pool and hands it
to the next owner, and a claim taken after that means nothing. On the ring thread the two
cannot overlap, because the same thread reaps completions.

A raw descriptor owned by a module therefore has the same teardown rule as the built-in
APIs: abandon it from the ring thread, or keep a reference of the owner's own for the
duration of the call and drop it right after the call returns — the helper accounts for
the references the operation itself carries, not for that one. A guard of its own, the way `RING_CONDITION_GUARD` and
`RING_CONDITION_REMOVE` work above, is no longer needed for this — the descriptor lock
covers every descriptor, not only those of the built-in APIs.

## Thread Model

`CreateFastRing()` records `gettid()` of the calling thread. That thread is the ring
thread. The ring is created with `IORING_SETUP_SINGLE_ISSUER`, so only the ring thread
may enter the kernel through this ring.

| Operation | Thread |
| --- | --- |
| `WaitForFastRing()` | Ring thread only |
| `CreateFastRing()` / `ReleaseFastRing()` | Ring thread |
| All completion, poll, watch, timeout and flush callbacks | Ring thread |
| `AllocateFastRingDescriptor()`, `PrepareFastRingDescriptor()`, `SubmitFastRingDescriptor()` | Any thread (lock-free MPSC queue) |
| `SetFastRingFlushHandler()` / `RemoveFastRingFlushHandler()` | Any thread (lock-free stack) |
| Poll API, Registered File API, Registered Buffer API | Any thread (recursive mutex) |
| `DiscardFastRingDescriptor()` | Ring thread, or any thread holding a reference of its own |
| `SubmitFastRingEvent()` | Any thread, including another ring's thread |

```c
int IsFastRingThread(struct FastRing* ring);
```

Returns `1` in the ring thread, `0` in another thread, and `-1` if the ring has no
recorded thread. Use it to decide between a direct call and a `ThreadCall` hop.

Submitting a descriptor from a foreign thread only enqueues it. The SQE is copied
into the ring on the next `WaitForFastRing()`. If the ring thread is blocked in
`io_uring_submit_and_wait_timeout()`, it will not notice the new descriptor until the
timeout expires — wake it with `SubmitFastRingEvent()` or `ThreadCall`.

## Chained Descriptors

`SubmitFastRingDescriptorRange(first, last)` enqueues a prepared chain as a unit.
`descriptor->linked` holds the number of following descriptors, and the submitting
loop refuses to start a chain that does not fit into the remaining SQ space, so a
chain is never split across two `io_uring_enter()` calls.

For chains built with `IOSQE_CQE_SKIP_SUCCESS`, only the last descriptor produces a
CQE. When it completes, the ring walks the `previous` pointers backwards and calls
each preceding handler with `completion == NULL` and `reason == RING_REASON_COMPLETE`.
A handler that is used in such a chain must tolerate a `NULL` completion.

## Shutdown

`ReleaseFastRing()` performs, in order:

1. every pending flush handler is called with `RING_REASON_RELEASED`;
2. every allocated descriptor that is not `FREE` and has a handler is called with
   `(descriptor, NULL, RING_REASON_RELEASED)` and then freed;
3. registered file and buffer metadata is freed and `io_uring_queue_exit()` is called.

Rules for the `RING_REASON_RELEASED` path:

- `completion` is always `NULL`.
- The return value of the handler is ignored — the descriptor is freed either way.
- The handler must not submit new descriptors, and must not call back into the ring
  API. It exists to release the module's own resources (close file descriptors, free
  closures, complete outstanding user callbacks with an error).
- The order in which descriptors are released is the reverse allocation order of the
  heap, not any module-level order. A handler must not assume that objects it points
  to are still alive; release module state before releasing the ring.

`ReleaseFastRing()` does not drain the completion queue. Requests still owned by the
kernel are dropped by `io_uring_queue_exit()`.

## Completion Reasons

| Reason | Delivered when |
| --- | --- |
| `RING_REASON_COMPLETE` | Normal CQE, or a chained descriptor completed via `IOSQE_CQE_SKIP_SUCCESS` |
| `RING_REASON_INCOMPLETE` | Historical: a chained SQE that would never run because an earlier link failed. Not produced by the current core, see below |
| `RING_REASON_RELEASED` | The ring or the flusher set is being torn down |

### RING_REASON_INCOMPLETE

Up to commit `af9c11c` (March 2024) the completion loop produced this reason itself.
When a CQE for a member of an `IOSQE_IO_LINK` chain came back with a negative result,
the loop walked *forward* along `descriptor->next` and completed every remaining link
of the chain with `completion == NULL` and `RING_REASON_INCOMPLETE`, meaning "this SQE
will never be executed, because an earlier link failed".

That walk was removed as a double-release bug: the kernel already emits `-ECANCELED`
CQEs for the links it cancelled, so completing them locally dropped each descriptor's
reference a second time. `FastSocket` and `FastBIO` were reworked in the same commit
to recognise `-ECANCELED` directly.

The constant is kept, and the consumer-side contract still holds: a handler that must
do something when its SQE never executes should test `reason != RING_REASON_COMPLETE`
rather than compare against this constant. `HandleThreadWakeupCompletion()` in
`Ring/ThreadCall.c` is the live example — on any non-complete reason it falls back to
calling `futex()` synchronously, because the queued `FUTEX_WAKE` may never have run.

## Buffer Ownership

For the Buffer Provider API the ownership rule is set by the `function` argument of
`AdvanceFastRingBuffer()`:

- `function == NULL` — the same buffer is returned to the buffer ring immediately.
  The data must already be consumed or copied when the call is made.
- `function != NULL` — a fresh buffer is allocated and put into the ring, and the
  buffer returned earlier by `GetFastRingBuffer()` stays with the caller, which
  becomes responsible for freeing it.

Buffers passed to `CreateFastRingBufferProvider()` are released through the
`ReleaseRingBufferFunction` given to `ReleaseFastRingBufferProvider()`. Passing
`NULL` there leaks the buffers by design, for the case where they come from an arena.

## Registered Files and Buffers

Registered files are reference counted per file descriptor: repeated
`AddFastRingRegisteredFile()` for the same handle returns the same index and only
increments the count, and the slot is released when the matching number of
`RemoveFastRingRegisteredFile()` calls have been made.

Indices are allocated from the lower half of the table; the upper half is reserved
for kernel-side allocation (`io_uring_register_file_alloc_range()`). The table size
is `RLIMIT_NOFILE / 2`, capped at `1 << 20`.

`UpdateFastRingRegisteredBuffer()` with `address == NULL` unregisters the slot and
returns `INT32_MIN`, not the index. Check for that sentinel explicitly.
