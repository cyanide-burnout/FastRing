# CoRing API Reference

Header: `Ring/CoRing.h`

`CoRing` is a C++ adapter integrating FastRing descriptors with the `Compromise` coroutine/event
abstraction. A `CoRing` is a `Compromise::Emitter<CoRingEvent>`: a coroutine `co_await`s it and
is resumed with one `CoRingEvent` per completion of a descriptor allocated through it. One
coroutine at a time can wait on a `CoRing`.

Requires C++20 coroutines and the `Compromise` library (`Compromise.h`), which is an
external dependency and is not vendored in this repository.

## Main Classes

- `CoRing`
- `CoRingEvent`

## CoRing

```cpp
CoRing(struct FastRing* ring);
~CoRing();

struct FastRingDescriptor* allocate();
void submit();
```

## CoRingEvent

```cpp
CoRingEvent();
void keep() const;
void release() const;
```

## Event Fields

`CoRingEvent` exposes the completion as it arrived:

- `descriptor` - the descriptor the event belongs to
- `completion` - the CQE, or `nullptr` for `RING_REASON_RELEASED` and for chained
  descriptors
- `reason` - `RING_REASON_COMPLETE` or `RING_REASON_RELEASED` (the core no longer
  produces `RING_REASON_INCOMPLETE`)

`completion` points into the completion queue and is valid only until the coroutine
suspends again.

## Notes

- `allocate()` prepares descriptor tracking and callback wiring and returns a descriptor
  whose SQE is an `IORING_OP_NOP`; fill `descriptor->submission` before it is submitted. It
  throws `std::system_error` instead of returning `nullptr` when no descriptor can be
  allocated (memory exhausted, or the ring being released); the error code is the current
  `errno`.
- `submit()` queues every currently allocated descriptor separately and moves them to
  the submitted set. It is also called automatically each time a coroutine suspends on
  `co_await`, so a coroutine that fills a descriptor and then awaits does not need an
  explicit `submit()`. `linked` is not set, so an `IOSQE_IO_LINK` chain built this way
  may be split between two submissions.
- `keep()` / `release()` control ownership of completed descriptors. Default is
  release: unless the coroutine calls `keep()`, the descriptor is dropped after the
  event, which matches returning `0` from a plain `HandleFastRingCompletionFunction`.
  Call `keep()` for multishot operations or when re-arming the same descriptor. Both are
  valid only until the coroutine suspends again: they write the handler's return value,
  which lives on the stack of the completion.
- A completion that arrives while no coroutine is waiting is dropped and the descriptor
  released — for a multishot request still armed as well.
- The destructor detaches every still-submitted descriptor (clearing its handler and
  closure) and submits a separate `IORING_OP_ASYNC_CANCEL` for each, then releases the
  descriptors that were allocated but never submitted. Outstanding operations therefore
  do not call back into a destroyed `CoRing`; each detached descriptor is freed by its own
  last completion. When the cancellation cannot be allocated, the operation runs to its
  natural end.

## Lifecycle

A `CoRing` owns the descriptors it allocated until they complete without `keep()`. It uses
unsynchronised containers and clears handlers without a claim: create, use and destroy it on
the ring thread. Destroy it before `ReleaseFastRing()`. If the ring is released first, its
teardown resumes the waiting coroutine with `RING_REASON_RELEASED` and frees the descriptors,
after which the destructor touches freed memory.

See [LIFECYCLE.md](LIFECYCLE.md) for the underlying reference counting rules.

