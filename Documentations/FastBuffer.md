# FastBuffer API Reference

Header: `Ring/FastBuffer.h`

`FastBuffer` is a reusable reference-counted buffer pool with optional `io_uring` fixed-buffer registration.

## Core Types

- `struct FastBufferPool` - shared pool
- `struct FastBuffer` - individual buffer

## API

```c
struct FastBufferPool* CreateFastBufferPool(struct FastRing* ring);
void ReleaseFastBufferPool(struct FastBufferPool* pool);
void TryRegisterFastBuffer(struct FastBuffer* buffer, int option);
struct FastBuffer* AllocateFastBuffer(struct FastBufferPool* pool, uint32_t size, int option);
struct FastBuffer* HoldFastBuffer(struct FastBuffer* buffer);
void ReleaseFastBuffer(struct FastBuffer* buffer);

void PrepareFastBuffer(struct FastRingDescriptor* descriptor, struct FastBuffer* buffer);
int CatchFastBuffer(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);

void* AllocateRingFastBuffer(size_t size, void* closure);
void ReleaseRingFastBuffer(void* buffer);
```

## Notes

- Use `FAST_BUFFER_REGISTER` to request fixed-buffer registration.
- `HoldFastBuffer()` / `ReleaseFastBuffer()` control ownership.
- `PrepareFastBuffer()` sets fixed-buffer fields in SQE when `buffer->index >= 0`.

