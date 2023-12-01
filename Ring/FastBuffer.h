#ifndef FASTBUFFER_H
#define FASTBUFFER_H

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FAST_BUFFER(address)          ((struct FastBuffer*)(((uint8_t*)(address)) - offsetof(struct FastBuffer, data)))

#define FAST_BUFFER_REGISTER          (1 << 0)

#define FAST_BUFFER_STATUS_ADDED      0
#define FAST_BUFFER_STATUS_UPDATED    1
#define FAST_BUFFER_STATUS_UNCHANGED  2

struct FastBuffer;
struct FastBufferStack;
struct FastBufferPool;

struct FastBuffer
{
  int index;                        // Index of registration or INT32_MIN (not set) or error code
  int status;                       // FAST_BUFFER_STATUS_*
  uint32_t size;                    // Size of available space for data
  uint32_t length;                  // Length of available data (optional)
  struct FastBufferPool* pool;      //
  ATOMIC(struct FastBuffer*) next;  //
  ATOMIC(uint32_t) count;           // Reference count
  ATOMIC(uint32_t) tag;             // Heap ABA tag
  uint8_t data[0];
};

struct FastBufferPool
{
  struct FastRing* ring;            //
  ATOMIC(uint32_t) count;           // Reference count
  ATOMIC(struct FastBuffer*) heap;  // Stack of available buffers
};

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

#ifdef __cplusplus
}
#endif

#endif
