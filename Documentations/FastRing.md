# FastRing API Reference

This document describes the public C API from `Ring/FastRing.h`.

## Conventions

- Integer return codes use `errno` style:
  - success: `0` or positive value
  - error: negative code (`-EINVAL`, `-ENOMEM`, `-EBADF`, ...)
- `FastRingDescriptor` objects are reference-counted.
- Asynchronous completions are delivered while `WaitForFastRing()` is running.
- Callbacks run in the ring-processing thread.

## Threading Model

- `CreateFastRing()` captures the processing thread id.
- `WaitForFastRing()` should run in the owner thread loop.

## Lifecycle

```c
struct FastRing* CreateFastRing(uint32_t length);
void ReleaseFastRing(struct FastRing* ring);
int WaitForFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask);
```

- `CreateFastRing(length)`:
  - `length == 0` means auto-size from `RLIMIT_NOFILE`.
  - queue length is rounded to power-of-two.
  - returns `NULL` on init failure.
- `ReleaseFastRing()`:
  - releases ring resources, descriptors, flush handlers, registered file/buffer metadata.
- `WaitForFastRing(interval_ms, mask)`:
  - submits pending SQEs, handles CQEs, optionally waits.
  - `interval` is milliseconds.
  - `mask` is passed to `io_uring_submit_and_wait_timeout`.
  - returns `0` on timeout (`-ETIME` is normalized), negative on error.

## Descriptor API

```c
struct FastRingDescriptor* AllocateFastRingDescriptor(
  struct FastRing* ring,
  HandleFastRingCompletionFunction function,
  void* closure);

void PrepareFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last);
int ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor);
```

- `AllocateFastRingDescriptor()` initializes descriptor with `IORING_OP_NOP`, refcount `1`.
- `PrepareFastRingDescriptor()` generates `user_data` and marks descriptor pending.
- `SubmitFastRingDescriptor()` prepares and enqueues one descriptor.
- `SubmitFastRingDescriptorRange()` enqueues a prepared chain.
- `ReleaseFastRingDescriptor()` decrements references and recycles when count reaches `0`.

Completion callback:

```c
int (*HandleFastRingCompletionFunction)(
  struct FastRingDescriptor* descriptor,
  struct io_uring_cqe* completion,
  int reason);
```

Completion reasons:
- `RING_REASON_COMPLETE`
- `RING_REASON_INCOMPLETE`
- `RING_REASON_RELEASED`

## Flush Handlers

```c
struct FastRingFlusher* SetFastRingFlushHandler(
  struct FastRing* ring,
  HandleFastRingFlushFunction function,
  void* closure);

int RemoveFastRingFlushHandler(struct FastRing* ring, struct FastRingFlusher* flusher);
```

- Run after CQ processing inside `WaitForFastRing()`.
- `RemoveFastRingFlushHandler()` returns `0`, `-EPERM`, or `-EBADF`.

## Poll API

```c
int AddFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
int UpdateFastRingPoll(struct FastRing* ring, int handle, uint64_t flags);
int RemoveFastRingPoll(struct FastRing* ring, int handle);
void DestroyFastRingPoll(struct FastRing* ring, HandleFastRingPollFunction function, void* closure);
int SetFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure);
struct FastRingDescriptor* GetFastRingPollDescriptor(struct FastRing* ring, int handle);
```

Poll callback:

```c
void (*HandleFastRingPollFunction)(int handle, uint32_t events, void* closure, uint64_t options);
```

Flag helpers:
- `RING_POLL_READ`, `RING_POLL_WRITE`, `RING_POLL_ERROR`, `RING_POLL_HANGUP`
- high-bit behavior: `RING_POLL_EDGE`, `RING_POLL_SHOT`

## Watch API

```c
struct FastRingDescriptor* AddFastRingWatch(struct FastRing* ring, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);
void UpdateFastRingWatch(struct FastRingDescriptor* descriptor, uint32_t mask);
void RemoveFastRingWatch(struct FastRingDescriptor* descriptor);
struct FastRingDescriptor* SetFastRingWatch(struct FastRing* ring, struct FastRingDescriptor* descriptor, int handle, uint32_t mask, uint32_t flags, HandleFastRingWatchFunction function, void* closure);
```

Watch callback:

```c
void (*HandleFastRingWatchFunction)(struct FastRingDescriptor* descriptor, int result);
```

## Timeout API

```c
struct FastRingDescriptor* SetFastRingTimeout(... int64_t interval_ms, ...);
struct FastRingDescriptor* SetFastRingCertainTimeout(... struct timeval* interval, ...);
struct FastRingDescriptor* SetFastRingPreciseTimeout(... struct timespec* interval, ...);
```

Timeout callback:

```c
void (*HandleFastRingTimeoutFunction)(struct FastRingDescriptor* descriptor);
```

- `TIMEOUT_FLAG_REPEAT` enables repeating timeout.
- Update by passing existing descriptor.
- Remove by passing negative interval or `NULL` interval.

## Event API

```c
struct FastRingDescriptor* CreateFastRingEvent(struct FastRing* ring, HandleFastRingCompletionFunction function, void* closure);
int SubmitFastRingEvent(struct FastRing* ring, struct FastRingDescriptor* event, uint32_t parameter, int option);
```

- `SubmitFastRingEvent()` uses `io_uring msg_ring`.

## Buffer Provider API

```c
struct FastRingBufferProvider* CreateFastRingBufferProvider(...);
void ReleaseFastRingBufferProvider(...);
void PrepareFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_sqe* submission);
uint8_t* GetFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion);
void AdvanceFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion, CreateRingBufferFunction function, void* closure);
```

- `PrepareFastRingBuffer()` sets `IOSQE_BUFFER_SELECT`.
- `GetFastRingBuffer()` maps CQE buffer id to address.
- `AdvanceFastRingBuffer()` returns consumed slot back to the ring.

## Registered Files and Buffers

```c
int AddFastRingRegisteredFile(struct FastRing* ring, int handle);
void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle);
int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length);
int UpdateFastRingRegisteredBuffer(struct FastRing* ring, int index, void* address, size_t length);
```

- File registration returns fixed-file index (`>= 0`) or negative error.
- Buffer registration returns buffer index (`>= 0`) or negative error.
