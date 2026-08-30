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
};

struct ResolverState* CreateResolver(struct FastRing* ring);
void UpdateResolverTimer(struct ResolverState* state);
void ReleaseResolver(struct ResolverState* state);
```

- `CreateResolver()` builds a channel with `ARES_FLAG_STAYOPEN` and installs a socket
  state callback that mirrors c-ares descriptors into the FastRing Poll API through
  `SetFastRingPoll()`. Returns `NULL` if allocation or `ares_init_options()` fails.
- `ReleaseResolver()` cancels the pending timeout, calls `ares_destroy()` and frees
  the state.
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

- c-ares callbacks run inside `WaitForFastRing()` in the ring thread.
- The adapter owns the poll registration for every descriptor the channel opens; do
  not register your own handler for those file descriptors.
- One `ResolverState` per channel. c-ares itself is not thread-safe per channel, so
  issue queries from the ring thread (or hop through `ThreadCall`).
