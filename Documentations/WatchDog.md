# WatchDog API Reference

Header: `Ring/WatchDog.h`

`WatchDog` keeps the systemd watchdog fed from a FastRing timeout, so a stalled ring
loop is what systemd actually detects.

## API

```c
struct WatchDog
{
  int state;
  uint64_t interval;                      // notification interval, milliseconds
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

struct WatchDog* CreateWatchDog(struct FastRing* ring);
void ReleaseWatchDog(struct WatchDog* state);
```

`CreateWatchDog()`:

- returns `NULL` when `sd_watchdog_enabled()` reports the watchdog is disabled or
  unavailable — this is the normal result outside a systemd unit with
  `WatchdogSec=`, not an error;
- sends `MAINPID=` for the current process;
- arms a repeating FastRing timeout at **a quarter** of the interval systemd allows,
  so three notifications can be missed before the unit is killed.

The first expiry sends `READY=1` together with `WATCHDOG=1`, every later one sends
`WATCHDOG=1` alone. That makes it usable as the readiness notification for
`Type=notify` units as well — the service is reported ready once the ring loop has
actually started turning.

`ReleaseWatchDog()` cancels the timeout and frees the state.

## Usage

```c
struct WatchDog* watchdog = CreateWatchDog(ring);   // NULL is fine, just means "not under systemd"

while (running)
  WaitForFastRing(ring, interval, NULL);

ReleaseWatchDog(watchdog);
```

`ReleaseWatchDog(NULL)` is safe, so the return value needs no special handling.

## Rules

- The notification is driven by a ring timeout, so it stops as soon as the loop stops
  running. That is the point: a handler that blocks the ring will trip the watchdog.
- Do not also call `sd_notify(0, "WATCHDOG=1")` from elsewhere; that would mask
  exactly the stall this module is meant to expose.
- Requires linking against `libsystemd`.
