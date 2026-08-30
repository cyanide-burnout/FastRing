# ProtoBuf API Reference

Header: `Supplimentary/ProtoBuf.h`

`ProtoBuf` is a bump allocator for `protobuf-c`. It replaces per-field `malloc()`
during message unpacking with pointer arithmetic in a caller-supplied buffer, which is
what makes decoding a message on the stack practical inside a ring callback.

## API

```c
ProtobufCAllocator* InitializeProtoBufArena(void* buffer, size_t length);

#define CreateProtoBufArena(length)  InitializeProtoBufArena(alloca(sizeof(struct ProtoBufArena) + length), length)
```

- `InitializeProtoBufArena()` lays a `struct ProtoBufArena` over `buffer` and returns
  the `ProtobufCAllocator*` to pass to the generated `..._unpack()` functions. The
  buffer must be at least `sizeof(struct ProtoBufArena) + length` bytes.
- `CreateProtoBufArena()` is the same thing on the stack, via `alloca()`.

## Semantics

- Allocation is a bump: each request is rounded up to `__BIGGEST_ALIGNMENT__`, carved
  off the front of the arena, and zeroed.
- **`free` is a no-op.** Nothing is reclaimed until the whole arena goes away.
- When the arena is exhausted, the allocator returns `NULL`. `protobuf-c` treats that
  as an unpack failure, so an oversized message fails cleanly rather than corrupting
  anything — but it fails, so size the arena for the largest message you accept.

## Usage

```c
ProtobufCAllocator* allocator = CreateProtoBufArena(8192);
MyMessage* message = my_message__unpack(allocator, length, data);

if (message != NULL)
{
  ...
}

// No my_message__free_unpacked() call needed: the arena dies with the stack frame
```

Because the arena is on the stack, the decoded message and everything it points at are
only valid inside the current function. Copy out anything that has to outlive it.

## Rules

- **Never pass `NULL` as the allocator to `..._free_unpacked()` for a message that was
  unpacked from an arena.** That runs the default allocator's `free()` over pointers
  that were never malloc'd. Always pass the same allocator that unpacked it.
- Calling `..._free_unpacked(message, allocator)` with the arena's own allocator is
  harmless but pointless — it walks the message invoking a no-op free. Doing it anyway
  keeps the code symmetric with the heap case, which is why `Examples/gRPCClient` does.
- Do not return a decoded message, or any pointer into it, from the function that
  created the arena with `CreateProtoBufArena()`.
- One arena per message decode. Reusing an arena keeps accumulating until it is
  exhausted, since nothing is ever released.
- For messages whose size is not bounded, use a heap buffer with
  `InitializeProtoBufArena()` instead of `alloca()`.
