# FastRing

`FastRing` is a set of high-performance asynchronous I/O modules built around `io_uring` (Linux).
It is designed for reactive networking workloads and low-overhead event handling.

This repository includes:
- the core event/descriptor engine (`Ring/FastRing.*`)
- networking and protocol adapters (`FastSocket`, `FastBIO`, `Fetch`, `CURLWSCore`, `Resolver`, etc.)
- supplementary modules (`Supplimentary/`)
- runnable examples (`Examples/`)

## What It Solves

FastRing provides:
- `SQE/CQE` multiplexing
- submission/completion queue flow control
- descriptor lifecycle tracking
- `poll/watch/timeout/event` primitives with an event-loop style API

## Requirements

- Linux with `io_uring` support (recommended: kernel >= 5.13)
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

## Repository Layout

- `Ring/` - core library and adapters
- `Supplimentary/` - extra modules (gRPC/H2O/KCP/XMPP, etc.)
- `Examples/` - example applications with per-folder Makefiles
- `Documentations/` - API and integration notes

## Documentations

- Documentation index: `Documentations/README.md`
- `FastRing` API: `Documentations/FastRing.md`
- `FastSocket` API: `Documentations/FastSocket.md`

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
Build examples from their own directories under `Examples/*`.

Example:

```bash
cd Examples/CURLWS
make
./curlwstest
```

Other example targets:
- `Examples/Avahi`
- `Examples/H2H3Server`
- `Examples/gRPCClient`
- `Examples/gRPCServer`

Dependencies for each example are defined in its local `Makefile` via `pkg-config`.

## Module Overview

### Core (`Ring/`)

- `FastRing` - core `io_uring` engine: submit/complete, poll/watch/timeout, descriptor lifecycle
- `FastBuffer` - buffer pool and buffer registration helpers
- `FastSocket` - asynchronous socket I/O on top of FastRing
- `FastBIO` - async OpenSSL BIO transport adapter
- `SSLSocket` - TLS socket layer built on OpenSSL
- `ThreadCall` - cross-thread calls into the ring handler thread
- `FastSemaphore` - reactive `sem_t` integration (glibc internals + io_uring futex ops)
- `FastGLoop` - `GLib` loop integration
- `FastUVLoop` - `libuv` loop integration
- `Fetch` - asynchronous wrapper over `libcurl` multi interface
- `CURLWSCore` - recommended WebSocket client adapter
- `LWSCore` - deprecated WebSocket adapter (kept for compatibility)
- `FastAvahiPoll` - Avahi poll adapter for FastRing
- `DBusCore` - D-Bus integration
- `Resolver` - c-ares DNS resolver integration
- `LuaPoll` - Lua/LuaJIT bindings
- `WatchDog` - systemd watchdog helper
- `RingProfiler` - profiling helpers for ring activity
- `CoRing` - C++ coroutine adapter

### Supplementary (`Supplimentary/`)

- `H2OCore` - H2O HTTP/2/HTTP/3 integration layer
- `PicoBundle` - picotls/certificate bundle helper
- `ProtoBuf` - protobuf-c support helpers
- `gRPC` - shared gRPC-related types/utilities
- `gRPCClient` - gRPC client implementation
- `gRPCServer` - gRPC server implementation
- `KCPAdapter` - KCP/FastRing adapter layer
- `KCPService` - KCP service implementation
- `XMPPServer` - XMPP server module

## Limitations

- Linux-only target platform
- some modules depend on specific libc/liburing behavior
- low-level API: descriptor and buffer lifetime must be managed carefully

## License

See `LICENSE`.
