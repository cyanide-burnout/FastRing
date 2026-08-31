# CoRing API Reference

Header: `Ring/CoRing.h`

`CoRing` is a C++ adapter integrating FastRing descriptors with the `Compromise` coroutine/event abstraction.

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
- `reason` - `RING_REASON_COMPLETE`, `RING_REASON_INCOMPLETE` or
  `RING_REASON_RELEASED`

## Notes

- `allocate()` prepares descriptor tracking and callback wiring. It throws
  `std::system_error` instead of returning `nullptr` when the ring is exhausted.
- `submit()` pushes all currently allocated descriptors and moves them to the
  submitted set. It is also called automatically each time the emitter resumes, so a
  coroutine that fills a descriptor and then awaits does not need an explicit
  `submit()`.
- `keep()` / `release()` control ownership of completed descriptors. Default is
  release: unless the handler calls `keep()`, the descriptor is dropped after the
  event, which matches returning `0` from a plain `HandleFastRingCompletionFunction`.
  Call `keep()` for multishot operations or when re-arming the same descriptor.
- The destructor detaches every still-submitted descriptor (clearing its handler and
  closure) and submits a `cancel64` for each, then releases the descriptors that were
  allocated but never submitted. Outstanding operations therefore do not call back
  into a destroyed `CoRing`.

See [Lifecycle.md](Lifecycle.md) for the underlying reference counting rules.

