#include "ProtoBuf.h"

#include <stddef.h>
#include <string.h>

#define ALIGNMENT  __BIGGEST_ALIGNMENT__

static void* Allocate(void* data, size_t size)
{
  struct ProtoBufArena* arena;
  void*  pointer;

  arena   = (struct ProtoBufArena*)data;
  size    = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
  pointer = NULL;

  if (size <= arena->length)
  {
    pointer = arena->pointer;
    memset(pointer, 0, size);
    arena->pointer += size;
    arena->length  -= size;
  }

  return pointer;
}

static void Free(void* data, void* pointer)
{
  // Dummy function
}

ProtobufCAllocator* InitializeProtoBufArena(void* buffer, size_t length)
{
  struct ProtoBufArena* arena;

  arena                       = (struct ProtoBufArena*)buffer;
  arena->table.alloc          = Allocate;
  arena->table.free           = Free;
  arena->table.allocator_data = arena;
  arena->pointer              = arena->data;
  arena->length               = length;

  return &arena->table;
}
