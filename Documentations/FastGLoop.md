# FastGLoop API Reference

Header: `Ring/FastGLoop.h`

`FastGLoop` integrates GLib main loop processing with FastRing.

## How the Integration Works

GLib's main loop insists on owning the poll call, and FastRing's loop does the same.
`FastGLoop` reconciles them with two `ucontext` fibers: one runs the FastRing loop,
the other runs `GMainLoop`. GLib's `GPollFunc` is replaced by a function that hands
the descriptor set over to the FastRing side, switches to the FastRing fiber, and
resumes the GLib fiber once the ring reports readiness. Both loops therefore run on a
single OS thread and no locking is required between them.

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
  fiber on the current loop iteration. It is a no-op when called from inside the GLib
  fiber or when a switch is already scheduled.
- `StopFastGLoop()` requests loop stop before release.
- `HandleGLogReport()` can be used as a GLib log forwarder callback. Install it with
  `g_log_set_default_handler(HandleGLogReport, function)`, where `function` is a
  `void (*)(int priority, const char* format, ...)` reporter; GLib levels are mapped
  to syslog priorities.
- `interval` of `CreateFastGLoop()` bounds how long the ring may wait before giving
  the GLib fiber a chance to run.

