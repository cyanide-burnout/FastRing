#include "FastBuffer.h"

#include <malloc.h>
#include <string.h>

struct FastBufferPool* CreateFastBufferPool(struct FastRing* ring)
{
  struct FastBufferPool* pool;

  pool        = (struct FastBufferPool*)calloc(1, sizeof(struct FastBufferPool));
  pool->ring  = ring;

  atomic_store_explicit(&pool->count, 1, memory_order_relaxed);

  return pool;
}

void ReleaseFastBufferPool(struct FastBufferPool* pool)
{
  struct FastBuffer* buffer;
  struct FastBuffer* heap;

  if ((pool != NULL) &&
      (atomic_fetch_sub_explicit(&pool->count, 1, memory_order_relaxed) == 1))
  {
    heap = atomic_load_explicit(&pool->heap, memory_order_acquire);
    heap = REMOVE_ABA_TAG(struct FastBuffer, heap, RING_DESC_ALIGNMENT);

    while (buffer = heap)
    {
      heap = REMOVE_ABA_TAG(struct FastBuffer, buffer->next, RING_DESC_ALIGNMENT);
      UpdateFastRingRegisteredBuffer(pool->ring, buffer->index, NULL, 0);
      free(buffer);
    }

    free(pool);
  }
}

void TryRegisterFastBuffer(struct FastBuffer* buffer, int option)
{
  struct FastBufferPool* pool;

  if ((option & FAST_BUFFER_REGISTER) &&
      (pool = buffer->pool) &&
      (pool->ring != NULL))
  {
    switch (buffer->status)
    {
      case FAST_BUFFER_STATUS_ADDED:
        buffer->index  = AddFastRingRegisteredBuffer(pool->ring, buffer->data, buffer->size);
        buffer->status = FAST_BUFFER_STATUS_UNCHANGED;
        break;

      case FAST_BUFFER_STATUS_UPDATED:
        buffer->index  = UpdateFastRingRegisteredBuffer(pool->ring, buffer->index, buffer->data, buffer->size);
        buffer->status = FAST_BUFFER_STATUS_UNCHANGED;
        break;
    }
  }
}

struct FastBuffer* AllocateFastBuffer(struct FastBufferPool* pool, uint32_t size, int option)
{
  uint32_t tag;
  void* _Atomic pointer;
  struct FastBuffer* buffer;

  tag = 0;
  atomic_fetch_add_explicit(&pool->count, 1, memory_order_relaxed);

  do pointer = atomic_load_explicit(&pool->heap, memory_order_acquire);
  while ((buffer = REMOVE_ABA_TAG(struct FastBuffer, pointer, RING_DESC_ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&pool->heap, &pointer, buffer->next, memory_order_relaxed, memory_order_relaxed)));

  if (buffer != NULL)
  {
    if (size <= buffer->size)
    {
      buffer->length = 0;
      buffer->next   = NULL;
      atomic_store_explicit(&buffer->count, 1, memory_order_release);
      return buffer;
    }

    // There is no alligned realloc, so only way is to release and allocate again
    UpdateFastRingRegisteredBuffer(pool->ring, buffer->index, NULL, 0);
    tag = buffer->tag;
    free(buffer);
  }

  if (buffer = (struct FastBuffer*)memalign(RING_DESC_ALIGNMENT, size + offsetof(struct FastBuffer, data)))
  {
    buffer->tag    = tag;
    buffer->pool   = pool;
    buffer->next   = NULL;
    buffer->size   = malloc_usable_size(buffer) - offsetof(struct FastBuffer, data);
    buffer->length = 0;
    buffer->status = FAST_BUFFER_STATUS_ADDED;
    buffer->index  = INT32_MIN;
    atomic_store_explicit(&buffer->count, 1, memory_order_release);
    TryRegisterFastBuffer(buffer, option);
    return buffer;
  }

  atomic_fetch_sub_explicit(&pool->count, 1, memory_order_relaxed);
  return NULL;
}

struct FastBuffer* HoldFastBuffer(struct FastBuffer* buffer)
{
  atomic_fetch_add_explicit(&buffer->count, 1, memory_order_relaxed);
  return buffer;
}

void ReleaseFastBuffer(struct FastBuffer* buffer)
{
  uint32_t tag;
  struct FastBufferPool* pool;

  if ((buffer != NULL) &&
      (atomic_fetch_sub_explicit(&buffer->count, 1, memory_order_relaxed) == 1))
  {
    pool = buffer->pool;
    tag  = atomic_fetch_add_explicit(&buffer->tag, 1, memory_order_relaxed) + 1;

    do buffer->next = atomic_load_explicit(&pool->heap, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&pool->heap, &buffer->next, ADD_ABA_TAG(buffer, tag, 0, RING_DESC_ALIGNMENT), memory_order_release, memory_order_relaxed));

    // Decrease pool reference count and release when required
    ReleaseFastBufferPool(pool);
  }
}

void PrepareFastBuffer(struct FastRingDescriptor* descriptor, struct FastBuffer* buffer)
{
  if (buffer->index >= 0)
  {
    descriptor->submission.ioprio    |= IORING_RECVSEND_FIXED_BUF;
    descriptor->submission.buf_index  = buffer->index;
  }
}

int CatchFastBuffer(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if ((descriptor->submission.opcode < IORING_OP_SEND_ZC) ||
      (completion == NULL) ||
      (completion->flags & IORING_CQE_F_NOTIF))
  {
    ReleaseFastBuffer(FAST_BUFFER(descriptor->submission.addr));
    descriptor->closure = NULL;
  }

  return (completion != NULL) && (completion->flags & IORING_CQE_F_MORE);
}

void* AllocateRingFastBuffer(size_t size, void* closure)
{
  struct FastBuffer* buffer;

  buffer = AllocateFastBuffer((struct FastBufferPool*)closure, size, 0);
  return (buffer != NULL) ? buffer->data : NULL;
}

void ReleaseRingFastBuffer(void* buffer)
{
  if (buffer != NULL)
  {
    // Prevent segmentation fault when NULL passed
    ReleaseFastBuffer(FAST_BUFFER(buffer));
  }
}
