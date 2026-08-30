# KCPAdapter API Reference

Header: `Supplimentary/KCPAdapter.h`

`KCPAdapter` is the ready-made transport binding for `KCPService`: it owns a UDP
socket, feeds inbound datagrams into the engine, and installs itself as the engine's
transmitter. With it, an application only deals with conversations and messages — see
`Documentations/KCPService.md`.

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

- binds a dual-stack `AF_INET6` UDP socket on `port` with `SO_REUSEADDR`,
  `IPV6_RECVPKTINFO` and `SO_TIMESTAMPING` (software RX timestamps);
- creates inbound and outbound `FastBufferPool`s and a ring buffer provider
  (2048 buffers of 2048 bytes);
- wraps the socket in a `FastSocket` in `FASTSOCKET_MODE_ZERO_COPY`;
- sets `service->transmitter` to its own transmitter, built on `send_zc`;
- arms a repeating timeout at the service congestion interval that calls
  `FlushKCPService(service, KCP_FLUSH_SEND | KCP_FLUSH_CLEANUP)`.

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
flush timeout. It does not touch the service.

## Choosing the Format Per Packet

With `validate == NULL`, every accepted datagram is parsed with the single `format`
given at construction.

Supplying `validate` instead lets the adapter demultiplex several envelopes on one
port: it is called with the peer address and the raw datagram, and returns the
`KCPFormat` to use, or `NULL` to drop the packet. This is also the natural place for
cheap source filtering before any conversation state is created.

## What the Adapter Adds Per Packet

- **RX timestamps.** `SCM_TIMESTAMPING` control data is converted from realtime to the
  monotonic clock (the adapter keeps a refreshed realtime/monotonic delta) and passed
  to `HandleKCPPacket()` as the packet time, so RTT estimation uses kernel arrival
  time rather than the moment the loop got round to it.
- **Destination address.** `IPV6_PKTINFO` is turned into a `struct KCPPoint`, so a
  service bound to a wildcard address knows which local address a conversation arrived
  on. V4-mapped addresses are unmapped to `AF_INET`.
- **Buffer lifetime.** Inbound `FastBuffer`s are passed to the engine with
  `HoldFastBuffer` / `ReleaseFastBuffer` as the acquire/release pair, so a segment that
  the engine keeps for reassembly pins only its own buffer.
- **Immediate flush.** After a packet updates a conversation, its outbound queue is
  flushed at once, so ACKs do not wait for the next timer tick.

## Rules

- One adapter per service. The adapter overwrites `service->transmitter`.
- The adapter must outlive the conversations of its service — release it before the
  service, never after.
- All handler callbacks run in the ring thread inside `WaitForFastRing()`.
- The socket is created dual-stack. To serve IPv4 only, filter in `validate` or bind
  the port externally.
