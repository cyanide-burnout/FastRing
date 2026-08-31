# FastRing

`FastRing` is a set of high-performance asynchronous I/O modules built around `io_uring` (Linux).
It is designed for reactive networking workloads and low-overhead event handling.

At the centre is the event and descriptor engine (`Ring/FastRing.*`). Around it sit
networking, TLS and protocol adapters, supplementary services, and runnable examples.

## What It Solves

FastRing provides:
- `SQE/CQE` multiplexing
- submission/completion queue flow control
- descriptor lifecycle tracking
- `poll/watch/timeout/event` primitives with an event-loop style API

## Requirements

- Linux with `io_uring` support
- `liburing` (some modules require >= 2.6)
- `pthread`
- optional dependencies by module:
  - `openssl`
  - `libcurl`
  - `libwebsockets`
  - `glib-2.0` / `libuv`
  - `avahi-client`
  - `dbus-1`
  - `c-ares`
  - `protobuf-c`
  - `systemd` (watchdog)
  - `libsmb2` (SMB2 adapter)

### Tested Platforms

There is no formally maintained kernel compatibility matrix. The reference targets
are the stable Debian kernels:

- Debian 12 (bookworm) is the origin of the project and the oldest supported baseline.
- Debian 13 (trixie) is the current target; recent changes are written against the
  Debian 13 kernel series.

Older kernels may work but are not exercised. Individual features degrade
independently: kTLS, multishot receive, `msg_ring`, futex operations and zerocopy
sends each have their own kernel requirements and fall back when unavailable.

## Repository Layout

- [`Ring/`](Ring) - core library and adapters
- [`Supplimentary/`](Supplimentary) - extra modules (gRPC/H2O/KCP/XMPP, etc.)
- [`Examples/`](Examples) - example applications with per-folder Makefiles
- [`Documentations/`](Documentations) - API and integration notes

## Quick Start (FastRing Core)

Typical lifecycle:
1. Create a ring: `CreateFastRing()`
2. Register operations (`poll/watch/timeout`) or submit custom SQEs via descriptor API
3. Drive the loop: `WaitForFastRing()`
4. Release resources: `ReleaseFastRing()`

Key APIs (`Ring/FastRing.h`):
- lifecycle: `CreateFastRing`, `ReleaseFastRing`, `WaitForFastRing`
- descriptors: `AllocateFastRingDescriptor`, `PrepareFastRingDescriptor`, `SubmitFastRingDescriptor`, `ReleaseFastRingDescriptor`
- poll: `AddFastRingPoll`, `UpdateFastRingPoll`, `RemoveFastRingPoll`, `SetFastRingPoll`
- watch: `AddFastRingWatch`, `UpdateFastRingWatch`, `RemoveFastRingWatch`, `SetFastRingWatch`
- timeout: `SetFastRingTimeout`, `SetFastRingCertainTimeout`, `SetFastRingPreciseTimeout`
- event: `CreateFastRingEvent`, `SubmitFastRingEvent`
- registered resources: `AddFastRingRegisteredFile`, `RemoveFastRingRegisteredFile`, `AddFastRingRegisteredBuffer`, `UpdateFastRingRegisteredBuffer`

## Building Examples

There is no single top-level build target in this repo.
Build examples from their own directories under `Examples/*`:

```bash
cd Examples/CURLWS
make
./curlwstest
```

Dependencies for each example are defined in its local `Makefile` via `pkg-config`.
[Examples/README.md](Examples/README.md) lists every example, what it demonstrates and
what it needs.

## Module Overview

### Core (`Ring/`)

- [`FastRing`](Documentations/FastRing.md) - core `io_uring` engine: submit/complete, poll/watch/timeout, descriptor lifecycle
- [`FastBuffer`](Documentations/FastBuffer.md) - buffer pool and buffer registration helpers
- [`FastSocket`](Documentations/FastSocket.md) - asynchronous socket I/O on top of FastRing
- [`FastBIO`](Documentations/FastBIO.md) - async OpenSSL BIO transport adapter
- [`SSLSocket`](Documentations/SSLSocket.md) - TLS socket layer built on OpenSSL
- [`ThreadCall`](Documentations/ThreadCall.md) - cross-thread calls into the ring handler thread
- [`FastSemaphore`](Documentations/FastSemaphore.md) - reactive `sem_t` integration (glibc internals + io_uring futex ops)
- [`FastGLoop`](Documentations/FastGLoop.md) - `GLib` loop integration
- [`FastUVLoop`](Documentations/FastUVLoop.md) - `libuv` loop integration
- [`Fetch`](Documentations/Fetch.md) - asynchronous wrapper over `libcurl` multi interface
- [`CURLWSCore`](Documentations/CURLWSCore.md) - recommended WebSocket client adapter
- [`LWSCore`](Documentations/LWSCore.md) - deprecated WebSocket adapter (kept for compatibility)
- [`FastAvahiPoll`](Documentations/FastAvahiPoll.md) - Avahi poll adapter for FastRing
- [`DBusCore`](Documentations/DBusCore.md) - D-Bus integration
- [`Resolver`](Documentations/Resolver.md) - c-ares DNS resolver integration
- [`LuaPoll`](Documentations/LuaPoll.md) - Lua/LuaJIT bindings
- [`WatchDog`](Documentations/WatchDog.md) - systemd watchdog helper
- [`SambarAdapter`](Documentations/SambarAdapter.md) - libsmb2 (SMB2) adapter, experimental
- [`CoRing`](Documentations/CoRing.md) - C++ coroutine adapter

### Supplementary (`Supplimentary/`)

- [`Latch`](Documentations/Latch.md) - shared-memory latch service for cross-process write exclusion
- [`H2OCore`](Documentations/H2OCore.md) - H2O HTTP/2/HTTP/3 integration layer
- [`PicoBundle`](Documentations/PicoBundle.md) - picotls/certificate bundle helper
- [`ProtoBuf`](Documentations/ProtoBuf.md) - protobuf-c support helpers
- [`gRPC`](Documentations/gRPC.md) - shared gRPC-related types/utilities
- [`gRPCClient`](Documentations/gRPCClient.md) - gRPC client implementation
- [`gRPCServer`](Documentations/gRPCServer.md) - gRPC server implementation
- [`KCPAdapter`](Documentations/KCPAdapter.md) - KCP/FastRing adapter layer
- [`KCPService`](Documentations/KCPService.md) - KCP service implementation
- [`XMPPServer`](Documentations/XMPPServer.md) - XMPP server module

## Documentations

Every module has an API reference under `Documentations/`, indexed and grouped by area in
[Documentations/README.md](Documentations/README.md). Two documents are worth reading
before the rest:

- [Descriptor lifecycle and concurrency rules](Documentations/Lifecycle.md) - ownership,
  reference counting, reentrancy and thread rules
- [`FastRing` API](Documentations/FastRing.md) - the core engine

## Limitations

- Linux-only target platform
- some modules depend on specific libc/liburing behavior
- low-level API: descriptor and buffer lifetime must be managed carefully

## License

MIT. See [LICENSE](LICENSE).
