# KCPAdapter API Reference

Header: `Supplimentary/KCPAdapter.h`

`KCPAdapter` is the ready-made transport binding for `KCPService`: it owns a UDP
socket, feeds inbound datagrams into the engine, and installs itself as the engine's
transmitter. With it, an application only deals with conversations and messages — see
[KCPService.md](KCPService.md).

## API

```c
typedef const struct KCPFormat* (*ValidateKCPPacket)(
  struct KCPAdapter* adapter,
  struct sockaddr* address,
  uint8_t* data,
  int length);

struct KCPAdapter* CreateKCPAdapter(
  struct FastRing* ring,
  struct KCPService* service,
  const struct KCPFormat* format,
  ValidateKCPPacket validate,
  void* closure,
  int port);

void ReleaseKCPAdapter(struct KCPAdapter* adapter);
```

`CreateKCPAdapter()` requires at least one of `format` and `validate`. It:

- binds an `AF_INET6` UDP socket to `[::]:port` (dual-stack unless
  `net.ipv6.bindv6only` is set) with `SO_REUSEADDR`, `IPV6_RECVPKTINFO` and
  `SO_TIMESTAMPING` (software RX timestamps);
- creates inbound and outbound `FastBufferPool`s and a ring buffer provider
  (2048 buffers of 2048 bytes);
- wraps the socket in a `FastSocket` in `FASTSOCKET_MODE_ZERO_COPY`;
- sets `service->transmitter` to its own transmitter, built on `send_zc`;
- arms a repeating timeout at the service congestion interval, read once at creation,
  that refreshes the realtime/monotonic delta and calls
  `FlushKCPService(service, KCP_FLUSH_SEND | KCP_FLUSH_CLEANUP)`.

Failures of the pools, the provider, the socket and the timer are not checked.

Because the adapter installs the transmitter, the service is created without one:

```c
struct KCPHandler handler = { .closure = self, .handle = HandleKCPEvent };

service = CreateKCPService(&handler, NULL);              // no transmitter here
adapter = CreateKCPAdapter(ring, service, &MyKCPFormat, NULL, NULL, port);
```

Teardown is the reverse order — release the adapter, then the service:

```c
ReleaseKCPAdapter(adapter);
ReleaseKCPService(service);
```

`ReleaseKCPAdapter()` releases the socket, the buffer provider, both pools and the
flush timeout. It does not touch the service, so `service->transmitter` points into the
freed adapter until the service is released: between the two calls the service must not be
flushed or fed packets.

## Choosing the Format Per Packet

With `validate == NULL`, every accepted datagram is parsed with the single `format`
given at construction.

Supplying `validate` instead lets the adapter demultiplex several envelopes on one
port: it is called with the peer address and the raw datagram, and returns the
`KCPFormat` to use, or `NULL` to drop the packet. When both are given, `validate` is used
and `format` ignored. This is also the natural place for
cheap source filtering before any conversation state is created.

## What the Adapter Adds Per Packet

- **RX timestamps.** `SCM_TIMESTAMPING` control data is converted from realtime to the
  monotonic clock (the adapter keeps a refreshed realtime/monotonic delta) and passed
  to `HandleKCPPacket()` as the packet time, so RTT estimation uses kernel arrival
  time rather than the moment the loop got round to it.
- **Destination address.** `IPV6_PKTINFO` is turned into a `struct KCPPoint`, so a
  service bound to a wildcard address knows which local address a conversation arrived
  on. V4-mapped addresses are unmapped to `AF_INET` there. Peer addresses — the address
  given to `validate` and `conversation->key.address` — stay `AF_INET6`, v4-mapped, which
  matters for keys passed to `CreateKCPConversation()`.
- **Buffer lifetime.** Inbound `FastBuffer`s are passed to the engine with
  `HoldFastBuffer` / `ReleaseFastBuffer` as the acquire/release pair, so a segment that
  the engine keeps for reassembly pins only its own buffer.
- **Immediate flush.** After a packet updates a conversation, its outbound queue is
  flushed at once, so ACKs do not wait for the next timer tick.

## Rules

- One adapter per service. The adapter overwrites `service->transmitter`.
- Release the adapter first and the service right after it, with no flush and no packet
  in between. Segment buffers from the adapter's pools survive the adapter, since the pools
  are reference counted.
- All handler callbacks run in the ring thread inside `WaitForFastRing()`.
- The adapter always creates and binds its own socket. To serve IPv4 or IPv6 only, filter
  in `validate`.
- The event of the socket callback is not inspected: on an error the adapter only drains
  what is queued, and it does not recreate the socket; sends then fail with `-EPIPE`.

## Lifecycle

On the ring the adapter owns a `FastSocket` (a multishot `recvmsg` and one `send_zc` per
packet, each holding the service's buffer until its completion), a buffer provider fed from
the inbound pool, and a repeating timeout. `ReleaseKCPAdapter()` releases the socket — its
receive is cancelled and its descriptors go when the cancellation and the sends in flight
have completed — then the provider, its references to the pools and the timeout. The timeout
callback is not called afterwards and the socket callback is detached.

Release the adapter before the ring. If `ReleaseFastRing()` runs first, the timer is dropped
without a callback, the socket callback is called once with `POLLHUP`, the socket's
descriptors are freed by the ring, and the adapter can no longer be released safely.
