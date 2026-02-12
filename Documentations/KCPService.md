# KCPService API Reference

Header: `Supplimentary/KCPService.h`

`KCPService` is the main KCP protocol engine for conversation tracking, congestion control, and packet processing.

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
