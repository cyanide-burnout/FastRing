# FastSemaphore API Reference

Header: `Ring/FastSemaphore.h`

`FastSemaphore` makes a plain POSIX `sem_t` reactive: instead of a thread parked in
`sem_wait()`, tokens are delivered to a callback on the ring thread.

This is the bridge for code that already communicates through `sem_t` — a producer
thread keeps calling `sem_post()`, while the consumer side becomes an ordinary
FastRing handler.

## Requirements

The module reaches into glibc's semaphore layout and drives its futex word directly.
The header enforces both preconditions at compile time:

- **glibc >= 2.34** — the `struct new_sem` layout it assumes (64-bit `data` with token
  count in the low half and waiter count in the high half, plus a `private` flag).
- **liburing >= 2.6** — `IORING_OP_FUTEX_WAIT` / `IORING_OP_FUTEX_WAKE`.

It is, by the header's own description, a workaround over glibc internals. A glibc
that changes that layout breaks it silently at runtime, so re-verify when moving to a
new libc.

## API

```c
// Return 1 to keep waiting for more tokens, 0 to unregister
typedef int (*FastSemaphoreFunction)(sem_t* semaphore, void* closure);

struct FastRingDescriptor* RegisterFastSemaphore(struct FastRing* ring, sem_t* semaphore,
                                                 FastSemaphoreFunction function, void* closure, int limit);
void CancelFastSemaphore(struct FastRingDescriptor* descriptor);

int PostFastSemaphore(struct FastRing* ring, sem_t* semaphore);
```

### Waiting

`RegisterFastSemaphore()` registers the ring as a waiter on the semaphore and arms a
futex wait. Each time tokens appear, the handler acquires them one at a time and calls
`function` once per token, then re-arms.

- `limit` bounds how many tokens are consumed per completion, so one busy producer
  cannot monopolise the loop. The rest are picked up on the next pass.
- The callback's return value is the subscription: `1` keeps waiting, `0` unregisters
  and lets the descriptor go.
- **The token is already consumed when the callback runs.** Do not call `sem_wait()`
  or `sem_trywait()` inside it.
- The semaphore must stay valid for as long as the registration exists.

`CancelFastSemaphore()` unregisters and cancels the outstanding futex wait. It is a
no-op while the callback is on the stack (the module tracks that in `state`), so
cancelling from inside the callback does nothing — return `0` from the callback
instead.

### Posting

`PostFastSemaphore()` replaces `sem_post()` when posting **from the ring thread**: it
increments the token count exactly like glibc does, and when waiters exist it submits
the futex wake through io_uring instead of issuing a syscall.

Returns `0`, `-EOVERFLOW` if the semaphore is already at `SEM_VALUE_MAX`, or `-ENOMEM`
if no descriptor could be allocated.

Threads outside the ring keep using the ordinary `sem_post()` — the registration waits
on the same futex word either way. `PostFastSemaphore()` is an optimisation for the
ring side, not a requirement.

## Usage

```c
static int HandleToken(sem_t* semaphore, void* closure)
{
  DrainOneItem((struct Consumer*)closure);
  return 1;                                    // keep waiting
}

descriptor = RegisterFastSemaphore(ring, &semaphore, HandleToken, consumer, 16);
...
CancelFastSemaphore(descriptor);
```

## Rules

- One registration per semaphore. Two waiters on the same `sem_t` would both
  manipulate the waiter count.
- The callback runs in the ring thread and must not block.
- Mixing a blocking `sem_wait()` on the same semaphore with a registration works —
  glibc and the ring compete for tokens as ordinary waiters — but which side gets a
  given token is unspecified.
