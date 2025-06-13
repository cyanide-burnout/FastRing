# FastRing

Event multiplexing library for io_uring

This is extract of my libraries I am using in my projects https://brandmeister.network and https://tetrapack.online

The library solves following problems:
- Submission/complition multiplexing
- Submission queue boundary control
- Events and handlers tracking, submission ordering
- Emulation of classic poll-based approach including handle watch and timers

## FastRing

TBD

## FastBuffer

Fast buffer pool implementation, specifically designed for use with FastRing.
See usage examples in FastBIO and FastSocket.

## CoRing

That is a small adapter to use in C++ coroutines with my Compromise library (https://github.com/cyanide-burnout/Compromise)

## FastGLoop

Adapter to incorporate Glib 2.0 main loop into FastRing. It creates green-thread / fiber for GLib by using ucontext.h.

- *CreateFastGLoop* - creates a new instance of FastGLoop
- *ReleaseFastGLoop* - destroys FastGLoop
- *StopFastGLoop* - you optionally can call it before ReleaseFastGLoop when you need to make extra actions before destruction of GMainLoop.

`struct FastGLoop` provides two attributes for be used in user code:
- `GMainLoop* loop`
- `GMainContext* context`

## ThreadCall

Make a call to a handler running FastRing from any other thread

- *CreateThreadCall* - creates a new ThreadCall
- *HoldThreadCall* - should be used by caller to increment weight (kind of reference counter) to hold the object
- *ReleaseThreadCall* - decrements weight, releases ThreadCall
- *FreeThreadCall* - simplified form of ReleaseThreadCall for caller, useful for callbacks
- *MakeVariadicThreadCall* / *MakeThreadCall* - makes a call

## FastSemaphore

Reactive backend for glibc's sem_t

- *SubmitFastSemaphoreWait* - registers an asynchronous handler to be called when a token becomes available, replaces sem_wait()
- *CancelFastSemaphoreWait* - cancels a previously registered asynchronous handler
- *SubmitFastSemaphorePost* - posts token to the semaphore, can be used instead of sem_post() to avoid a synchronous syscall futex_wake()

*FastSemaphoreFunction* can return 1 to continue receiving tokens or 0 to stop receiving tokens

## FastSocket

Generic socket I/O through FastRing

## FastBIO / SSLSocket

Asynchronous TLS and BIO on top of OpenSSL

## DBusCore

D-BUS adapter for FastRing

## WatchDog

Watchdog implementation for systemd

## LuaPoll

Bindings to liblua / luajit. Please read LuaPoll.txt

## Resolver

Bindings to DNS resolution library C-ARES

## LWSCore

WebSocket client library on top of libwebsockets, it uses main loop integration over Glib 2.0

## Fetch

CURL wrapper with asynchronous fetch using CURL's multi interface.

