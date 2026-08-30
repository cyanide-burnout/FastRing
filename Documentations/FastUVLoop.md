# FastUVLoop API Reference

Header: `Ring/FastUVLoop.h`

`FastUVLoop` runs a `libuv` loop inside a FastRing loop, so libuv-based libraries
(H2O, for example) can be used without giving them their own thread.

## How the Integration Works

Unlike `FastGLoop`, which switches between two `ucontext` fibers, `FastUVLoop` never
lets libuv block:

- libuv's backend descriptor (`uv_backend_fd()`) is polled for `POLLIN` through a
  FastRing descriptor;
- when it signals, or when `TouchFastUVLoop()` is called, a one-shot FastRing flush
  handler is armed;
- that handler runs `uv_run(loop, UV_RUN_NOWAIT)` — a single non-blocking iteration —
  and then arms a FastRing timeout for `uv_backend_timeout()`, capped by the
  `interval` given at construction.

So libuv advances once per FastRing loop iteration, on the ring thread, and its
timers are served by ring timeouts. Nothing is ever waited on inside libuv.

## API

```c
struct FastUVLoop* CreateFastUVLoop(struct FastRing* ring, int interval);
void ReleaseFastUVLoop(struct FastUVLoop* loop);
void TouchFastUVLoop(struct FastUVLoop* loop);
void DepleteFastUVLoop(struct FastUVLoop* loop, int timeout, uint64_t kick, CheckUVLoopDepletion function, void* closure);
```

- `CreateFastUVLoop()` initialises its own `uv_loop_t` (reachable as `loop->loop`,
  which is what libuv-based libraries are handed) and arms the poll. `interval` is an
  upper bound in milliseconds on how long libuv may go without an iteration; pass `0`
  or a negative value to rely purely on `uv_backend_timeout()`.
- `TouchFastUVLoop()` requests an iteration on the current cycle. It is idempotent
  within a cycle — the flush handler is armed at most once. Call it after handing work
  to libuv from outside a libuv callback.
- `ReleaseFastUVLoop()` cancels the poll and the timeout, removes the flush handler
  and calls `uv_loop_close()`.

## Draining Before Shutdown

`uv_loop_close()` fails if handles are still active, and some libraries leave timers
or handles behind. `DepleteFastUVLoop()` is the shutdown helper for that:

```c
void DepleteFastUVLoop(struct FastUVLoop* loop, int timeout, uint64_t kick,
                       CheckUVLoopDepletion function, void* closure);
```

It optionally kicks the loop's handles, then spins `uv_run(UV_RUN_NOWAIT)` with a
blocking `poll()` on the backend descriptor between iterations, until one of:

- the loop reports no more active handles,
- `function(closure)` returns `0`,
- `timeout` milliseconds have passed.

`kick` is a bit mask applied through `uv_walk()` before draining:

- `UVLOOP_KICK_UNREF(type)` — `uv_unref()` every non-closing handle of the given
  `uv_handle_type`, so it no longer keeps the loop alive. Combine several types by
  OR-ing.
- `UVLOOP_KICK_POKE_TIMER` — make every active one-shot timer fire immediately
  instead of waiting out its delay.

Pass `0` to skip the kick entirely.

**This function blocks the calling thread**, which means it blocks the ring. That is
intended: it is meant for the teardown path, between "stop accepting work" and
`ReleaseFastUVLoop()`, not for steady-state operation.

## Rules

- All libuv callbacks run in the ring thread inside `WaitForFastRing()`, and must not
  block.
- Hand `loop->loop` to libuv-based libraries; do not create a second `uv_loop_t`.
- Do not call `uv_run()` with `UV_RUN_DEFAULT` or `UV_RUN_ONCE` on this loop — that
  would block the ring. The adapter owns loop iteration.
