# Documentation

Reference documents of the core and of the modules under `Ring/` and `Supplimentary/`. The
[README](../README.md) of the repository explains the problem, the architecture and how to start.

## Core Concepts

- [LIFECYCLE.md](LIFECYCLE.md) — the ring, descriptor states, references and ownership,
  completion, cancellation, Poll, Watch, Timeout, Event, flushers, threads, shutdown, pitfalls.
  Read this before writing a module that submits its own SQEs.

## Core

- [API.md](API.md) — the `io_uring` engine: ring, descriptors, flushers, discard, poll, watch,
  timeout, event, buffer provider, registered resources, limits
- [FastBuffer.md](FastBuffer.md) — reference-counted buffer pool with optional
  fixed-buffer registration
- [ThreadCall.md](ThreadCall.md) — synchronous calls from a foreign thread into the ring
  thread
- [FastSemaphore.md](FastSemaphore.md) — reactive POSIX `sem_t`: tokens are delivered to
  a handler instead of parking a thread in `sem_wait()`
- [CoRing.md](CoRing.md) — C++20 coroutine adapter over FastRing descriptors

## Sockets and TLS

- [FastSocket.md](FastSocket.md) — asynchronous socket send/receive on FastRing and
  FastBuffer
- [SSLSocket.md](SSLSocket.md) — TLS session wrapper: owns the `SSL*`, drives the
  handshake, reports session events
- [FastBIO.md](FastBIO.md) — OpenSSL `BIO` whose transport is io_uring, including kTLS
  offload

## HTTP, WebSocket and DNS

- [Fetch.md](Fetch.md) — the libcurl multi interface on the ring, and a multiplexer hosting
  protocol modules built on top of it
- [CURLWSCore.md](CURLWSCore.md) — WebSocket client on top of `Fetch` (recommended)
- [LWSCore.md](LWSCore.md) — legacy `libwebsockets` adapter (deprecated)
- [Resolver.md](Resolver.md) — c-ares DNS channel driven by the ring
- [H2OCore.md](H2OCore.md) — H2O HTTP/2 and HTTP/3 server runtime
- [PicoBundle.md](PicoBundle.md) — picotls certificate context derived from an `SSL_CTX`

## gRPC

- [gRPC.md](gRPC.md) — wire format constants and shared macros
- [gRPCClient.md](gRPCClient.md) — gRPC client over `Fetch`, with a protobuf-c service
  wrapper
- [gRPCServer.md](gRPCServer.md) — gRPC request dispatch wired into `H2OCore`
- [ProtoBuf.md](ProtoBuf.md) — bump allocator for `protobuf-c` unpacking

## Loop and System Integrations

- [FastGLoop.md](FastGLoop.md) — GLib main loop
- [FastUVLoop.md](FastUVLoop.md) — libuv loop
- [FastAvahiPoll.md](FastAvahiPoll.md) — Avahi poll adapter
- [DBusCore.md](DBusCore.md) — libdbus connection driven by the ring
- [LuaPoll.md](LuaPoll.md) — Lua/LuaJIT coroutines resumed by ring events
- [WatchDog.md](WatchDog.md) — systemd watchdog fed from a FastRing timeout

## Other Protocols and Services

- [KCPService.md](KCPService.md) — KCP protocol engine: conversations, congestion
  control, packet processing
- [KCPAdapter.md](KCPAdapter.md) — ready-made UDP transport binding for `KCPService`
- [XMPPServer.md](XMPPServer.md) — XMPP server on FastRing/FastSocket
- [SambarAdapter.md](SambarAdapter.md) — libsmb2 (SMB2) adapter (experimental)
- [Latch.md](Latch.md) — shared-memory latch that freezes a loop for cross-process write
  exclusion
