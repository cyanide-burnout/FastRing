# FastGLoop API Reference

Header: `Ring/FastGLoop.h`

`FastGLoop` integrates GLib main loop processing with FastRing.

## How the Integration Works

GLib's main loop insists on owning the poll call, and FastRing's loop does the same.
`FastGLoop` reconciles them with two `ucontext` contexts: the FastRing loop runs on the
thread's own stack, and `GMainLoop` runs on a fiber with a stack of its own, `RLIMIT_STACK`
bytes allocated with `malloc()` (with an unlimited stack limit the allocation fails and
`CreateFastGLoop()` returns `NULL`). GLib's `GPollFunc` is replaced by a function that hands
the descriptor set over to the FastRing side and switches back to it. The ring arms one
one-shot poll per descriptor and one timeout for GLib's poll timeout; a completion of any of
them, or `TouchFastGLoop()`, arms a flush handler that reports the results to GLib and
resumes its fiber. Both loops therefore run on a single OS thread and no locking is required
between them.

`IsInFastGLoop()` returns non-zero while execution is inside the GLib fiber, which is
how a module can tell which side of the switch it is running on.

The loop creates its own `GMainContext` but does not install it as the thread-default
context. Do that yourself if GLib libraries should attach to it automatically:

```c
g_main_context_push_thread_default(loop->context);
...
g_main_context_pop_thread_default(loop->context);
```

Because the switch is cooperative, a GLib callback that blocks stalls the ring, and a
FastRing callback that blocks stalls GLib.

## API

```c
struct FastGLoop* CreateFastGLoop(struct FastRing* ring, int interval);
void ReleaseFastGLoop(struct FastGLoop* loop);
void TouchFastGLoop(struct FastGLoop* loop);
void StopFastGLoop(struct FastGLoop* loop);
int IsInFastGLoop();

void HandleGLogReport(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data);
```

## Notes

- `TouchFastGLoop()` schedules a FastRing flush handler that switches into the GLib
  fiber: in the current iteration when called during `WaitForFastRing()`, otherwise in
  the next one. It is a no-op when called from inside the GLib fiber or when a switch is
  already scheduled. Call it after changing GLib sources from outside the GLib fiber, since
  GLib re-evaluates its poll set only when its fiber runs.
- `StopFastGLoop()` quits the GLib loop synchronously: it calls `g_main_loop_quit()` and
  switches into the fiber so that `g_main_loop_run()` returns. It does not cancel the ring
  polls or the timeout, and the loop cannot be restarted. `ReleaseFastGLoop()` stops the loop
  itself, so the call is optional.
- `HandleGLogReport()` can be used as a GLib log forwarder callback. Install it with
  `g_log_set_default_handler(HandleGLogReport, function)`, where `function` is a
  `void (*)(int priority, const char* format, ...)` reporter; GLib levels are mapped
  to syslog priorities.
- `interval` of `CreateFastGLoop()` (milliseconds, `1000` when not positive) is the period
  of a dummy GLib timeout source, so GLib never asks for an infinite poll; it bounds how
  long the GLib fiber may go without running.
- A poll that fails is not reported to GLib: its `revents` is `0`, and it is armed again in
  the next GLib iteration.

## Lifecycle

`CreateFastGLoop()` allocates the loop, the fiber stack, a `GMainContext` with the dummy
timeout source and a `GMainLoop`, switches into the fiber once so that GLib reports its poll
set, and arms the ring side. Call it on the ring thread.

For every GLib iteration the adapter keeps at most one one-shot poll descriptor per file
descriptor and one timeout descriptor. A changed mask cancels the old poll with a transient
`IORING_OP_POLL_REMOVE` and adds a new one; the timeout is updated with a transient
`IORING_OP_TIMEOUT_REMOVE`. Transient descriptors are released by their own completions and do
not touch the loop.

`ReleaseFastGLoop()` stops the GLib loop if its fiber is parked, abandons the timeout and every
poll descriptor with `DiscardFastRingDescriptor()`, cancels a pending flush handler, and frees
the GLib objects, the fiber stack and the loop. Call it on the ring thread, outside any GLib
callback — from inside the fiber the switch would land in a stale context — and before
`ReleaseFastRing()`: if the ring is released first, it frees the descriptors and the flush
handler without calling back, and `ReleaseFastGLoop()` touches freed memory.

Once the GLib loop has returned, after `StopFastGLoop()` or a `g_main_loop_quit()` issued by
GLib code, the polls and the timeout of its last iteration are still armed, and their next
completion switches into a fiber that is no longer running. Release the loop before the ring
handles another completion.

