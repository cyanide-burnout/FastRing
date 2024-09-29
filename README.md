# FastRing

Event multiplexing library for io_uring

This is extract of my libraries I am using in my projects https://brandmeister.network and https://tetrapack.online

## FastRing

TBD

## FastBuffer

Fast buffer pool implementation especialy to use with FastRing
Check usage examples at FastBIO and FastSocket

## CoRing

That is a small adapter to use in C++ coroutines with my Compromise library (https://github.com/cyanide-burnout/Compromise)

## FastGLoop

Adapter to incorporate Glib 2.0 main loop into FastRing. It creates green-thread / fiber for GLib by using ucontext.h.

- *CreateFastGLoop* - create a new instance of FastGLoop
- *ReleaseFastGLoop* - destroys FastGLoop
- *StopFastGLoop* - you optionally can call it before ReleaseFastGLoop when you need to make extra actions before destruction of GMainLoop.

`struct FastGLoop` provides two attributes for be used in user code:
- `GMainLoop* loop`
- `GMainContext* context`

## ThreadCall

Make a call to a handler running FastRing from any other thread

- *CreateThreadCall* - create a new ThreadCall
- *HoldThreadCall* - should be used by caller to increment weight (kind of reference counter) to hold the object
- *ReleaseThreadCall* - decrements weight, releases ThreadCall
- *FreeThreadCall* - simplified form of ReleaseThreadCall for caller, useful for callbacks
- *MakeVariadicThreadCall* / *MakeThreadCall* - makes a call

## FastSocket

Generic socket I/O through FastRing

## FastBIO / SSLSocket

Asynchronous TLS and BIO on top of OpenSSL

## DBusCore

D-BUS adapter for FastRing

## WatchDog

WatchDog implementation for systemd

## LuaPoll

Bindings to liblua / luajit. Please read LuaPoll.txt

## Resolver

Bindings to DNS resolution library C-ARES

