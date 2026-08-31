# KCPService API Reference

Header: `Supplimentary/KCPService.h`

`KCPService` is the main KCP protocol engine for conversation tracking, congestion control, and packet processing.

It is transport-agnostic on its own: packets come in through `HandleKCPPacket()` and go
out through a `KCPTransmitter` the caller supplies. `KCPAdapter` is the ready-made
FastRing/FastSocket binding — see [KCPAdapter.md](KCPAdapter.md).

**External dependency:** this header includes `HashMap.h`, which is not part of the
FastRing repository. It lives in the consuming project's own common code, so
`KCPService` cannot be compiled from this repository alone.

## KCP Primer

KCP is a reliable transport protocol built on top of UDP, focused on reducing latency versus TCP in lossy/high-RTT networks.
It adds ARQ, retransmission, RTT/RTO estimation, fast resend, flow/congestion control, and message fragmentation/reassembly in user space.

Primary references:
- Official repository: https://github.com/skywind3000/kcp
- Protocol notes from upstream: https://github.com/skywind3000/kcp/blob/master/protocol.txt
- Reference implementation (`ikcp.c`): https://github.com/skywind3000/kcp/blob/master/ikcp.c

## Why A Custom KCP Layer Here

Inference from this project code (`Supplimentary/KCPService.h`, `Supplimentary/KCPAdapter.h`, `Supplimentary/KCPService.c`):
- You need tight integration with `FastRing`/`FastSocket` async pipeline, not a standalone socket loop.
- You expose pluggable wire format hooks (`verify/parse/propose/prepare/compose` in `KCPFormat`) to support protocol framing variants.
- You keep explicit control over memory ownership (`AcquireKCPClosure` / `ReleaseKCPClosure`) and batching/flush behavior.
- You maintain service-level conversation management and event callbacks tailored to application logic.
- Compared to the original library integration style, this implementation includes built-in connection tracking at service level.
- Compared to a naive per-packet/per-conversation scan approach, this design targets significantly better asymptotic complexity (hash-based conversation lookup and queue-based processing).

In practice this is exactly where a project-specific KCP adaptation makes sense: same KCP semantics, different runtime, packet envelope, and lifecycle model.

## Asymptotic and Performance Comparison

Below is a practical comparison between:
- upstream-style embedding where application keeps conversations and can do linear lookup paths;
- this project service-layer design (`KCPService` + hash map + ring-buffer queues).

| Operation | Naive/linear integration | This implementation |
|---|---:|---:|
| Conversation lookup by packet key | `O(C)` | average `O(1)` (hash map) |
| Conversation create/remove | `O(C)` with linear containers | average `O(1)` map insert/remove |
| Enqueue outbound segment | `O(1)` amortized (container-dependent) | `O(1)` amortized ring queue |
| Dequeue/ack progression | often `O(1)` but app-dependent | `O(1)` amortized queue ops |
| Full service flush tick | app-dependent | `O(C)` (iterate active conversations) |

Where:
- `C` = number of active conversations.

### Practical Impact

- At high `C`, replacing linear conversation lookup with hash lookup usually dominates latency and CPU gains.
- Queue-based per-conversation buffering gives stable `O(1)` amortized push/pop behavior under load.
- End-to-end throughput and tail latency generally improve as connection count grows because packet dispatch cost is flatter.

Important:
- Exact "X times faster" numbers depend on traffic mix, loss/RTT profile, allocator behavior, and NIC/kernel settings.
- To claim a concrete multiplier, run workload-specific benchmarks (same host/kernel, same packet profile, varying `C`).

## Wire Format

The KCP state machine is separated from the packet envelope. A `struct KCPFormat`
supplies five hooks, so the same engine drives different framings:

```c
typedef int (*VerifyKCPPacket)(uint8_t* packet, uint32_t size);
typedef uint32_t (*ParseKCPPacket)(struct KCPKey* key, struct KCPSegment* segment, uint8_t* packet, uint32_t size);
typedef uint32_t (*ProposeKCPPacket)(struct sockaddr* address, uint32_t length);
typedef void (*PrepareKCPPacket)(uint8_t* buffer, struct KCPKey* key, struct KCPSegment* segment, uint32_t length);
typedef void (*ComposeKCPPacket)(struct KCPSegment* segment);
```

- `verify` classifies a raw datagram as `KCP_PACKET_INVALID`,
  `KCP_PACKET_PROBABLY_VALID` or `KCP_PACKET_EXACTLY_VALID`, before any state is
  touched.
- `parse` extracts the conversation key and fills the segment from the wire.
- `propose` returns the packet size needed for a payload of `length` towards a given
  peer, so the envelope overhead stays in the format.
- `prepare` writes the envelope into an allocated buffer.
- `compose` finalises a segment before transmission (checksums, encryption).

`StandardKCPFormat` implements the upstream KCP header (`struct StandardKCPHeader`:
`conv`, `struct KCPControl`, `len`). A project with its own envelope defines its own
constant — BrandMeister's Hytera link, for example, ships a `HyteraKCPFormat` whose
header carries an extra 64-bit application id ahead of the KCP control block.

## Events

```c
typedef void (*HandleKCPEvent)(void* closure, struct KCPConversation* conversation, int event, uint8_t* data, size_t length);
```

| Event | Meaning |
| --- | --- |
| `KCP_EVENT_CREATE` | A conversation was created for a previously unknown key. Attach application state here |
| `KCP_EVENT_REMOVE` | The conversation is going away. Detach application state |
| `KCP_EVENT_RECEIVE` | `data` / `length` carry one reassembled message |
| `KCP_EVENT_RESUME` | The outbound queue drained completely; a sender that was throttling may continue |

The handler is installed through `struct KCPHandler` passed to `CreateKCPService()`.

## Conversation Ownership

`conversation->closure` is free for the application, and `conversation->count` is a
reference count. The established pattern is to claim a reference when the conversation
appears and drop it when the application object dies:

```c
void HandleEvent(void* closure, struct KCPConversation* conversation, int event, uint8_t* data, size_t length)
{
  switch (event)
  {
    case KCP_EVENT_CREATE:
      conversation->count ++;                     // hold it
      conversation->closure = CreateContext(...);
      break;

    case KCP_EVENT_REMOVE:
      conversation->closure = NULL;               // engine-side removal
      break;
  }
}

...
ReleaseKCPConversation(conversation);             // drop the held reference
```

`ReleaseKCPConversation()` called from inside a handler is deferred: the conversation
is marked `KCP_CONVERSATION_DEAD | KCP_CONVERSATION_SILENT` and reclaimed by the next
cleanup pass, so a handler may release the conversation it is currently handling.

## Sending

```c
SubmitKCPMessage(conversation, data, length);
SubmitKCPVectorList(conversation, list, count);   // scatter/gather variant
FlushKCPConversation(conversation, NULL);         // push what is now sendable
```

Submission only queues; `FlushKCPConversation()` (or the periodic
`FlushKCPService(service, KCP_FLUSH_SEND)`) is what puts segments on the wire. Passing
`NULL` as `time` makes the flush use the current clock. Both submission functions
respect the conversation state flags documented below.

## Core API

```c
uint32_t GetKCPQueueLength(struct KCPQueue* queue);

int HandleKCPPacket(
  struct KCPService* service,
  const struct KCPFormat* format,
  struct KCPConversation** reference,
  struct timespec* time,
  struct sockaddr* address,
  void* packet,
  uint32_t size,
  struct KCPPoint* point,
  AcquireKCPClosure acquire,
  ReleaseKCPClosure release,
  void* closure);

int SubmitKCPMessage(struct KCPConversation* conversation, const uint8_t* data, size_t length);
int SubmitKCPVectorList(struct KCPConversation* conversation, const struct iovec* list, size_t length);
int FlushKCPConversation(struct KCPConversation* conversation, struct timespec* time);

void ReleaseKCPConversation(struct KCPConversation* conversation);
int CreateKCPConversation(struct KCPService* service, const struct KCPKey* key, const struct KCPFormat* format, struct KCPConversation** reference);

int FlushKCPService(struct KCPService* service, uint32_t flags);
void ReleaseKCPService(struct KCPService* service);
struct KCPService* CreateKCPService(struct KCPHandler* handler, struct KCPTransmitter* transmitter);
```

Global format:

```c
extern const struct KCPFormat StandardKCPFormat;
```

## Conversation State

`conversation->state` is a bit mask that determines what the submission and flush
entry points do:

- `KCP_CONVERSATION_DETACHED` (`1 << 0`) - the conversation was detached from the
  service. `SubmitKCPMessage()`, `SubmitKCPVectorList()` and
  `FlushKCPConversation()` return `-EBADF`.
- `KCP_CONVERSATION_SILENT` (`1 << 1`) - suppresses the outbound notification on
  detach, used when the peer is already gone.
- `KCP_CONVERSATION_GUARD` (`1 << 2`) - a handler callback is on the stack.
  `FlushKCPConversation()` returns `-EBUSY` and a release requested from inside the
  callback is deferred instead of freeing the object.
- `KCP_CONVERSATION_DEAD` (`1 << 3`) - the conversation exceeded
  `KCP_DEFAULT_TRIES` retransmissions or its timeout. Submissions return
  `-ECONNRESET`; the object is reclaimed by the next `KCP_FLUSH_CLEANUP` pass.

## Service Flush Flags

The `flags` argument of `FlushKCPService()`:

- `KCP_FLUSH_CLEANUP` (`1 << 0`) - reap conversations marked `KCP_CONVERSATION_DEAD`
- `KCP_FLUSH_SEND` (`1 << 1`) - run the retransmission and send pass over every
  conversation

The two are independent and are normally combined.

## Segment State

`segment->state` is internal bookkeeping, exposed through `struct KCPSegment`:

- `KCP_SEGMENT_NUMBERED` (`1 << 0`) - a sequence number has been assigned; it is
  assigned lazily at first send, not at queue time
- `KCP_SEGMENT_ANCHOR` (`1 << 1`) - the segment starts a message boundary in the
  stream
- `KCP_SEGMENT_SENT` (`1 << 2`) - the segment is in flight; cleared when a
  retransmission is scheduled
