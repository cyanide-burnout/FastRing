# Resolver API Reference

Header: `Ring/Resolver.h`

`Resolver` drives a [c-ares](https://c-ares.org/) channel from a FastRing loop. It
supplies socket and timer plumbing only; queries are issued through the normal c-ares
API against `state->channel`.

## API

```c
struct ResolverState
{
  ares_channel channel;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
  uint32_t condition;
};

struct ResolverState* CreateResolver(struct FastRing* ring);
void UpdateResolverTimer(struct ResolverState* state);
void ReleaseResolver(struct ResolverState* state);
```

- `CreateResolver()` builds a channel with `ARES_FLAG_STAYOPEN` and installs a socket
  state callback that mirrors c-ares descriptors into the FastRing Poll API through
  `SetFastRingPoll()`: kernel multishot polls for `POLLIN | POLLERR | POLLHUP`, plus
  `POLLOUT` when c-ares wants to write; a socket c-ares no longer watches is removed.
  Returns `NULL` if allocation or `ares_init_options()` fails. It does not call
  `ares_library_init()`.
- `ReleaseResolver()` cancels the pending timeout, calls `ares_destroy()` and frees
  the state. `ares_destroy()` completes pending queries with `ARES_EDESTRUCTION`
  synchronously, on the calling thread. Called from a c-ares callback that the adapter is
  running, the release is deferred until that processing returns; repeated calls do
  nothing.
- `condition` is internal: it marks the adapter as inside c-ares processing and the
  release as requested.
- `state->channel` is the handle to pass to `ares_getaddrinfo()`, `ares_query()` and
  friends.

## The Timer Contract

c-ares computes its own next-timeout, and FastRing has to be told about it. That is
what `UpdateResolverTimer()` does: it reads `ares_timeout()` and re-arms a single
FastRing timeout accordingly.

The adapter calls it automatically after every socket event and after every timer
expiry. **The application must call it after starting a query**, because a freshly
submitted query changes the channel's timeout and nothing else will notice:

```c
ares_getaddrinfo(state->channel, name, service, &hints, callback, closure);
UpdateResolverTimer(state);
```

Without that call a query can sit until an unrelated event happens to refresh the
timer.

## Usage

```c
struct ResolverState* state = CreateResolver(ring);

ares_getaddrinfo(state->channel, "example.org", NULL, &hints, HandleResult, closure);
UpdateResolverTimer(state);
...
ReleaseResolver(state);
```

## Rules

- c-ares callbacks run inside `WaitForFastRing()` in the ring thread, inside
  `ReleaseResolver()` (`ARES_EDESTRUCTION`), and possibly inside the call that starts a
  query, which c-ares may complete at once.
- A query started from a c-ares callback run by the adapter needs no
  `UpdateResolverTimer()`; the adapter updates the timer when the processing returns.
- The adapter owns the poll registration for every descriptor the channel opens; do
  not register your own handler for those file descriptors.
- One `ResolverState` per channel. c-ares itself is not thread-safe per channel, so
  issue queries from the ring thread (or hop through `ThreadCall`).

## Lifecycle

`CreateResolver()` creates nothing on the ring until c-ares opens a socket. For every socket
c-ares reports, the adapter adds, updates or removes a Poll API registration with the state as
closure, and `UpdateResolverTimer()` keeps one timeout matching `ares_timeout()`. The timeout
is not `TIMEOUT_FLAG_OWNED`; when it fires the state forgets it and the core frees it.

`ares_destroy()` closes the sockets, which removes their poll registrations. Everything runs on
the ring thread, and the resolver is released before `ReleaseFastRing()`: if the ring is
released first, the polls and the timeout are freed without calling c-ares, and the release
touches freed memory.
