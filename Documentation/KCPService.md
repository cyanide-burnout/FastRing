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
  touched. Only a negative result rejects the datagram (`-EPROTO`); the two valid
  classes are treated alike.
- `parse` extracts the conversation key and fills the segment from the wire. It returns
  the number of bytes consumed; `0` ends the processing of the datagram, and more than
  the remaining size is `-EPROTO`. A datagram may carry several segments.
- `propose` returns the packet size needed for a payload of `length` towards a given
  peer, so the envelope overhead stays in the format.
- `prepare` writes the envelope into an allocated buffer.
- `compose` finalises a segment before transmission (checksums, encryption). It runs
  before every transmission of a data segment, retransmissions of the same buffer
  included, so it must be idempotent: encryption in place would be applied twice.

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
| `KCP_EVENT_CREATE` | `HandleKCPPacket()` created a conversation for a previously unknown key. Attach application state here. `CreateKCPConversation()` delivers no such event |
| `KCP_EVENT_REMOVE` | The service removes the conversation: a cleanup pass reaps a dead one, or the service is released. Not delivered for a conversation the application released. Detach application state |
| `KCP_EVENT_RECEIVE` | `data` / `length` carry one reassembled message, valid during the callback only |
| `KCP_EVENT_RESUME` | An acknowledgement arrived while the outbound queue is (or became) empty; a sender that was throttling may continue. Duplicate acknowledgements repeat it |

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

The service's map holds one reference of its own, and a release that leaves the count at
one or less closes the conversation. Outside a handler it is then removed from the map and
freed before the call returns, without `KCP_EVENT_REMOVE`. From inside its own handler the
release is deferred: the conversation is marked `KCP_CONVERSATION_DEAD |
KCP_CONVERSATION_SILENT` and reclaimed by the next cleanup pass (from `KCP_EVENT_REMOVE`, as
soon as that handler returns), so a handler may release the conversation it is currently
handling.

## Sending

```c
SubmitKCPMessage(conversation, data, length);
SubmitKCPVectorList(conversation, list, count);   // scatter/gather variant
FlushKCPConversation(conversation, NULL);         // push what is now sendable
```

Submission only queues; `FlushKCPConversation()` (or the periodic
`FlushKCPService(service, KCP_FLUSH_SEND)`) is what puts segments on the wire. Passing
`NULL` as `time` makes the flush use the current clock. Both submission functions
respect the conversation state flags documented below, and may be called from handlers;
flushing may not. A message of more than 255 segments (255 × `mss` bytes) is refused with
`-EMSGSIZE`, as is an empty vector list; `SubmitKCPMessage()` with length `0` queues one
empty segment.

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
- `KCP_CONVERSATION_SILENT` (`1 << 1`) - no `KCP_EVENT_REMOVE` will be delivered; set
  when the application releases the conversation.
- `KCP_CONVERSATION_GUARD` (`1 << 2`) - a handler callback is on the stack.
  `FlushKCPConversation()` returns `-EBUSY` and a release requested from inside the
  callback is deferred instead of freeing the object.
- `KCP_CONVERSATION_DEAD` (`1 << 3`) - the conversation exceeded its transmission limit
  (`conversation->tries`, `KCP_DEFAULT_TRIES` by default) or its inactivity timeout
  (`conversation->timeout` since the last received packet), detected during a flush and
  recorded in `conversation->cause`, or it was released from inside its own handler.
  Submissions return `-ECONNRESET`; the object is reclaimed by the next
  `KCP_FLUSH_CLEANUP` pass.

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
- `KCP_SEGMENT_ANCHOR` (`1 << 1`) - the segment ends a message that was already
  delivered; it stays at the head of the inbound queue as the sequence reference, with its
  buffer, until the next message is delivered
- `KCP_SEGMENT_SENT` (`1 << 2`) - the segment is in flight; cleared when a
  retransmission is scheduled

## Lifecycle

`KCPService` creates nothing on the ring. It is a single-threaded engine: packets come in
through `HandleKCPPacket()`, segments go out through the transmitter, and time advances only
when the caller flushes; `KCPAdapter` supplies all three on a ring. The transmitter's
`allocate()` returns buffers that the service owns until it gives them back through
`release()`; `transmit()` does not take ownership, so an asynchronous transmitter takes its
own reference.

A conversation is freed when its count reaches zero, which releases every queued segment and
hands the buffers back. `ReleaseKCPService()` delivers `KCP_EVENT_REMOVE` to every
conversation not marked silent and drops the map's references; a reference the application
still holds afterwards points into freed memory, so drop it in `KCP_EVENT_REMOVE` at the
latest. `ReleaseKCPService()` does not transmit and does not touch the transmitter. Nothing is
locked: use the service and its conversations from one thread.
