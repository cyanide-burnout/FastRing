# Latch API Reference

Headers:

- `Supplimentary/Latch.h` — shared state and bit layout
- `Supplimentary/LatchServer.h` — the side that runs in the ring
- `Supplimentary/LatchClient.h` — the side that takes the lock

`Latch` freezes a FastRing loop on request from another thread or process, so that
shared, memory-mapped state can be inspected or modified while nothing else is
touching it.

## What Actually Happens

The important part is not visible from the API: **while the latch is granted, the ring
thread is blocked inside `WaitForFastRing()`**. The server's completion handler enters
a `futex()` wait loop and does not return until the client releases the latch. No
completions are processed, no timeouts fire, nothing else in the ring runs.

That is the mechanism, not a side effect. "Stop the writers" here means "stop the
loop", which is why the whole design is built around holding the latch for as short a
time as possible and around detecting a holder that never releases.

Direction of control:

- the process owning the ring loop — the one that owns the shared pools, trees and
  filters — creates the **server**;
- a cooperating thread or process, which wants a consistent view of that state, creates
  a **client** and calls `LockLatch()`;
- the ring loop stops for the duration, and resumes on `UnlockLatch()`.

## Shared State

A single 64-bit word in a `MAP_SHARED` mapping of one file descriptor:

| Bits | Contents |
| --- | --- |
| `[1:0]` | `LATCH_STATE_LOCK_REQUESTED` (`0b01`), `LATCH_STATE_LOCK_GRANTED` (`0b10`), `LATCH_STATE_MASK` |
| `[31:8]` | Requesting thread id, `LATCH_TID_SHIFT` / `LATCH_TID_MASK` |
| `[63:32]` | Requesting process id, `LATCH_PID_SHIFT` / `LATCH_PID_MASK` |

`0` means unlocked and unrequested. Because the whole owner identity lives in the
word, the latch is strictly exclusive: one holder at a time, process-wide.

`LATCH(address)` selects the low 32-bit half of that word for `futex()`, picking the
correct half by endianness. The futex is **not** private — it is shared across
processes.

## Server

```c
struct LatchServer* CreateLatchServer(struct FastRing* ring, int handle);
void ReleaseLatchServer(struct LatchServer* server);
```

`CreateLatchServer()` truncates `handle` to `sizeof(struct Latch)`, maps it shared,
zeroes the word, and arms `IORING_OP_FUTEX_WAIT` on it. Any shared descriptor works —
a `memfd`, a tmpfs file — as long as the client can obtain it.

On wake-up the handler:

1. if the state is `LOCK_REQUESTED`, promotes it to `LOCK_GRANTED` and wakes every
   waiter;
2. then blocks in `futex(FUTEX_WAIT, 100 ms)` for as long as the state stays
   `LOCK_GRANTED`;
3. re-arms the futex wait and returns.

`ReleaseLatchServer()` cancels the descriptor, unmaps and closes the handle.

### Dead-holder recovery

Every 100 ms while blocked, the server checks the holder with `kill(pid, 0)`. If the
process is gone (`ESRCH`), it releases the latch on the holder's behalf and wakes
everyone. A client that crashes therefore unwedges the ring within about 100 ms rather
than hanging it forever.

The check is per **process**, not per thread. A client process that is still alive but
whose locking thread is stuck holds the ring indefinitely — the recovery path does not
help there.

## Client

```c
struct LatchClient* CreateLatchClient(int handle);
void ReleaseLatchClient(struct LatchClient* client);

int LockLatch(struct LatchClient* client, struct timespec* timeout);
void UnlockLatch(struct LatchClient* client);
```

`CreateLatchClient()` maps an existing latch handle, requiring the file size to match
`sizeof(struct Latch)` exactly — a mismatch is how a wrong descriptor is caught.

`LockLatch()` publishes `(pid, tid, LOCK_REQUESTED)` when the word is `0`, wakes the
server, and waits for the grant. `timeout` is an absolute `CLOCK_MONOTONIC` deadline,
or `NULL` to wait indefinitely.

Return values:

- `0` — the latch is held by the calling thread;
- negative `errno` — `-ETIMEDOUT` on deadline, `-EINVAL` for a `NULL` client;
- **`0` is also possible on a timeout that raced with the grant.** The timeout path
  tries to roll the pending request back to `0`; if that fails because the server had
  already granted it, the call succeeds. Never treat a deadline as "did not acquire" —
  check the return value.

Calling `LockLatch()` again from the thread that already holds the latch returns `0`
immediately without a second grant. It is not counted, so a single `UnlockLatch()`
releases it.

`UnlockLatch()` releases only if the shared word still matches what this client
recorded at grant time, then wakes the server and any waiters. It is safe to call when
nothing is held. `ReleaseLatchClient()` unlocks, unmaps and closes.

## Usage

Ring side, in the object owning the shared state:

```c
server = CreateLatchServer(ring, handles[HANDLE_LATCH]);
...
ReleaseLatchServer(server);
```

Client side, exposing a guarded section to whatever needs it — a scripting binding, a
worker thread, a maintenance process:

```c
client = CreateLatchClient(handle);
...
result = LockLatch(client, &deadline);

if (result < 0)
  return Fail(strerror(-result));

// the ring loop is stopped here — read or fix the shared state

UnlockLatch(client);
```

## Rules

- **Keep the critical section as short as possible.** Everything the ring serves —
  sockets, timers, watchdog — is stalled for its full duration.
- Never call `LockLatch()` from the ring thread. The server handler that would grant it
  cannot run, so the call deadlocks.
- A `LatchClient` carries the grant of one thread. Use one client per thread rather
  than sharing.
- Do not perform blocking or unbounded work while holding the latch, and set a
  `timeout` rather than passing `NULL` in anything that must not hang.
- Linux-only: relies on `futex()` and io_uring futex operations, so liburing >= 2.6
  and a kernel with `IORING_OP_FUTEX_WAIT`.
