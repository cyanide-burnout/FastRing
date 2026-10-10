# Object Lifecycle and Concurrency Rules

This document describes how the objects of `FastRing` live: the ring, descriptors and their
references, submission and completion, cancellation, the built-in Poll, Watch, Timeout and Event
operations, flushers, and the thread rules that hold them together. It covers the invariants that
`Ring/FastRing.c` enforces but that are not visible from the function signatures in
[API.md](API.md). The last section lists the mistakes that were actually made with them.

Read it before writing a module that submits its own SQEs or keeps a `struct FastRingDescriptor*`
across callbacks. The modules under `Ring/` and `Supplimentary/` describe their own objects in a
*Lifecycle* section of their documents, in terms of the rules below.

- [The Ring](#the-ring)
- [Descriptors](#descriptors)
- [References and Ownership](#references-and-ownership)
- [Completion Handlers](#completion-handlers)
- [Chains](#chains)
- [Cancellation](#cancellation)
- [Poll and Watch](#poll-and-watch)
- [Timeouts](#timeouts)
- [Events Between Rings](#events-between-rings)
- [Flushers](#flushers)
- [Concurrency](#concurrency)
- [Shutdown](#shutdown)
- [Buffers and Registered Resources](#buffers-and-registered-resources)
- [Pitfalls](#pitfalls)

## The Ring

### Creation and the ring thread

`CreateFastRing(length)` creates one `io_uring` instance and the bookkeeping around it:

- The submission queue has `length` entries, rounded up to a power of two and capped at 16384;
  `0` takes the value of `RLIMIT_NOFILE`. The completion queue is four times longer.
- The ring is set up with `IORING_SETUP_SUBMIT_ALL`, `IORING_SETUP_COOP_TASKRUN`,
  `IORING_SETUP_SINGLE_ISSUER` and `IORING_SETUP_CQSIZE`. A kernel that does not know one of them
  fails the setup, and `CreateFastRing()` returns `NULL`; `IORING_SETUP_SINGLE_ISSUER` makes
  Linux 6.0 the floor (see [Requirements](../README.md#requirements)).
- A sparse table of fixed files is registered, `RLIMIT_NOFILE / 2` entries capped at `1 << 20`;
  the upper half of it is left to the kernel for direct descriptors
  (`io_uring_register_file_alloc_range()`).
- The ring memory is excluded from `fork()` (`io_uring_ring_dontfork()`), and, where liburing and
  the kernel support it, waiting is not accounted as iowait (`io_uring_set_iowait(ring, 0)`).
- The supported opcodes are probed once and kept in `ring->probe`, so that modules can check for
  an operation with `io_uring_opcode_supported()` before using it.

The thread that calls `CreateFastRing()` becomes **the ring thread**: its `gettid()` is recorded in
`ring->thread`, and with `IORING_SETUP_SINGLE_ISSUER` the kernel records the same task as the only
one allowed to submit. Since Linux 6.1 the kernel also refuses `io_uring_register()` from any other
task. Both refusals are `-EEXIST` (checked against `io_uring/io_uring.c` of v6.1 and
`io_uring/register.c` of the current tree). A ring therefore cannot be handed over to another
thread: a worker that needs a ring creates it itself, and the starting thread waits for it to be
ready, which is how the services that run several rings do it.

### One iteration of the loop

`WaitForFastRing(ring, interval, mask)` is the event loop; the application calls it in a loop on
the ring thread. One call performs, in this order:

```text
WaitForFastRing()
  1. if the completion queue overflowed, flush the overflow; if that made completions ready, go to 4
  2. move queued descriptors into the submission queue
  3. submit; wait up to `interval` ms for one completion unless something is ready already
  4. run the handler of every completion in the queue
  5. run the flushers armed during this iteration, until none is left
```

- Step 2 copies the SQE of every descriptor that was queued by `SubmitFastRingDescriptor()` or
  `SubmitFastRingDescriptorRange()` since the last iteration, from any thread, until the
  submission queue is full. What does not fit stays queued for the next iteration, and a chain is
  never split (see [Chains](#chains)).
- Step 3 does not wait when there is an SQE to submit or a completion to handle; otherwise it
  calls `io_uring_submit_and_wait_timeout()` with `interval` and `mask`, which is the signal mask
  for the duration of the wait.
- The return value is the result of the submission: the number of SQEs submitted, `0` when the
  wait timed out (`-ETIME` is mapped to `0`), or a negative error code, for example `-EINTR` when
  a signal interrupted the wait. `-EINVAL` means the ring is `NULL` or not initialized. The code
  is returned, `errno` is not set.

A loop written as `while (running && WaitForFastRing(ring, 200, NULL) >= 0);` therefore also stops
on a signal delivered to the thread; a loop that must survive `SIGSTOP`/`SIGCONT` or handled
signals tests for `-EINTR` explicitly. `interval` bounds how late the loop notices a state flag
set by a signal handler and how late it picks up work queued by another thread (see
[Concurrency](#concurrency)).

`WaitForFastRing()` must not be called from a completion handler or a flusher. The outer call is
in the middle of walking the completion queue, and a nested call would consume and advance the
same entries under it. Integrations that need to run a foreign loop inside FastRing do it from a
flusher and switch stacks (`FastGLoop`), or poll the foreign loop's descriptor (`FastUVLoop`).

### Release

`ReleaseFastRing()` is called on the ring thread after the loop has stopped. It completes what is
left with `RING_REASON_RELEASED` and frees everything the ring owns; see [Shutdown](#shutdown).

## Descriptors

### What a descriptor is

A `struct FastRingDescriptor` is one operation, or one long-lived object such as a poll, a watch,
a timeout or an event receiver. It is a 512-byte aligned block holding:

| Field | Meaning |
| --- | --- |
| `ring` | The ring the descriptor belongs to |
| `state`, `lock` | Position in the submission pipeline and the completion lock, see below |
| `submission` | A private copy of the SQE (with room for `IORING_SETUP_SQE128`), copied into the ring at submission |
| `length` | Bytes of `submission` to copy, `sizeof(struct io_uring_sqe)` by default |
| `identifier` | Address plus generation tag, put into `user_data` by `PrepareFastRingDescriptor()` |
| `references` | Reference count |
| `function`, `closure` | Completion handler and its argument |
| `linked`, `next`, `previous` | Chain bookkeeping, see [Chains](#chains) |
| `data` | 256 bytes of storage for the owner: a number, a pointer, socket address and `msghdr`, or the private data of Poll, Watch and Timeout |

`data` lives as long as the descriptor and is the natural place for anything the kernel reads or
writes during the operation: an address for `sendmsg()`, the 8 bytes an `eventfd` read fills in,
a `__kernel_timespec`.

Descriptors are allocated from the heap once and then recycled through a free list of the ring.
Their memory is returned to `malloc` only by `ReleaseFastRing()`. This is what makes a late
completion for a recycled descriptor harmless: the pointer in its `user_data` still points to a
descriptor, only to a later generation of it (see [Identifier](#identifier-and-user_data)).

### States

A descriptor is always in exactly one of the states stored in `descriptor->state`:

| State | Meaning |
| --- | --- |
| `RING_DESC_STATE_FREE` | On the free list, owned by the ring, must not be touched |
| `RING_DESC_STATE_ALLOCATED` | Returned by `AllocateFastRingDescriptor()`, owned by the caller, the SQE may be filled in |
| `RING_DESC_STATE_PENDING` | Prepared: the identifier is valid and the SQE may still be patched in place |
| `RING_DESC_STATE_LOCKED` | Transient: another party is changing the descriptor right now |
| `RING_DESC_STATE_SUBMITTED` | The SQE has been copied into the ring, the kernel owns the request |

```text
                AllocateFastRingDescriptor()
      FREE ─────────────────────────────────▶ ALLOCATED
       ▲                                          │ PrepareFastRingDescriptor()
       │ last reference dropped                   │ SubmitFastRingDescriptor()
       │                                          ▼
       │                                       PENDING ◀───────────────┐
       │                                          │ WaitForFastRing()   │ re-armed by
       │                                          ▼ copies the SQE      │ its handler
       └─────────────────────────────────────  SUBMITTED ──────────────┘
```

`LOCKED` sits between `PENDING` and `SUBMITTED` while the submission loop copies the SQE, and
replaces either of them while an update, a removal or `DiscardFastRingDescriptor()` claims the
descriptor. A caller that follows the API never observes it.

Being queued for submission is a separate property, not part of the state.
`SubmitFastRingDescriptor()` prepares **and** queues; `PrepareFastRingDescriptor()` only prepares,
and a descriptor left that way stays `PENDING` indefinitely: the submission loop walks the queue,
not the states. An event receiver is exactly that, prepared for the sake of its identifier and
never queued, so `PENDING` is where it lives for good.

The state is what lets an update be cheap. `UpdateFastRingPoll()`, `UpdateFastRingWatch()` and the
timeout update try to patch the SQE in place while the descriptor is `PENDING`, and only when it
has reached `SUBMITTED` do they send a separate `POLL_UPDATE` / `TIMEOUT_UPDATE` request. An
update issued before the next `WaitForFastRing()` costs nothing; one issued after it costs an
extra SQE and CQE.

### The submission queue

Queued descriptors form a lock-free multi-producer list that only the ring thread consumes, so
any thread may queue. The list always keeps its last node until another one is appended, and the
loop appends a free stub descriptor to push the last real one out; such stubs are recycled as the
loop passes them.

The list links descriptors through `next`, and its push assumes that the node is not in it
already. **A descriptor must not be queued twice**: re-queuing the last node makes it point at
itself, the submission loop stops advancing, and no SQE is ever submitted by that ring again. The
failure is silent: completions keep arriving and flushers keep running, so it looks like a hang.
A handler that re-arms its own descriptor is safe, because the descriptor left the list when it
was submitted; code that resubmits a descriptor whose state it has not claimed is not.

### Identifier and user_data

`PrepareFastRingDescriptor()` computes `descriptor->identifier` from the descriptor address and its
generation tag, which grows every time the descriptor returns to the free list, and stores it in
`submission.user_data` together with the option bits:

```text
[63:60]  reserved (LA57 / TBI / MTE)
[59:48]  tag, high bits
[47:09]  descriptor address (512-byte aligned)
[08:06]  options: RING_DESC_OPTION_IGNORE, RING_DESC_OPTION_USER1, RING_DESC_OPTION_USER2
[05:00]  tag, low bits
```

On completion the ring checks

```c
(completion->user_data & ~RING_DESC_OPTION_MASK) == descriptor->identifier
```

and ignores a CQE that does not match without touching the descriptor. Such a CQE belongs to an
earlier generation: an operation completed after its descriptor had been released and handed to
someone else. The check is a backstop for premature releases and for late messages to a released
event receiver, not a pattern to rely on otherwise.

`user_data` values at or above `RING_DATA_UNDEFINED` are skipped by the completion loop. Code
that submits raw SQEs past the descriptor API must keep its `user_data` in that range.

The option bits are free for the owner to distinguish several SQEs that share one descriptor and
one handler:

| Option | Meaning |
| --- | --- |
| `RING_DESC_OPTION_IGNORE` | Housekeeping completion: a cancellation, a poll or timeout update, or a NOP that replaced a cancelled request. The built-in Poll, Watch and Timeout handlers skip the user callback for it |
| `RING_DESC_OPTION_USER1` | Free for the owner, carried through unchanged |
| `RING_DESC_OPTION_USER2` | Free for the owner, carried through unchanged |

### Reusing the SQE

`AllocateFastRingDescriptor()` hands out a descriptor whose SQE is a clean `IORING_OP_NOP`. Every
later preparation of the same descriptor must start with `io_uring_initialize_sqe()`: since
liburing 2.6 the `io_uring_prep_*()` helpers set only the fields of their operation and leave
`flags`, `ioprio`, `rw_flags`, `buf_index`, `personality`, `file_index` and `addr3` as they were,
so `IOSQE_IO_LINK`, `IOSQE_BUFFER_SELECT` or a fixed-file index of the previous use would apply to
the next one. `FastRing.h` defines `io_uring_initialize_sqe()` as a no-op for older liburing, so
the call is always valid. Flags such as `IOSQE_FIXED_FILE` are set after the preparation.

## References and Ownership

### The rule

`descriptor->references` counts the parties that may still dereference the descriptor. When it
drops to zero, `closure`, `function`, `previous` and `identifier` are cleared, the state becomes
`FREE` and the descriptor returns to the free list. A reference is dropped in exactly two ways:

- the completion handler returns `0` for a CQE of the descriptor, or the descriptor has no handler
  and the CQE does not carry `IORING_CQE_F_MORE`;
- `ReleaseFastRingDescriptor()` is called.

A reference is taken by `AllocateFastRingDescriptor()`, which starts the count at `1`, by the
APIs listed below, or by the owner itself. There is no function for the last: the owner
increments `references` atomically, the way the library does internally:

```c
atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
```

A reference taken like this is dropped with `ReleaseFastRingDescriptor()`. It has to be taken
while the descriptor is known to be alive, for example in its own handler or while it is still
`ALLOCATED`; incrementing the count through a pointer that may already be stale protects nothing.

### Who holds what

| Object | Count at rest | Held by | Dropped by |
| --- | --- | --- | --- |
| Raw descriptor, allocated, not submitted | 1 | The owner | `ReleaseFastRingDescriptor()` or `DiscardFastRingDescriptor()` |
| Raw descriptor, submitted | 1 | The operation | The handler returning `0` |
| Raw descriptor, re-armed by its handler | 1 | The new operation | The handler returning `0` for the new one |
| Multishot operation | 1 | The operation | The last CQE, the one without `IORING_CQE_F_MORE`, when the handler returns `0` |
| Poll (`AddFastRingPoll()`) | 2 | The table entry, and the poll request | `RemoveFastRingPoll()` / `DestroyFastRingPoll()`, and the request's last completion |
| Watch (`AddFastRingWatch()`) | 2 | The returned pointer, and the poll request | `RemoveFastRingWatch()`, and the request's last completion |
| Timeout | 1 | The timeout request | Its completion, unless the timeout repeats or is re-armed |
| Timeout with `TIMEOUT_FLAG_OWNED` | 2 | The returned pointer, and the timeout request | The removal, and the request's completion |
| Event receiver (`CreateFastRingEvent()`) | 1 | The receiver | Its handler returning `0`, or `ReleaseFastRingDescriptor()` |
| Event sender (inside `SubmitFastRingEvent()`) | 1 | The `MSG_RING` request | Its completion |
| Cancellation, update, remove request | +1 | The housekeeping request | Its completion (`RING_DESC_OPTION_IGNORE`) |
| `GetFastRingPollDescriptor()` result | +1 | The caller | `ReleaseFastRingDescriptor()` by the caller |
| Temporary hold inside an update or removal | +1 | The call | The same call before it returns |

For a raw descriptor the reference returned by `AllocateFastRingDescriptor()` and the reference of
its operation are **the same one**. Once the descriptor is submitted, the owner holds no
reference of its own: after the handler returns `0` the pointer is dangling. An owner that keeps
the pointer beyond the completion takes a reference of its own or forgets the pointer inside the
handler. Poll, Watch and owned timeouts are built the other way round, with one reference for the
owner and one for the request, so that the handle stays valid until it is removed.

### Who frees what

| Resource | Freed by | When |
| --- | --- | --- |
| Descriptor | The ring | Its last reference is dropped, or `ReleaseFastRing()` |
| `closure` | The owner | When no callback can be delivered any more; see [Cancellation](#cancellation) |
| Memory the kernel reads or writes (buffers, `msghdr`, futex words) | The owner | After the operation has completed, or after `ReleaseFastRing()` |
| Memory inside `descriptor->data` | Nobody | Lives and dies with the descriptor |
| Buffers of a buffer provider | `ReleaseFastRingBufferProvider()`, or the caller after `AdvanceFastRingBuffer()` with a `function` | See [Buffers](#buffers-and-registered-resources) |
| Flusher | The ring | After it has run or was cancelled, or `ReleaseFastRing()` |
| The ring | The application | `ReleaseFastRing()` |

## Completion Handlers

```c
int (*HandleFastRingCompletionFunction)(struct FastRingDescriptor* descriptor,
                                        struct io_uring_cqe* completion,
                                        int reason);
```

### Return value

- Return `0`: the handler is done with this completion and one reference is dropped. If it was the
  last one, the descriptor is free when the call returns.
- Return non-zero: the reference stays, because the descriptor is still armed. Either the handler
  has submitted it again (`SubmitFastRingDescriptor(descriptor, 0); return 1;` is the usual
  re-arm), or the completion carries `IORING_CQE_F_MORE` and the multishot request will produce
  more CQEs (`return completion && (completion->flags & IORING_CQE_F_MORE);`).

Returning non-zero without an outstanding request leaks the descriptor for the lifetime of the
ring. Returning `0` while the descriptor is still armed lets it be recycled under an in-flight
request; the identifier check turns the later CQE into a silent drop instead of memory
corruption, but the request is lost.

A descriptor without a handler (`function == NULL`) drops its reference on every CQE that does
not carry `IORING_CQE_F_MORE`, and on none of the others. This is how fire-and-forget operations
and the senders of events release themselves.

### Arguments

| `reason` | `completion` | Delivered when |
| --- | --- | --- |
| `RING_REASON_COMPLETE` | The CQE | A normal completion |
| `RING_REASON_COMPLETE` | `NULL` | A chained descriptor that produced no CQE of its own because of `IOSQE_CQE_SKIP_SUCCESS` |
| `RING_REASON_RELEASED` | `NULL` | `ReleaseFastRing()` is tearing the ring down |
| `RING_REASON_INCOMPLETE` | — | Not produced any more, see below |

`completion->res` is the result of the operation, a negative error code included; `-ECANCELED`
reports an operation that was cancelled or that an earlier link of its chain did not let run.
The CQE belongs to the ring and is valid only for the duration of the call.

`RING_REASON_INCOMPLETE` was produced up to commit `af9c11c` (March 2024): when a member of an
`IOSQE_IO_LINK` chain failed, the loop completed the remaining links itself with
`completion == NULL` and this reason. The kernel already posts `-ECANCELED` CQEs for those links,
so each of them was released twice; the walk was removed and `FastSocket` and `FastBIO` were
changed to recognise `-ECANCELED`. The constant is kept. A handler that must react when its SQE
never executes tests `reason != RING_REASON_COMPLETE` rather than this constant;
`HandleThreadWakeupCompletion()` in `Ring/ThreadCall.c` does so.

### Exclusion while a handler runs

A handler runs with the descriptor's completion lock held (`RING_DESC_LOCK_HELD` in
`descriptor->lock`). An update, a removal or a discard made from another thread waits on that
lock as a futex until the handler returns, so once such a call has returned, the handler is not
running any more. The ring thread skips the wait, since its own handlers cannot run concurrently
with it. The details are in [Three layers of exclusion](#three-layers-of-exclusion).

## Chains

`SubmitFastRingDescriptorRange(first, last)` queues a chain of descriptors as a unit. The caller
builds it:

- every descriptor is prepared with `PrepareFastRingDescriptor()`;
- each one points to the following one through `next`, from `first` to `last`;
- every member but the last carries `IOSQE_IO_LINK` or `IOSQE_IO_HARDLINK` in `submission.flags`;
- `first->linked` holds the number of SQEs the chain needs. The submission loop starts the chain
  only when `linked` is smaller than the free space of the submission queue, so a chain is never
  split across two `io_uring_enter()` calls. The in-tree users store the length of the chain.

Every member holds its own reference and completes on its own. When a member linked with
`IOSQE_IO_LINK` fails, the kernel completes the remaining ones with `-ECANCELED`;
`IOSQE_IO_HARDLINK` keeps the chain going regardless of the result.

For chains built with `IOSQE_CQE_SKIP_SUCCESS`, only the last member produces a CQE. The caller
sets `previous` of each member to the member before it; when the last one completes, the ring
walks `previous` backwards and calls each preceding handler with `completion == NULL` and
`RING_REASON_COMPLETE`. A handler used in such a chain must accept a `NULL` completion.

`FastSocket` and `FastBIO` use chains for ordered sends: one batch of a loop iteration becomes one
linked chain, flushed by a flusher.

## Cancellation

### Built-in operations

Poll, Watch and Timeout are removed through their own API: `RemoveFastRingPoll()`,
`DestroyFastRingPoll()`, `RemoveFastRingWatch()` (or `SetFastRingWatch()` with a zero mask),
`SetFastRingTimeout(..., -1, ...)` and `NULL` intervals for the other two timeout functions. They
all work the same way:

- `RING_CONDITION_REMOVE` is set first, so that no further user callback is delivered, from this
  moment on, whatever is still in flight.
- The descriptor is claimed. A request still queued for submission is rewritten in place, as a
  NOP when it was the operation itself or as a removal when it was an update; a request already
  in the kernel gets a separate `POLL_REMOVE`, `TIMEOUT_REMOVE` or `ASYNC_CANCEL`, submitted with
  `RING_DESC_OPTION_IGNORE` and holding a reference of its own.
- The owner's reference is dropped.

**Removal is asynchronous.** The descriptor stays alive until the last CQE of the cancelled request
has arrived, which takes at least one more iteration of the loop. The closure outlives the call by
less than that: once the removal has returned, no callback will be delivered and none is running,
because a claim made off the ring thread waits a running handler out. The exception is a removal
made from inside the callback itself: that callback is still on the stack and still uses the
closure, so the closure may be freed only after the callback has returned to the ring.

A removal can be made from inside the operation's own callback, and that is the intended way to
stop an operation on an event; see [Reentrancy](#reentrancy).

### Raw descriptors: DiscardFastRingDescriptor()

A module that keeps its own descriptors abandons them on teardown with
`DiscardFastRingDescriptor()`. It claims the state the same way the submission loop does, detaches
the handler and the closure under that claim, and then:

- rewrites a submission that has not reached the kernel yet as `IORING_OP_NOP`, which consumes
  the reference the queued entry holds;
- or submits `IORING_OP_ASYNC_CANCEL` for an operation already in the kernel; the cancellation
  takes one more reference, its own completion drops it, and the original operation completes
  with `-ECANCELED` and drops the last one. Timeouts are covered too, the kernel's cancellation
  falls through to the timeout list;
- or releases outright a descriptor that was allocated and never submitted, since no completion
  will ever arrive to do it.

In any other state it does nothing. After the call no completion reaches the owner, so the owner
frees its own resources right after it, closure included. Memory the cancelled operation may still
read or write, a receive buffer for example, has to wait for the cancellation, which the owner
does not see; such memory is owned by something that outlives the operation (a reference-counted
`FastBuffer`, `descriptor->data`) or is freed only after `ReleaseFastRing()`.

The function expects a descriptor carrying a single operation. If the queued submission is not
the operation itself but an update prepared for an earlier one, which is what
`UpdateFastRingWatch()` does, rewriting it as a NOP would leave the earlier operation armed. An
event receiver carries no operation at all, so discarding it finds nothing to cancel and leaves
its reference behind; release it with `ReleaseFastRingDescriptor()` instead.

**Hold a reference when calling it from another thread.** The claim covers both writers, the
submission loop and a running handler. It cannot cover the pointer: a completion that drops the
last reference recycles the descriptor and hands it to the next owner, and a claim taken after
that means nothing. On the ring thread the two cannot overlap, because the same thread reaps the
completions. From any other thread the caller holds a reference of its own for the duration of
the call and drops it after the call returns; the references of the operation itself are
accounted inside.

The two claims are tried once each, pending first. Off the ring thread, a handler that runs
between them and re-arms the descriptor leaves it pending again, the second claim fails, and
the call returns without doing anything while the operation stays armed. A descriptor whose
handler re-arms itself is therefore abandoned from the ring thread.

Doing the same by hand is a trap, twice over:

- Reading `descriptor->state` and acting on it races with the submission loop whenever the
  owner can be torn down from another thread: the loop may take the descriptor between the read
  and the write, so the operation reaches the kernel uncancelled while its copy is overwritten.
- Resubmitting a descriptor that is still queued corrupts the queue, see
  [The submission queue](#the-submission-queue).

### Reentrancy

All Poll, Watch and Timeout callbacks run inside `WaitForFastRing()` on the ring thread. Calling
`Update*`, `Remove*` or `Set*` for the same operation from inside its own callback is supported.
The mechanism is the `condition` word in the descriptor's `data`:

| Condition | Meaning |
| --- | --- |
| `RING_CONDITION_GUARD` | Set while the user callback is running |
| `RING_CONDITION_UPDATE` | Poll and Timeout: the operation ended without being re-armed, so nothing of it is left in the kernel and an update has to arm it anew. Watch never raises it, because its handler always re-arms |
| `RING_CONDITION_REMOVE` | Removal was requested; no further user callbacks will be issued |

What a module may rely on:

- **Remove from inside the callback is safe.** The handler sees `RING_CONDITION_REMOVE` after the
  callback returns and does not re-arm, so the operation is not resurrected.
- **Update from inside the callback does not double-arm.** The update claims the descriptor and
  submits the new request itself, which leaves the state no longer submitted; the handler checks
  that on its way out and gives the descriptor up instead of re-arming it. Leaving the work to
  the handler would lose the update whenever the completion carries `POLLERR` or `POLLHUP`, since
  the handler ends the operation in that case.
- **After remove, no callback is delivered.** A CQE that arrives for an already removed operation
  is consumed without calling the user function.

`RemoveFastRingWatch()` and the timeout removal retry `LockPendingRingDescriptor()` /
`LockSubmittedRingDescriptor()` until one of them succeeds. Off the ring thread each attempt
first waits on the futex for a running handler, so the retrying is only over claims held by other
writers, which are released without waiting for anything. A second removal of the same watch or
timeout is idempotent: it waits for a running handler and returns without dropping anything.

### Three layers of exclusion

A descriptor carries three words, and each answers a different question. All three are involved
in every update or removal.

**`descriptor->lock`: who has the descriptor right now.** Bit `RING_DESC_LOCK_HELD` says that a
completion handler is running; the bits above it (`RING_DESC_LOCK_WAITER` each) count writers
waiting to get in.

| Side | Taken by | Meaning |
| --- | --- | --- |
| `LockRingDescriptor()` / `UnlockRingDescriptor()` | Anyone about to change the descriptor | Wait for a running handler, and be visible while waiting |
| `LockCompletedRingDescriptor()` / `UnlockCompletedRingDescriptor()` | `HandleCompletedRingDescriptor()` | Take the descriptor for the duration of the handler |

A completion takes `lock` and `state` in one 64-bit compare-and-swap that succeeds only while the
lock is clear and the state is pending or submitted, so a single waiting writer keeps it out,
which stops a stream of multishot completions from starving that writer. A writer adds itself to
the waiter count and sleeps on `lock` until the handler returns. The ring thread skips both.

**`descriptor->state`: where the descriptor is in the submission pipeline.**
`LockPendingRingDescriptor()` and `LockSubmittedRingDescriptor()` take the descriptor lock on the
way, claim the state as `RING_DESC_STATE_LOCKED`, and drop the lock again; from there on the
claimed state is the exclusion, and the completion lock does not accept it. They have no matching
`Unlock*`: the state is released by whatever writes the next one, `PrepareFastRingDescriptor()`,
a plain store, or the release path.

**`descriptor->data.*.condition`: what has been asked of the operation.** The conditions above do
not exclude anybody; they carry intent between the API and the handler.

The order is always the same: the descriptor lock first, the state second. The condition word is
mostly read after a claim rather than before, because taking the claim may have waited long
enough for the answer to change. Two exceptions are deliberate: `RING_CONDITION_REMOVE` is
published before any claim, so that whoever takes the state next sees it, and
`RING_CONDITION_GUARD` is examined between the `PENDING` and `SUBMITTED` attempts, where it
decides whether a claim is worth trying at all.

## Poll and Watch

Both arm `IORING_OP_POLL_ADD` on a file descriptor and call a function when it becomes ready.
They differ in how they are addressed and in how long they live.

**Poll** is addressed by the file descriptor number: the ring keeps one entry per handle in a
table, and `AddFastRingPoll()` on a handle that already has one overwrites the entry. It is the
model of `epoll`-style libraries that identify a watch by its handle (c-ares, D-Bus, Lua):

```text
AddFastRingPoll(ring, fd, flags, f, c)          entry[fd] = descriptor, references 2
  each event, request still armed (F_MORE)       f(fd, events, c, flags)
  each event, request ended                      f(fd, events, c, flags), then
    RING_POLL_REPEAT, no POLLERR/POLLHUP           armed again
    otherwise                                      RING_CONDITION_UPDATE; the entry stays
UpdateFastRingPoll(ring, fd, flags)              patched, updated in the kernel, or armed anew
RemoveFastRingPoll(ring, fd)                     entry cleared, request cancelled
```

The request is a kernel multishot poll by default: it stays armed and reports every readiness
change with `IORING_CQE_F_MORE` until the kernel ends it. `RING_POLL_SHOT` turns it into a
one-shot poll, which `RING_POLL_REPEAT` arms again after each callback. A request that ended
without being armed again leaves its entry in the table, and `UpdateFastRingPoll()` or
`SetFastRingPoll()` arms it anew. An entry exists until it is removed, whether or not a request
is armed, so every `AddFastRingPoll()` needs a matching removal.
`DestroyFastRingPoll(ring, function, closure)` removes every entry of one owner, which is the
usual teardown of an object that polls several handles.

Remove the poll before closing the file descriptor. The armed request holds a reference to the
file in the kernel, so closing the descriptor does not end it; and the entry stays under the
descriptor number, so `SetFastRingPoll()` for a new file that gets the same number updates the
old request instead of arming a new one.

**Watch** is addressed by its descriptor, so several watches may exist for one handle. Its
handler always re-arms a request that has ended, unless the watch was removed, so a watch lives
until `RemoveFastRingWatch()`:

```text
d = AddFastRingWatch(ring, fd, mask, flags, f, c)   references 2
  each event                                        f(d, result); re-armed
UpdateFastRingWatch(d, mask)                        new mask, in place or by POLL_UPDATE
RemoveFastRingWatch(d)                              cancelled, d must not be used again
```

`result` is the `poll` event mask, or a negative error code of the request; the watch is re-armed
after an error as well, so a watch on a broken descriptor keeps reporting it until removed.

Neither callback is called at `ReleaseFastRing()`: a poll or watch still registered then is freed
silently. The owner releases its closure itself.

## Timeouts

`SetFastRingTimeout()`, `SetFastRingCertainTimeout()` and `SetFastRingPreciseTimeout()` differ only
in the type of the interval (milliseconds, `struct timeval`, `struct timespec`). Each call does
one of three things depending on its arguments:

| `descriptor` | interval | Effect | Returns |
| --- | --- | --- | --- |
| `NULL` | valid | Creates and arms a timeout | The new descriptor, or `NULL` |
| existing | valid | Changes the interval and re-arms | `descriptor` |
| existing | `-1` / `NULL` | Removes | `NULL` |

`function` and `closure` are used only at creation. The callback receives the descriptor;
`descriptor->closure` is the closure. Only an expiry (`-ETIME`) calls it.

### Ownership of a timeout descriptor

By default a timeout that has fired and is not repeating is released by its own completion: the
descriptor is gone by the time the callback has returned. The owner forgets the pointer inside
the callback, and every in-tree user does exactly that:

```c
static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct Module* module = (struct Module*)descriptor->closure;

  module->timer = NULL;          /* the descriptor is released after this returns */
  /* ... */
}

module->timer = SetFastRingTimeout(ring, module->timer, 1000, 0, HandleTimeout, module);  /* arm or restart */
module->timer = SetFastRingTimeout(ring, module->timer, -1, 0, NULL, NULL);              /* stop: NULL */
```

A timeout that the callback re-arms through `SetFastRingTimeout(ring, descriptor, ...)` survives,
because the new request takes the reference, and so does a repeating one. Nothing may touch such
a descriptor from another thread, since there is no telling from outside whether it still exists.

`TIMEOUT_FLAG_OWNED`, passed at creation, switches the model. The descriptor takes a reference on
the owner's behalf and outlives its own completion, so the stored pointer stays valid: the
timeout can be re-armed through it after firing, and it can be touched from another thread. In
exchange the removal becomes mandatory, and a descriptor that is never removed is a leak.
Concurrent duplicate removals are idempotent and wait for a running callback; after the first
removal has returned, however, the pointer is no longer a handle. The flag lives above the 32 bits
the kernel reads and never reaches an SQE.

### Repeating timeouts

`TIMEOUT_FLAG_REPEAT` makes a timeout fire every `interval` until it is removed. What the flag is
**is decided when FastRing is compiled**, not at run time:

- When the liburing headers define `IORING_TIMEOUT_MULTISHOT` (liburing 2.4 and newer), the flag
  *is* `IORING_TIMEOUT_MULTISHOT`: the kernel repeats the timeout and every expiry carries
  `IORING_CQE_F_MORE`. The kernel accepts it since Linux 6.4.
- With older headers it is bit 62, which the kernel never sees, and the handler re-arms the
  timeout after every expiry.

A binary built against liburing 2.4 or newer and run on a kernel older than 6.4 gets `-EINVAL` for
every repeating timeout: Linux 6.1 rejects unknown timeout flags (`__io_timeout_prep()` in
`io_uring/timeout.c`). The handler calls no callback for anything but `-ETIME` and releases the
descriptor, so the timeout silently never fires and the pointer the owner keeps dangles. Build
against the liburing of the target system.

`flags` is also passed to `io_uring_prep_timeout()`, so `IORING_TIMEOUT_ABS` and the clock
selection (`IORING_TIMEOUT_BOOTTIME`, `IORING_TIMEOUT_REALTIME`) apply; `FastAvahiPoll` uses
absolute `CLOCK_REALTIME` timeouts because that is what Avahi hands out.

## Events Between Rings

A FastRing Event delivers a completion from one ring to another through `IORING_OP_MSG_RING`.

```text
target thread                                   source thread (runs its own ring)
receiver = CreateFastRingEvent(target, f, c)
  PENDING, never queued, references 1
                                                SubmitFastRingEvent(source, receiver, parameter, option)
                                                  temporary sender on `source`, MSG_RING queued
                                                next WaitForFastRing(source) submits it
f(receiver, completion, COMPLETE)  ◀── kernel posts the CQE to `target` and wakes it
  completion->res == parameter
  return 1 to keep the receiver, 0 to release it
```

- The receiver's handler follows the ordinary contract. A receiver that is used repeatedly
  returns non-zero and is released by its owner with `ReleaseFastRingDescriptor()` at the end.
- A message that arrives after the receiver was released fails the identifier check and is
  dropped, as long as the target ring exists.
- The sender is allocated on the **source** ring, submitted by its loop and released by its own
  completion. `SubmitFastRingEvent()` therefore belongs to the source ring thread: from any other
  thread it only queues the `MSG_RING`, and nothing wakes the source to submit it. It is not a
  general wakeup from a foreign thread; that is `ThreadCall`.
- `SubmitFastRingEvent()` returns `0`, or `-EINVAL` when the receiver is `NULL` or no descriptor
  could be allocated.

The pattern used in production is a counter guarded by an edge: the producer increments it and
sends an event only on the transition from 0, and the receiver drains everything before resetting
it.

## Flushers

A flusher is a one-shot callback that runs at the end of the current loop iteration, after every
CQE of the pass has been handled (step 5 above). It is the mechanism for "do this once, but not
right now": batching work that several completions would each trigger, and getting off a
handler's stack before touching shared state. Its interface and rules are in
[API.md](API.md#flush-handlers); for the lifecycle the following matters:

- `SetFastRingFlushHandler()` arms it; it is called exactly once with `RING_REASON_COMPLETE`, or
  never when `RemoveFastRingFlushHandler()` cancels it in time. A flusher armed from a flusher runs
  in the same iteration.
- After the call the flusher goes back to a free list, and the next `SetFastRingFlushHandler()`
  may return the same object to somebody else. The handle is valid only until the handler runs,
  so the handler clears the stored handle first.
- At `ReleaseFastRing()` a flusher that is still armed is called with `RING_REASON_RELEASED`.
  A handler that does its work without checking `reason` runs it once more during shutdown.
- The whole facility belongs to the ring thread.

## Concurrency

### Thread rules

| Operation | Thread |
| --- | --- |
| `CreateFastRing()` | The future ring thread |
| `WaitForFastRing()`, `ReleaseFastRing()` | The ring thread; the kernel refuses submissions from other tasks with `-EEXIST` |
| Completion, Poll, Watch, Timeout and flush callbacks | The ring thread |
| `AllocateFastRingDescriptor()`, `PrepareFastRingDescriptor()`, `SubmitFastRingDescriptor()`, `SubmitFastRingDescriptorRange()`, `ReleaseFastRingDescriptor()` | Any thread (lock-free) |
| Poll API | Any thread (`ring->files.lock`) |
| `AddFastRingWatch()`, `UpdateFastRingWatch()`, `RemoveFastRingWatch()` | Any thread, as long as the watch has not been removed |
| Timeouts | Creation from any thread. Update and removal from another thread only with `TIMEOUT_FLAG_OWNED` |
| `DiscardFastRingDescriptor()` | The ring thread, or any thread holding a reference of its own |
| `SetFastRingFlushHandler()`, `RemoveFastRingFlushHandler()` | The ring thread |
| `SubmitFastRingEvent()` | The thread of the **source** ring |
| `AddFastRingRegisteredFile()`, `RemoveFastRingRegisteredFile()`, `AddFastRingRegisteredBuffer()`, `UpdateFastRingRegisteredBuffer()` | The ring thread; the kernel refuses registration from other tasks with `-EEXIST` since Linux 6.1 |
| Buffer provider: create, hold, release, `AdvanceFastRingBuffer()` | The ring thread |
| `IsFastRingThread()` | Any thread |

```c
int IsFastRingThread(struct FastRing* ring);
```

Returns `1` on the ring thread, `0` on another thread, and `-1` if the ring has no recorded thread.
Modules use it to choose between a direct call and a `ThreadCall` hop.

### Work from another thread

Queuing a descriptor from another thread only puts it on the list. Its SQE is copied into the
ring on the next `WaitForFastRing()`, and if the ring thread is waiting, it does not notice the
new descriptor until the wait ends: until `interval` expires or an unrelated completion arrives.
Something has to wake the ring:

- `ThreadCall` runs a function on the ring thread synchronously and supplies a real wakeup: a
  futex wait (`IORING_OP_FUTEX_WAIT`, Linux 6.7) or an `eventfd` read armed on the ring.
- A module of its own can keep a read armed on an `eventfd` and write to it after queuing.
- Between two rings, `SubmitFastRingEvent()`.

Descriptors may be allocated and queued on a ring owned by another thread; a sending module in
production allocates on its worker's ring from the main thread and lets the worker submit. The
handler then runs on the ring thread, not on the thread that queued it.

## Shutdown

### What ReleaseFastRing() does

1. Submission stops: from here on a submission is dropped and `SetFastRingFlushHandler()` returns
   `NULL`, so what a handler below does on release cannot reach memory being freed.
2. Every flusher still armed is called with `RING_REASON_RELEASED`, and all flushers are freed.
3. Every descriptor that is not `FREE` and has a handler is called with
   `(descriptor, NULL, RING_REASON_RELEASED)`, and every descriptor is freed. This includes
   descriptors in flight, event receivers and descriptors that were allocated but never
   submitted.
4. The probe and the bookkeeping of registered files and buffers are freed, and
   `io_uring_queue_exit()` closes the ring; the kernel cancels what it still holds.

`ReleaseFastRing()` does not wait for or reap completions. A cancellation queued by a removal or a
discard after the last `WaitForFastRing()` never reaches the kernel; it does not need to,
because closing the ring cancels everything, but no completion is delivered for it either.

Rules for the `RING_REASON_RELEASED` path:

- `completion` is `NULL`, and the return value is ignored: the descriptor is freed either way.
- The handler releases the module's own resources: closes file descriptors, frees closures,
  completes outstanding user requests with an error. It need not submit anything or arm flushers,
  which are dropped at this point, and `AllocateFastRingDescriptor()` returns `NULL`; releasing an
  object whose release submits, a socket from its `POLLHUP` for example, is therefore safe.
- Descriptors are visited in the reverse order of their first allocation, which says nothing about
  module order. A handler must not assume that the objects it points to are still alive.
- The built-in Poll, Watch and Timeout handlers do not call the user callbacks; their
  descriptors are just freed.

A never-submitted descriptor whose handler runs only on this path is a usable teardown hook for
state that has to live exactly as long as the ring, for example per-thread state of a worker
ring.

### Order

The robust order is the one the long-running services use:

1. Stop the loop: set a flag that the loop condition tests, typically from a signal handler.
2. Release the objects that use the ring, in reverse order of creation, while the ring still
   exists: application objects, protocol adapters, loop adapters, `ThreadCall`s
   (`TC_ROLE_HANDLER`), `WatchDog`.
3. `ReleaseFastRing()`.
4. Free what in-flight operations could still reference: memory a cancelled operation may still
   read or write, futex words, shared mappings.

An object released after `ReleaseFastRing()` operates on freed memory: its own release removes
timeouts or watches through descriptors that no longer exist. A LIFO destroyer list makes this
easy to get wrong when the ring is registered after one of its users.
`RING_REASON_RELEASED` is the safety net for objects that were not released before the ring, not
a substitute for releasing them.

A worker thread with its own ring follows the same order on its own thread and is joined
afterwards.

## Buffers and Registered Resources

### Buffer provider

For the Buffer Provider API (`IORING_REGISTER_PBUF_RING`) the ownership of a buffer is set by the
`function` argument of `AdvanceFastRingBuffer()`:

- `function == NULL`: the same buffer is returned to the buffer ring immediately; the data must
  have been consumed or copied before the call.
- `function != NULL`: a fresh buffer from `function` takes the slot, and the buffer returned
  earlier by `GetFastRingBuffer()` stays with the caller, which becomes responsible for freeing
  it.

Every completion that consumed a buffer has to be advanced, an error completion that carries one
included; otherwise the slot is lost for good. Buffers passed to `CreateFastRingBufferProvider()`
are released through the `ReleaseRingBufferFunction` given to `ReleaseFastRingBufferProvider()`;
`NULL` leaves them to the caller, for the case where they come from an arena. A provider must be
released before the ring. A receiver that holds it with `HoldFastRingBufferProvider()`, as
`FastSocket` and `FastBIO` do, keeps it registered until its last completion; for operations of
its own the owner must not release it before they have completed.

### Registered files and buffers

Registered files are reference counted per file descriptor: repeated
`AddFastRingRegisteredFile()` for the same handle returns the same index and increments the count;
the slot is released when the matching number of `RemoveFastRingRegisteredFile()` calls has been
made. Indices come from the lower half of the table; the upper half is left to the kernel.
Removing a registration does not cancel operations that use it; the kernel keeps the file until
they complete.

`AddFastRingRegisteredBuffer()` registers a sparse table of 16384 buffers on first use.
`UpdateFastRingRegisteredBuffer()` with `address == NULL` unregisters the slot and returns
`INT32_MIN`, not the index; check for that sentinel explicitly.

## Pitfalls

Each of these was made in real code, in this repository or in the services that use it.

**Creating the ring on one thread and running it on another.** Every submission then fails with
`-EEXIST`. Create the ring on the thread that runs the loop; a starting thread that needs to know
when the worker is ready waits for it (a semaphore posted after `CreateFastRing()`).

**Reading `errno` after `WaitForFastRing()`.** The error is the return value. And a loop that stops
on any negative value also stops on `-EINTR`.

**Keeping the pointer of a submitted raw descriptor.** After its handler returned `0` the
descriptor belongs to somebody else. Keep a reference, or forget the pointer in the handler.

**Keeping the pointer of a fired timeout.** A timeout without `TIMEOUT_FLAG_OWNED` frees itself
after its callback; clear the stored pointer in the callback, or use `TIMEOUT_FLAG_OWNED`.

**Building against a newer liburing than the target has.** Repeating timeouts become kernel
multishot timeouts and fail on kernels older than 6.4; see
[Repeating timeouts](#repeating-timeouts).

**Re-preparing an SQE without `io_uring_initialize_sqe()`.** With liburing 2.6 and newer the old
flags survive into the next operation. The migration from liburing 2.3 to 2.6 turned exactly this
into a large descriptor leak (commit `64600b1`).

**Cancelling a raw descriptor by hand.** The idiom "clear `function` and `closure`, take a
reference, prepare `ASYNC_CANCEL`, submit" handles only the submitted state: a descriptor still
queued gets its SQE overwritten and is queued twice. It predates `DiscardFastRingDescriptor()`,
which handles all states under a claim; use that.

**Freeing the closure after removing from inside the callback.** The callback that removed its own
operation is still running. Free the closure after it returns, or arm a flusher to do it.

**Cancelling a flusher through a stale handle.** The handle may belong to another module by now,
and the cancellation succeeds on it. Clear the handle in the handler.

**Doing work in a flusher regardless of `reason`.** It runs again during `ReleaseFastRing()`,
when the objects it touches may be half released.

**Releasing a user of the ring after the ring.** See [Order](#order).

**Expecting work queued from another thread to run promptly.** It waits for the next iteration.
Use `ThreadCall` or an `eventfd` to wake the ring.

**Calling `WaitForFastRing()` from a callback.** The nested call consumes the completions the
outer call is walking.

**Using the ring in a child process.** The ring memory is not inherited across `fork()`; a child
creates its own ring.

**Closing a file descriptor that still has a poll entry.** The kernel keeps the file open for the
armed request, and a new file that gets the same number inherits the entry. Remove the poll
first.
