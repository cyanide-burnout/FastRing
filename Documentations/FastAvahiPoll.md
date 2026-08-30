# FastAvahiPoll API Reference

Header: `Ring/FastAvahiPoll.h`

`FastAvahiPoll` implements Avahi's `AvahiPoll` interface on top of FastRing, so an
`avahi-client` runs inside the ring loop instead of Avahi's own `AvahiSimplePoll` or
a GLib main loop.

## API

```c
AvahiPoll* CreateFastAvahiPoll(struct FastRing* ring);

#define ReleaseFastAvahiPoll  free
```

`CreateFastAvahiPoll()` allocates and fills an `AvahiPoll` vtable, storing the ring in
`poll->userdata`. There is no private state beyond that, which is why the release is
literally `free()`.

The result is passed wherever Avahi wants an `AvahiPoll*`:

```c
struct FastRing* ring = CreateFastRing(0);
AvahiPoll* poll       = CreateFastAvahiPoll(ring);

client = avahi_client_new(poll, 0, HandleClientEvent, closure, &error);
```

## What It Maps To

| Avahi operation | FastRing mechanism |
| --- | --- |
| `watch_new` / `watch_update` | `SetFastRingWatch()` — the **Watch API**, not the Poll API |
| `watch_free` | `RemoveFastRingWatch()` |
| `watch_get_events` | Returns the event mask from the last completion |
| `timeout_new` / `timeout_update` | `SetFastRingCertainTimeout()`, absolute time on the realtime clock |
| `timeout_free` | Timeout removal |

Two consequences of those choices:

- Watches go through the Watch API, which is addressed by descriptor rather than by
  file descriptor. Avahi's watches therefore do not collide with anything the
  application has registered for the same descriptor through `AddFastRingPoll()`.
- Avahi schedules timeouts as **absolute times on the realtime clock**, and the
  adapter passes them through as such. They are one-shot: the descriptor is cleared
  when it fires, and `timeout_update` re-arms it. A `timeout_new()` with a `NULL`
  value creates an unarmed timeout that a later `timeout_update()` starts, which is
  how Avahi expects it to behave.

## Using the Poll for Your Own Timers

`AvahiPoll` is a public vtable, and the adapter implements all of it, so application
code that already holds the `poll` can schedule its own timers through it rather than
reaching for a separate FastRing timeout. A reconnect backoff, for instance:

```c
struct timeval interval;

interval.tv_sec  =  RESTART_TIMEOUT / 1000;
interval.tv_usec = (RESTART_TIMEOUT % 1000) * 1000;

if (discovery->timeout != NULL)
  poll->timeout_update(discovery->timeout, &interval);
else
  discovery->timeout = poll->timeout_new(poll, &interval, HandleTimeoutEvent, discovery);
```

Remember to free such timeouts with `poll->timeout_free()` before releasing the poll.

## Lifetime and Ownership

The poll object holds **no registry** of the watches and timeouts created through it.
`ReleaseFastAvahiPoll()` frees the vtable and nothing else, so anything still alive at
that moment leaks and keeps pointing at freed memory.

That gives one hard ordering rule — destroy everything the poll serves, then the poll,
then the ring:

```c
ReleaseInstantDiscovery(discovery);   // frees browser, group, client and its own timeouts
ReleaseFastAvahiPoll(poll);
ReleaseFastRing(ring);
```

Conversely, the poll is **reusable across client restarts**. On
`AVAHI_CLIENT_FAILURE` the usual recovery is to free the browser, entry group and
client, and call `avahi_client_new()` again with the same `AvahiPoll*`:

```c
avahi_service_browser_free(browser);
avahi_entry_group_free(group);
avahi_client_free(client);

client = avahi_client_new(poll, 0, HandleClientEvent, closure, &error);
```

Nothing in the adapter is tied to a particular client, so one poll serves the whole
life of the process.

## Rules

- All Avahi callbacks run in the ring thread inside `WaitForFastRing()` and must not
  block.
- One poll per ring is enough; several clients may share it.
- `avahi_client_new()` failing does not invalidate the poll — retry with the same one.

## Example

`Examples/Avahi` covers both directions against one poll: an entry group that
publishes a service, and a browser that resolves the services it finds.
