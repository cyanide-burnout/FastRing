# ProtoBuf Arena API Reference

Header: `Supplimentary/ProtoBuf.h`

Small arena allocator helper for protobuf-c decoding/encoding workflows.

## API

```c
ProtobufCAllocator* InitializeProtoBufArena(void* buffer, size_t length);
```

Convenience macro:

```c
#define CreateProtoBufArena(length)  InitializeProtoBufArena(alloca(sizeof(struct ProtoBufArena) + length), length)
```

