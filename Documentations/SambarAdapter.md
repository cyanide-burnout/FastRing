# SambarAdapter API Reference

Header: `Ring/SambarAdapter.h`

`SambarAdapter` drives [libsmb2](https://github.com/sahlberg/libsmb2) from a
`FastRing` loop. It covers both directions: attaching an `smb2_context` to the ring,
and accepting inbound SMB2 connections for a server built on libsmb2.

Status: **experimental**. It has been exercised by the SMB examples in this
repository, but it is not as hardened as the core modules.

libsmb2 is used unpatched — avoiding a fork of libsmb2 is the whole point of this
adapter. See [Constraints](#constraints) for what that costs.

## Client Side

```c
struct SambarOpaque
{
  struct FastRing* ring;
  struct FastRingDescriptor* timeout;
};

void AttachSambarOpaque(struct SambarOpaque* opaque, struct FastRing* ring, struct smb2_context* context, int force);
void DetachSambarOpaque(struct SambarOpaque* opaque, struct smb2_context* context);
void* GetSambarClosure(struct smb2_context* context, int offset);

#define GetSambarData(context, type, field)  ((type*)GetSambarClosure(context, offsetof(type, field)))
```

`AttachSambarOpaque()`:

- stores `opaque` as the libsmb2 opaque pointer (`smb2_set_opaque()`), so the adapter
  owns that slot for the lifetime of the context;
- installs `smb2_fd_event_callbacks()` handlers that mirror libsmb2's descriptor and
  event-mask changes into the FastRing Poll API;
- arms a repeating 1000 ms timeout that calls `smb2_service_fd(context, -1, 0)` so
  libsmb2 can run its own timers and time out stalled requests.

Poll requests are armed with `RING_POLL_ERROR | RING_POLL_HANGUP | RING_POLL_SHOT |
RING_POLL_REPEAT` in addition to the mask libsmb2 asks for, so a single armed request
survives across events.

`force != 0` arms the current descriptor immediately instead of waiting for the first
`SMB2_ADD_FD` callback. Use it when the context is already connected at attach time.

`DetachSambarOpaque()` removes the poll registration for the context's current
descriptor and cancels the service timeout. It does not free the context — call
`smb2_destroy_context()` yourself afterwards.

### Embedding the opaque

`struct SambarOpaque` is meant to be embedded in the owning object, and
`GetSambarData()` recovers the owner from an `smb2_context`:

```c
struct SombreroSession
{
  ...
  struct SambarOpaque sambar;
  ...
};

struct SombreroSession* GetSession(struct smb2_context* context)
{
  return GetSambarData(context, struct SombreroSession, sambar);
}
```

Because the adapter occupies libsmb2's opaque pointer, this macro is the only
supported way to get from a context back to application state. `GetSambarClosure()`
returns `NULL` when no opaque is set.

## Server Side

```c
struct SambarListener
{
  int handle;
  void* closure;
  smb2_client_connection function;
  struct FastRingDescriptor* accept;
};

struct SambarListener* OpenSambarListener(struct FastRing* ring, uint16_t port, smb2_client_connection function, void* closure);
void CloseSambarListener(struct SambarListener* listener);
```

`OpenSambarListener()` creates a dual-stack (`AF_INET6`) listening socket with
`SO_REUSEADDR`, `SO_REUSEPORT`, `TCP_NODELAY` and `TCP_DEFER_ACCEPT`, and arms a
re-submitted `IORING_OP_ACCEPT` on the ring. For every accepted connection it builds a
fresh `smb2_context`, installs the accepted descriptor into it, and calls `function`.
This is the asynchronous equivalent of libsmb2's `smb2_serve_port_async()`.

The callback owns the context: it is expected to call `AttachSambarOpaque()` and,
eventually, `DetachSambarOpaque()` plus `smb2_destroy_context()`. If
`smb2_init_context()` fails, the accepted descriptor is closed and no callback is made.

`CloseSambarListener()` detaches the accept handler, closes the listening socket and
frees the listener. The outstanding accept descriptor is left to be reclaimed by the
ring on its next completion.

## Constraints

- **libsmb2's opaque pointer is reserved.** Do not call `smb2_set_opaque()` on a
  context that is attached, and do not install your own
  `smb2_fd_event_callbacks()`.
- **The accept path writes libsmb2's transport descriptor directly.**
  `HandleAcceptCompletion()` stores the accepted handle into the first `int`-sized
  field of `struct smb2_context`, mirroring what `smb2_serve_port_async()` does
  internally. This keeps libsmb2 unpatched, but it depends on that field staying
  first in the structure. Re-verify it when upgrading libsmb2.
- **Timer granularity is fixed** at 1000 ms (`SAMBAR_TIMEOUT_INTERVAL`), which bounds
  how quickly libsmb2 detects its own timeouts.
- **One poll registration per descriptor.** The adapter routes through
  `SetFastRingPoll()`, so an application must not register its own poll handler for a
  descriptor owned by an attached `smb2_context`.

## Examples

- `Examples/SMBServer` — SMB2 server (`SMBServer.c`, `Sombrero.c`) built on
  `OpenSambarListener()` and an embedded `SambarOpaque` per session.
- `Examples/SMBClient` — named-pipe client (`SambarPipe.c`, `SMBPipeEcho.c`) built on
  `AttachSambarOpaque()`.
