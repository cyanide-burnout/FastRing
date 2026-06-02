# Latch API Reference

Headers:

- `Supplimentary/Latch.h`
- `Supplimentary/LatchServer.h`
- `Supplimentary/LatchClient.h`

`Latch` provides a shared-memory locking primitive for putting an application into an idempotent state while critical work is in progress.
Its main use is to block concurrent data mutation across cooperating processes or threads until the protected section is complete.

## Overview

The implementation has two sides:

- `LatchServer` owns the shared-memory latch and serves lock requests through `FastRing`
- `LatchClient` maps the same latch and acquires/releases the lock

The shared state is a single `struct Latch` value stored in a `MAP_SHARED` mapping.
Clients publish a lock request into that word, the server observes it through an `io_uring` futex wait, grants the request, and wakes the waiters.

## API

Server side:

```c
struct LatchServer* CreateLatchServer(struct FastRing* ring, int handle);
void ReleaseLatchServer(struct LatchServer* server);
```

Client side:

```c
int LockLatch(struct LatchClient* client, struct timespec* timeout);
void UnlockLatch(struct LatchClient* client);

struct LatchClient* CreateLatchClient(int handle);
void ReleaseLatchClient(struct LatchClient* client);
```

## Semantics

- `CreateLatchServer()` truncates `handle` to `sizeof(struct Latch)`, maps it with `MAP_SHARED`, initializes the latch to `0`, and registers a futex wait through `FastRing`
- `CreateLatchClient()` maps an existing latch handle and validates that the file size matches `struct Latch`
- `LockLatch()` waits until the current thread receives the lock; returns `0` on success or a negative `errno` value on failure or timeout
- `UnlockLatch()` releases the latch only when the current client thread owns the granted lock
- `ReleaseLatchClient()` calls `UnlockLatch()`, then unmaps and closes the client handle
- `ReleaseLatchServer()` cancels the outstanding descriptor, then unmaps and closes the server handle

## Main Loop Usage

For a `main loop`, `Latch` acts as a coarse-grained write gate:

- create one `LatchServer` in the process that owns the `FastRing` loop
- share the backing handle with workers or helper processes
- call `LockLatch()` before a critical section that must freeze data mutation
- perform the protected read, transition, or recovery work
- call `UnlockLatch()` as soon as the critical section ends

This is useful when the main loop must temporarily stop writers and force a deterministic, idempotent phase around shared-memory-backed state.

## Notes

- Linux-only: relies on `futex()` and `io_uring` futex operations
- lock ownership is tracked with state bits plus the requesting thread id
- the API is intentionally low-level; caller code is responsible for sharing the backing handle and keeping critical sections short
