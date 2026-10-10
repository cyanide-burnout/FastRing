# DBusCore API Reference

Header: `Ring/DBusCore.h`

`DBusCore` drives a libdbus connection from a FastRing loop. It is a pure adapter:
messages, matches and method calls stay entirely on the normal libdbus API, and
`DBusCore` only supplies the event plumbing libdbus asks for.

## API

```c
struct DBusCore* CreateDBusCore(DBusConnection* connection, struct FastRing* ring);
void ReleaseDBusCore(struct DBusCore* core);
```

`CreateDBusCore()` takes its own reference on the connection
(`dbus_connection_ref()`) and installs three sets of libdbus callbacks:

- **watch functions** - every `DBusWatch` becomes a FastRing watch descriptor
  (`AddFastRingWatch()`), with the D-Bus flags mapped to `poll` flags and back. A
  watch created or toggled into the disabled state is simply not armed.
- **timeout functions** - every `DBusTimeout` becomes a repeating FastRing timeout
  (`SetFastRingTimeout()` with `TIMEOUT_FLAG_REPEAT`) at the interval libdbus
  requests. A FastRing built with liburing 2.4 or newer makes it a kernel multishot
  timeout, which needs Linux 6.4; see
  [Repeating timeouts](LIFECYCLE.md#repeating-timeouts).
- **dispatch status function** - when libdbus reports that messages are pending, a
  one-shot FastRing flush handler is armed to call `dbus_connection_dispatch()` after
  the current CQ batch. It dispatches one message per call and re-arms itself until
  the status is `DBUS_DISPATCH_COMPLETE`. Because the flush phase drains its stack to
  the end, that re-arm runs within the same `WaitForFastRing()` iteration — the queue
  is emptied before the loop moves on, one message at a time rather than in a single
  blocking sweep. See the flush handler section of [API.md](API.md#flush-handlers).
  `CreateDBusCore()` arms one dispatch unconditionally, for messages queued before the
  adapter was installed.

`ReleaseDBusCore()` removes the flush handler, unhooks all three callback sets and
drops the connection reference. Unhooking makes libdbus call the remove functions of the
adapter for every watch and timeout, which is where their ring objects go away. It does not
close or disconnect the connection — call `dbus_connection_close()` /
`dbus_connection_unref()` yourself if you own it. Called from a message handler run by the
adapter's dispatch, it does the same, and the `DBusCore` itself is freed once
`dbus_connection_dispatch()` returns.

## Usage

```c
DBusConnection* connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
struct DBusCore* core      = CreateDBusCore(connection, ring);
...
ReleaseDBusCore(core);
```

The `DBusCore` handle is opaque and is only needed for the release call.

## Rules

- All libdbus callbacks (message filters, method handlers, reply notifiers) end up
  running inside `WaitForFastRing()` in the ring thread. The usual rule applies: do
  not block them.
- Do not install your own watch, timeout or dispatch-status functions on a connection
  handed to `DBusCore` — they would replace the adapter's.
- Only the connection is adapted. `DBusServer`, and libdbus's own private loop
  (`dbus_connection_read_write_dispatch()`), are not used and must not be mixed in.
- One `DBusCore` per connection. Adapt the system and session buses separately if both
  are needed.
- Use the connection only from the ring thread. libdbus calls the watch, timeout and
  dispatch-status functions on whichever thread touches the connection, and they remove
  timeouts and arm flushers, which belong to the ring thread.

## Lifecycle

An enabled `DBusWatch` becomes one Watch API descriptor (a poll the core re-arms after every
callback; an error result reaches libdbus as `DBUS_WATCH_ERROR`), stored with
`dbus_watch_set_data()`. An enabled `DBusTimeout` becomes one repeating timeout, not
`TIMEOUT_FLAG_OWNED`, stored with `dbus_timeout_set_data()`. Disabling or removing either
removes the ring object, also from inside its own callback, which libdbus does on disconnect or
on a reply timeout. At most one dispatch flusher is pending.

Release the adapter before `ReleaseFastRing()`. If the ring is released first, the watches,
timeouts and flusher are freed without calling libdbus, and `ReleaseDBusCore()` then touches
freed memory.
