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
  requests.
- **dispatch status function** - when libdbus reports that messages are pending, a
  one-shot FastRing flush handler is armed to call `dbus_connection_dispatch()` after
  the current CQ batch. It dispatches one message per call and re-arms itself until
  the status is `DBUS_DISPATCH_COMPLETE`. Because the flush phase drains its stack to
  the end, that re-arm runs within the same `WaitForFastRing()` iteration — the queue
  is emptied before the loop moves on, one message at a time rather than in a single
  blocking sweep. See the flush handler section of [FastRing.md](FastRing.md).

`ReleaseDBusCore()` removes the flush handler, unhooks all three callback sets and
drops the connection reference. It does not close or disconnect the connection —
call `dbus_connection_close()` / `dbus_connection_unref()` yourself if you own it.

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
