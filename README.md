# FastRing

FastRing is a C library for building single-threaded event loops on Linux `io_uring`. One thread
owns one ring; everything that thread does, its own sockets, files and timers, and the event
loops of libraries that were not written for `io_uring` (curl, c-ares, Avahi, D-Bus, GLib, libuv,
Lua), completes through that ring and is handled by callbacks on that thread. Around the core
sit buffer pools, socket and TLS transports, cross-thread calls, and adapters for protocol
libraries.

It was introduced in 2023 as part of the BrandMeister and TetraPack projects and is used by both;
modules that those projects use are kept in sync with their copies, so this repository carries
the code that runs in production. Some modules exist only here, as experimental work or as
groundwork for what those projects, or others, are expected to need; experimental and deprecated
modules are marked as such in [Modules](#modules).

- [The Problem](#the-problem)
- [Architecture](#architecture)
- [Getting Started](#getting-started)
- [Execution Contract](#execution-contract)
- [Requirements](#requirements)
- [Limits](#limits)
- [Modules](#modules)
- [Examples](#examples)
- [Repository Layout](#repository-layout)
- [Documentation](#documentation)

## The Problem

A network service such as a radio network core handles thousands of sockets, timers and
protocol sessions, and talks to external services through libraries: HTTP and WebSocket through
curl, DNS through c-ares, service discovery through Avahi, system control through D-Bus,
scripting through Lua. Each library comes with its own idea of an event loop. Running them in
threads of their own means locks around all shared state; running them on `epoll` costs a system
call per readiness change and per operation.

`io_uring` removes the per-operation system call, but it is a queue pair, not an event loop. It
says nothing about who owns a request while the kernel holds it, how a request is cancelled when
its owner goes away, how a callback is found for a completion, or how a library that only knows
"tell me when this descriptor is readable" is attached to it.

FastRing supplies that missing layer:

- Every operation is a **descriptor**: a small reference-counted object that carries a copy of
  its SQE, a completion handler and storage for the data the kernel reads or writes. A completion
  finds its handler through the descriptor, and a late completion for a descriptor already reused
  is recognised and dropped.
- The **loop** is one function, `WaitForFastRing()`, that submits what was queued, waits, calls
  the handlers, and then runs deferred work.
- **Poll, Watch, Timeout and Event** are built on descriptors and can be updated or removed at
  any time, including from inside their own callbacks.
- **Adapters** implement the poll and timer hooks of foreign libraries with these primitives, so
  the libraries run on the same thread and need no locks.
- Descriptors can be queued from any thread, and **ThreadCall** runs a function on the ring
  thread synchronously, so worker threads and foreign thread pools can reach ring-owned state
  without locking it.

FastRing is not a thin wrapper around liburing: the descriptor model, its ownership rules and the
loop are its own. It is also not a scheduler: there are no tasks, priorities or thread pools, and
a callback that blocks stalls its ring.

## Architecture

### The Ring and Its Thread

A ring (`struct FastRing`) is one `io_uring` instance created by `CreateFastRing()`. The thread
that creates it is the **ring thread**: the ring is set up with `IORING_SETUP_SINGLE_ISSUER`, so
only that thread may submit to it, and every callback of the ring runs on it. A program with
several loops has several rings, one per thread; rings exchange messages with
`IORING_OP_MSG_RING` (FastRing Event).

### Descriptors

A descriptor (`struct FastRingDescriptor`) is a 512-byte block allocated from the ring and
recycled through its free list:

```text
AllocateFastRingDescriptor(ring, handler, closure)      references = 1, SQE = NOP
  io_uring_prep_*(&descriptor->submission, ...)          fill the private SQE copy
SubmitFastRingDescriptor(descriptor, 0)                  queued, from any thread
  WaitForFastRing() copies the SQE into the ring          the kernel owns the request
  handler(descriptor, completion, RING_REASON_COMPLETE)
    return 1   -> still armed (re-submitted, or multishot with IORING_CQE_F_MORE)
    return 0   -> one reference dropped; at 0 the descriptor is recycled
```

The reference count is what tells the ring when a descriptor is free: an in-flight operation, a
handle held by an API, a cancellation request each hold one. The rules, the states a descriptor
goes through and the ways to cancel it are in [LIFECYCLE.md](Documentation/LIFECYCLE.md).

### One Iteration of the Loop

```text
WaitForFastRing(ring, interval, mask)
  move queued descriptors into the submission queue
  submit, and wait up to `interval` ms unless something is ready
  call the handler of every completion
  run the flushers armed during this iteration
```

A **flusher** is a one-shot callback for the end of the iteration. Modules use it to batch: all
sends queued by the completions of one iteration go out as one linked chain, all buffered frames
of a library are processed once.

### Layers

```text
 application
 ──────────────────────────────────────────────────────────────────────────────────────────
 protocol modules      Fetch ─ CURLWSCore ─ gRPCClient     H2OCore ─ gRPCServer     XMPPServer
                       KCPService ─ KCPAdapter     SambarAdapter     Latch
 loop adapters         FastAvahiPoll   DBusCore   Resolver (c-ares)   LuaPoll   FastGLoop   FastUVLoop
 transports            FastSocket   FastBIO ─ SSLSocket   FastBuffer
 threads, system       ThreadCall   FastSemaphore   CoRing (C++ coroutines)   WatchDog (systemd)
 ──────────────────────────────────────────────────────────────────────────────────────────
 core (FastRing)       descriptors   Poll   Watch   Timeout   Event   flushers   buffer provider
 ──────────────────────────────────────────────────────────────────────────────────────────
 liburing / io_uring
```

The loop adapters differ in how the foreign library describes its waits:

| Library interface | Adapter mechanism |
| --- | --- |
| "watch this descriptor, call me when ready" (Avahi, D-Bus, c-ares) | Watch or Poll per descriptor, Timeout per timer |
| "socket and timer callbacks" (curl multi) | A poll request per socket, a Timeout for the multi timer, a flusher to drive the transfers |
| "here is my whole poll set, run one iteration" (GLib) | The GLib loop runs on a fiber of its own inside a flusher, its `poll()` answered by the ring |
| "here is my backend descriptor" (libuv) | A poll request on the backend descriptor and a Timeout for the next timer |

`LuaPoll` goes the other way: it gives Lua coroutines ring operations to wait on.

## Getting Started

The library is a set of C sources without a build of its own: an application compiles the
modules it needs, as the examples do (see [Examples](#examples)). The core is `Ring/FastRing.c`
plus liburing.

### 1. The Loop

```c
#include <signal.h>
#include <stdatomic.h>

#include "FastRing.h"

static atomic_int running = 1;

static void HandleSignal(int signal)
{
  atomic_store(&running, 0);
}

int main()
{
  struct FastRing* ring = CreateFastRing(0);        /* this thread is now the ring thread */

  signal(SIGINT,  HandleSignal);
  signal(SIGTERM, HandleSignal);

  /* create objects that use the ring here */

  while (atomic_load(&running) && (WaitForFastRing(ring, 200, NULL) >= 0));

  /* release them here, before the ring */

  ReleaseFastRing(ring);
  return 0;
}
```

`200` is the longest the loop waits for a completion. A signal interrupts the wait with `-EINTR`,
which ends this loop as well; a loop that must survive handled signals checks for `-EINTR`
explicitly.

### 2. An Operation of Your Own

Any `io_uring` operation runs through a descriptor. This one echoes standard input, keeping the
read buffer inside the descriptor so that it lives exactly as long as the request:

```c
static int HandleRead(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if ((completion == NULL) ||                      /* RING_REASON_RELEASED: the ring is going away */
      (completion->res <= 0))                      /* end of file or error */
    return 0;                                      /* drop the reference, the descriptor is freed */

  fwrite(descriptor->data.data, 1, completion->res, stdout);

  io_uring_initialize_sqe(&descriptor->submission);
  io_uring_prep_read(&descriptor->submission, STDIN_FILENO, descriptor->data.data, sizeof(descriptor->data.data), -1);
  SubmitFastRingDescriptor(descriptor, 0);
  return 1;                                        /* re-armed: keep the reference */
}

struct FastRingDescriptor* descriptor = AllocateFastRingDescriptor(ring, HandleRead, NULL);

io_uring_prep_read(&descriptor->submission, STDIN_FILENO, descriptor->data.data, sizeof(descriptor->data.data), -1);
SubmitFastRingDescriptor(descriptor, 0);
```

The handler's return value is the whole ownership protocol of a raw descriptor: non-zero while
the descriptor is armed, `0` when it is done. After it returns `0` the pointer belongs to the
ring. A descriptor whose owner goes away while the operation is in flight is abandoned with
`DiscardFastRingDescriptor()` from the ring thread, which cancels the operation and detaches the
handler; see [Cancellation](Documentation/LIFECYCLE.md#cancellation).

### 3. Timers

```c
static void HandleTick(struct FastRingDescriptor* descriptor)
{
  /* every second until removed */
}

static void HandleDeadline(struct FastRingDescriptor* descriptor)
{
  struct Session* session = (struct Session*)descriptor->closure;

  session->deadline = NULL;                        /* a fired one-shot timeout frees itself */
  CloseSession(session);
}

struct FastRingDescriptor* tick = SetFastRingTimeout(ring, NULL, 1000, TIMEOUT_FLAG_REPEAT, HandleTick, NULL);

session->deadline = SetFastRingTimeout(ring, session->deadline, 30000, 0, HandleDeadline, session);   /* arm or restart */
session->deadline = SetFastRingTimeout(ring, session->deadline, -1, 0, NULL, NULL);                  /* cancel: NULL */

tick = SetFastRingTimeout(ring, tick, -1, 0, NULL, NULL);
```

A one-shot timeout that has fired is released after its callback, so the stored pointer is
cleared in the callback; `TIMEOUT_FLAG_OWNED` keeps the descriptor alive until it is removed
instead. Repeating timeouts depend on the liburing the code is compiled with; see
[Repeating timeouts](Documentation/LIFECYCLE.md#repeating-timeouts).

### 4. Readiness of a Descriptor Owned by Someone Else

When a library does its own I/O and only needs to know when to do it, watch the descriptor:

```c
static void HandleReadable(struct FastRingDescriptor* descriptor, int result)
{
  struct Connection* connection = (struct Connection*)descriptor->closure;

  if (result < 0)
    return;                                        /* error of the poll request */

  ProcessInput(connection);                        /* read() until EAGAIN */
}

connection->watch = AddFastRingWatch(ring, connection->handle, POLLIN, 0, HandleReadable, connection);

/* ... */

RemoveFastRingWatch(connection->watch);            /* no callback runs after this returns */
close(connection->handle);
free(connection);
```

The watch is re-armed after every callback until it is removed, and it may be removed or updated
from inside the callback. A removal made inside the callback means the callback is still running:
free the closure after it returns.

### 5. Calls From Other Threads

A worker or a foreign thread pool reaches ring-owned state through `ThreadCall`:

```c
static void HandleCall(void* closure, va_list arguments)
{
  struct Registry* registry = (struct Registry*)closure;
  const char* name          = va_arg(arguments, const char*);
  int* result               = va_arg(arguments, int*);

  *result = AddRegistryName(registry, name);       /* runs on the ring thread */
}

struct ThreadCall* call = CreateThreadCall(ring, HandleCall, registry);   /* ring thread */
HoldThreadCall(call);                                                    /* one reference per caller */

/* any other thread */
int result;
if (MakeThreadCall(call, "name", &result) == TC_RESULT_CALLED)
  ...

ReleaseThreadCall(call, TC_ROLE_CALLER);           /* when the caller is done */
ReleaseThreadCall(call, TC_ROLE_HANDLER);          /* ring thread, before the registry goes away */
```

The caller blocks until the handler has run on the ring thread, so it can pass pointers to its
own stack. After the handler role is released every call returns `TC_RESULT_CANCELED`. Work
queued from another thread with `SubmitFastRingDescriptor()` does not wake the ring; see
[Concurrency](Documentation/LIFECYCLE.md#concurrency).

### 6. A Foreign Event Loop

An adapter hands the library hooks that create watches and timeouts on the ring. With Avahi:

```c
AvahiPoll* poll     = CreateFastAvahiPoll(ring);
AvahiClient* client = avahi_client_new(poll, AVAHI_CLIENT_NO_FAIL, HandleClientEvent, NULL, &error);

/* the loop runs; Avahi callbacks arrive on the ring thread */

avahi_client_free(client);                         /* frees its watches and timeouts */
ReleaseFastAvahiPoll(poll);
```

Every adapter follows the same order: the library objects are released first, while the ring and
the adapter still exist, then the adapter, then the ring.

### 7. Shutting Down

1. Leave the loop.
2. Release everything that uses the ring, in reverse order of creation: application objects,
   protocol and loop adapters, `ThreadCall` handler roles, `WatchDog`.
3. `ReleaseFastRing()`. Whatever is still alive gets its handler called with
   `RING_REASON_RELEASED`, which is a safety net, not a substitute for step 2.
4. Free memory that cancelled operations could still have used.

## Execution Contract

**One ring, one thread.** The ring is used by the thread that created it: `WaitForFastRing()`,
`ReleaseFastRing()`, flushers and registration of files and buffers belong to that thread, and
the kernel refuses submissions and registrations from other threads with `-EEXIST`. Callbacks
run on it. A ring cannot be handed to another thread; a worker creates its own.

**Callbacks do not block.** Everything the ring does waits while a callback runs: other
completions, flushers, the next submission. Long work goes to another thread and comes back
through `ThreadCall` or an event. `WaitForFastRing()` is never called from a callback.

**The handler's return value is ownership.** A completion handler returns non-zero while its
descriptor is still armed and `0` when it is done; `0` drops the reference of the operation, and
the descriptor may be reused before the handler's caller has returned. A pointer to a raw
descriptor is valid only while somebody holds a reference.

**Removal is asynchronous, callbacks stop at once.** Removing a poll, a watch or a timeout
returns before the kernel has let go of the request, but no callback is delivered after the
removal has returned, and none is running then, unless the removal was made from inside that very
callback.

**Other threads queue, they do not wake.** Descriptors may be allocated and submitted from any
thread; the ring picks them up on its next iteration. Something has to wake it: `ThreadCall`, an
`eventfd` read armed on the ring, or an event from another ring.

**Release before the ring.** Objects that use a ring are released before `ReleaseFastRing()`.
Handlers called with `RING_REASON_RELEASED` release their own resources only; they do not submit,
arm flushers or rely on other objects still being alive.

The detailed rules, with the reasons behind them, are in
[LIFECYCLE.md](Documentation/LIFECYCLE.md); the mistakes that were made with them are collected
in its [Pitfalls](Documentation/LIFECYCLE.md#pitfalls).

## Requirements

FastRing runs on Linux only. Every `io_uring` feature it uses has its own minimum kernel; the
versions below come from the liburing manual pages and were checked against the kernel sources
where noted.

| Feature | Linux | liburing | Used by | Without it |
| --- | --- | --- | --- | --- |
| `IORING_SETUP_SINGLE_ISSUER` | 6.0 | 2.3 | `CreateFastRing()` | `CreateFastRing()` returns `NULL` |
| `IORING_SETUP_SUBMIT_ALL`, `COOP_TASKRUN` | 5.18, 5.19 | | `CreateFastRing()` | Below the 6.0 floor |
| Sparse file table, file alloc range | 5.19, 6.0 | | Registered files | Below the 6.0 floor |
| Provided buffer rings | 5.19 | | Buffer provider, `FastSocket` receive | Below the 6.0 floor |
| `IORING_OP_MSG_RING` | 5.18 | | FastRing Event | Below the 6.0 floor |
| Multishot `recv`, `recvmsg` | 6.0 | | `FastSocket`, `FastBIO` | Below the 6.0 floor |
| `IORING_OP_SEND_ZC`, `SENDMSG_ZC` | 6.0, 6.1 | | `FastSocket` zero-copy mode; `FastBIO` sends without kTLS | `FastSocket`: the mode must not be selected |
| Multishot timeouts | 6.4 | 2.4 | `TIMEOUT_FLAG_REPEAT` when built with liburing 2.4 or newer | Repeating timeouts fail, see [LIFECYCLE.md](Documentation/LIFECYCLE.md#repeating-timeouts) |
| `IORING_OP_FUTEX_WAIT`, `FUTEX_WAKE` | 6.7 | 2.6 | `ThreadCall`, `FastSemaphore`, `LatchServer` | `ThreadCall` uses an `eventfd`; `FastSemaphore` does not build |
| `IORING_OP_READ_MULTISHOT` | 6.7 | 2.6 | `FastSocket` file mode (`FASTSOCKET_MODE_FILE_IO`) | The mode is not compiled |
| `SOCKET_URING_OP_SETSOCKOPT` | 6.7 | 2.5 | `FastBIO` kTLS TX offload | `FastBIO` does not build with older liburing; on older kernels the TX setup fails with `POLLERR` |
| `IORING_ENTER_NO_IOWAIT` | 6.15 | 2.10 | `CreateFastRing()` | Waiting counts as iowait |

The floor of the core is therefore Linux 6.0 and liburing 2.3, the first release with
`IORING_SETUP_SINGLE_ISSUER`, `io_uring_get_events()` and `io_uring_register_file_alloc_range()`.
`IORING_SETUP_SINGLE_ISSUER` was checked in `io_uring/io_uring.c` of v6.0, and the refusal of
registrations from other threads, which the thread rules rely on, in v6.1. Since liburing 2.6 the SQE of a reused descriptor must be cleared
with `io_uring_initialize_sqe()`; `FastRing.h` makes the call a no-op for older versions.

### Tested Platforms

There is no formally maintained kernel compatibility matrix. The reference targets are the stable
Debian kernels:

- Debian 12 (bookworm: Linux 6.1, liburing 2.3) is the origin of the project and the oldest
  supported baseline. Its liburing has no futex operations, so `ThreadCall` uses an `eventfd`
  and `FastSemaphore` does not build.
- Debian 13 (trixie: Linux 6.12, liburing 2.9) is the current target; recent changes are written
  against its kernel series.

Older kernels may work but are not exercised.

### Libraries

| Module | Requires |
| --- | --- |
| Core, `FastBuffer`, `FastSocket`, `ThreadCall`, `WatchDog` | liburing, pthread; `WatchDog` libsystemd |
| `FastSemaphore` | glibc 2.34 or newer (it relies on the layout of `sem_t`), liburing 2.6 |
| `FastBIO`, `SSLSocket` | OpenSSL |
| `Fetch`, `CURLWSCore`, `gRPCClient` | libcurl 8.4 or newer; `gRPCClient` protobuf-c, zlib |
| `LWSCore` | libwebsockets |
| `Resolver` | c-ares |
| `FastAvahiPoll` | avahi-client |
| `DBusCore` | libdbus-1 |
| `FastGLoop`, `FastUVLoop` | GLib 2, libuv |
| `LuaPoll` | Lua 5.1 or LuaJIT, and `LuaTrace.h`, which is not part of this repository |
| `SambarAdapter` | libsmb2 |
| `H2OCore`, `gRPCServer`, `PicoBundle` | H2O with `Supplimentary/h2o.patch`, libuv, OpenSSL, picotls; `gRPCServer` protobuf-c, zlib |
| `XMPPServer` | libxml2 |
| `KCPService` | `HashMap.h`, which is not part of this repository |
| `CoRing` | C++20 coroutines and `Compromise.h`, which is not part of this repository |

## Limits

| Limit | Value |
| --- | --- |
| Submission queue | 16384 entries (`length` of `CreateFastRing()`, `0` = `RLIMIT_NOFILE`) |
| Completion queue | 4 × the submission queue |
| Registered files | `RLIMIT_NOFILE / 2`, at most `1 << 20`; the lower half for `AddFastRingRegisteredFile()` |
| Registered buffers | 16384 |
| Buffers per provider | 32768 |
| Descriptor | About 512 bytes, 256 of them owner storage (`data`) |
| Pending submissions | Unbounded; what does not fit into the submission queue waits for the next iteration |

Descriptors and flushers are allocated on demand and kept for reuse; their memory is returned
only by `ReleaseFastRing()`, so the footprint of a ring is its peak.

## Modules

Each module has a document with its API and its lifecycle.

### Core (`Ring/`)

| Module | Purpose |
| --- | --- |
| [`FastRing`](Documentation/API.md) | The engine: descriptors, loop, Poll, Watch, Timeout, Event, flushers, buffer provider, registered resources |
| [`FastBuffer`](Documentation/FastBuffer.md) | Reference-counted buffer pool with optional fixed-buffer registration |
| [`FastSocket`](Documentation/FastSocket.md) | Asynchronous socket and file I/O on FastRing and FastBuffer |
| [`FastBIO`](Documentation/FastBIO.md) | OpenSSL `BIO` whose transport is `io_uring`, with kTLS offload |
| [`SSLSocket`](Documentation/SSLSocket.md) | TLS session wrapper on OpenSSL |
| [`ThreadCall`](Documentation/ThreadCall.md) | Synchronous calls from other threads into the ring thread |
| [`FastSemaphore`](Documentation/FastSemaphore.md) | Reactive POSIX `sem_t`: tokens are delivered to a handler |
| [`CoRing`](Documentation/CoRing.md) | C++20 coroutine adapter |
| [`WatchDog`](Documentation/WatchDog.md) | systemd watchdog fed from a ring timeout |
| [`Fetch`](Documentation/Fetch.md) | libcurl multi interface on the ring, and a host for protocols built on it |
| [`CURLWSCore`](Documentation/CURLWSCore.md) | WebSocket client on top of `Fetch` (recommended) |
| [`LWSCore`](Documentation/LWSCore.md) | libwebsockets adapter; deprecated, kept for compatibility |
| [`Resolver`](Documentation/Resolver.md) | c-ares DNS channel driven by the ring |
| [`FastAvahiPoll`](Documentation/FastAvahiPoll.md) | Avahi poll adapter |
| [`DBusCore`](Documentation/DBusCore.md) | libdbus connection driven by the ring |
| [`LuaPoll`](Documentation/LuaPoll.md) | Lua and LuaJIT bindings |
| [`FastGLoop`](Documentation/FastGLoop.md) | GLib main loop on the ring |
| [`FastUVLoop`](Documentation/FastUVLoop.md) | libuv loop on the ring |
| [`TurboRing`](Documentation/TurboRing.md) | Turbo Vision application on the ring |
| [`SambarAdapter`](Documentation/SambarAdapter.md) | libsmb2 (SMB2) adapter; experimental |

### Supplementary (`Supplimentary/`)

| Module | Purpose |
| --- | --- |
| [`Latch`](Documentation/Latch.md) | Shared-memory latch for cross-process write exclusion |
| [`H2OCore`](Documentation/H2OCore.md) | H2O HTTP/2 and HTTP/3 server runtime |
| [`PicoBundle`](Documentation/PicoBundle.md) | picotls certificate context derived from an `SSL_CTX` |
| [`gRPC`](Documentation/gRPC.md) | Wire format constants and shared macros |
| [`gRPCClient`](Documentation/gRPCClient.md) | gRPC client over `Fetch` |
| [`gRPCServer`](Documentation/gRPCServer.md) | gRPC request dispatch on `H2OCore` |
| [`ProtoBuf`](Documentation/ProtoBuf.md) | Bump allocator for `protobuf-c` unpacking |
| [`KCPService`](Documentation/KCPService.md) | KCP protocol engine |
| [`KCPAdapter`](Documentation/KCPAdapter.md) | UDP transport for `KCPService` |
| [`XMPPServer`](Documentation/XMPPServer.md) | XMPP server on FastRing and FastSocket |

## Examples

The examples are didactic: they show the wiring and leave out most error handling. Each has its
own `Makefile`, resolves its dependencies through `pkg-config`, and compiles the modules it needs
directly from `Ring/` and `Supplimentary/`:

```bash
make -C Examples/Fetch
```

| Example | Shows |
| --- | --- |
| `Examples/Avahi` | `FastAvahiPoll`: browsing and publishing services |
| `Examples/Fetch` | `Fetch`: every way an HTTP transfer can end |
| `Examples/CURLWS` | `Fetch` and `CURLWSCore`: a WebSocket client |
| `Examples/SSL` | `FastBIO`, `SSLSocket` and `Resolver`: a TLS client |
| `Examples/AAA` | `FastSocket` in a RADIUS client, an extract from production code |
| `Examples/gRPCClient`, `Examples/gRPCServer` | `gRPCClient` and `gRPCServer` |
| `Examples/H2H3Server` | `H2OCore`, `FastUVLoop` and `PicoBundle`: HTTP/2 and HTTP/3 |
| `Examples/SMBClient`, `Examples/SMBServer` | `SambarAdapter` |
| `Examples/TurboVision` | `TurboRing`: a Turbo Vision application fed by a ring timeout |

[Examples/README.md](Examples/README.md) lists what each one needs and how to run it.

## Repository Layout

| Path | Content |
| --- | --- |
| `Ring/` | The core and the modules |
| `Supplimentary/` | Protocol services and their helpers, `h2o.patch` |
| `Examples/` | Examples, see above |
| `Documentation/` | Reference and design documents |

## Documentation

- [LIFECYCLE.md](Documentation/LIFECYCLE.md): the ring, descriptors, references, completion,
  cancellation, Poll, Watch, Timeout, Event, flushers, threads, shutdown and pitfalls.
- [API.md](Documentation/API.md): the functions and constants of the core.
- [Documentation/README.md](Documentation/README.md): the index of the module documents.

## License

MIT. See [LICENSE](LICENSE).
