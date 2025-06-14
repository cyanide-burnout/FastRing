#include "FastSemaphore.h"

#include <linux/futex.h>

/*
  Extract of glibc internals

  https://codebrowser.dev/glibc/glibc/sysdeps/nptl/internaltypes.h.html
  https://codebrowser.dev/glibc/glibc/nptl/sem_post.c.html
  https://codebrowser.dev/glibc/glibc/nptl/sem_wait.c.html
  https://codebrowser.dev/glibc/glibc/nptl/sem_waitcommon.c.html
  https://codebrowser.dev/glibc/glibc/nptl/futex-internal.c.html

  data:
    Lower 32 bits - tokens count
    Upper 32 bits - waiters count

  private:
    LLL_PRIVATE = 0
    LLL_SHARED  = FUTEX_PRIVATE_FLAG
*/

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define SEM_VALUE_OFFSET  0
#else
#define SEM_VALUE_OFFSET  1
#endif

#define SEM_NWAITERS_SHIFT  32
#define SEM_VALUE_MASK      UINT32_MAX
#define SEM_VALUE_MAX       INT32_MAX

struct new_sem
{
  uint64_t data;
  int private;
  int pad;
};

//

static int HandleSemaphoreWaitCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastSemaphoreData* data;
  struct new_sem* primitive;
  uint64_t _Atomic value;
  int result;
  int count;

  if ((completion != NULL) &&
      (~completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    data      = (struct FastSemaphoreData*)&descriptor->data;
    primitive = (struct new_sem*)data->semaphore;
    result    = 1;
    count     = data->limit;

    data->state ++;

    while ((count          != 0)    &&
           (result         != 0)    &&
           (data->function != NULL) &&
           (value = atomic_load_explicit(&primitive->data, memory_order_relaxed)) &&
           (value & SEM_VALUE_MASK))
    {
      if (!atomic_compare_exchange_weak_explicit(&primitive->data, &value, value - 1ULL - (1ULL << SEM_NWAITERS_SHIFT), memory_order_acquire, memory_order_relaxed))
      {
        // Try to grab both a token and stop being a waiter.  We need acquire MO so this synchronizes with all token providers (i.e.,
        // the RMW operation we read from or all those before it in modification order; also see sem_post).  On the failure path,
        // relaxed MO is sufficient because we only eventually need the up-to-date value; the futex_wait or the CAS perform the real work.
        continue;
      }

      result = data->function(data->semaphore, data->closure);

      atomic_fetch_add_explicit(&primitive->data, (uint64_t)(!!result) << SEM_NWAITERS_SHIFT, memory_order_relaxed);
      count --;
    }

    data->state --;

    if ((result         != 0) &&
        (data->function != NULL))
    {
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    data->function  = NULL;
    data->semaphore = NULL;
  }

  return 0;
}

struct FastRingDescriptor* SubmitFastSemaphoreWait(struct FastRing* ring, sem_t* semaphore, FastSemaphoreFunction function, void* closure, int limit)
{
  struct FastRingDescriptor* descriptor;
  struct FastSemaphoreData* data;
  struct new_sem* primitive;

  if (descriptor = AllocateFastRingDescriptor(ring, HandleSemaphoreWaitCompletion, NULL))
  {
    data            = (struct FastSemaphoreData*)&descriptor->data;
    primitive       = (struct new_sem*)semaphore;
    data->semaphore = semaphore;
    data->function  = function;
    data->closure   = closure;
    data->limit     = limit;
    data->state     = 0;

    atomic_fetch_add_explicit(&primitive->data, (1ULL << SEM_NWAITERS_SHIFT), memory_order_relaxed);

    io_uring_prep_futex_wait(&descriptor->submission, (uint32_t*)&primitive->data + SEM_VALUE_OFFSET, 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | primitive->private ^ FUTEX2_PRIVATE, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  return descriptor;
}

void CancelFastSemaphoreWait(struct FastRingDescriptor* descriptor)
{
  struct FastSemaphoreData* data;
  struct new_sem* primitive;

  if ((descriptor      != NULL) &&
      (data             = (struct FastSemaphoreData*)&descriptor->data) &&
      (data->semaphore != NULL) &&
      (data->function  != NULL) &&
      (data->state     == 0))
  {
    primitive       = (struct new_sem*)data->semaphore;
    data->semaphore = NULL;
    data->function  = NULL;
    atomic_fetch_sub_explicit(&primitive->data, (1ULL << SEM_NWAITERS_SHIFT), memory_order_relaxed);

    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

int SubmitFastSemaphorePost(struct FastRing* ring, sem_t* semaphore)
{
  struct FastRingDescriptor* descriptor;
  struct FastSemaphoreData* data;
  struct new_sem* primitive;
  uint64_t _Atomic value;

  primitive = (struct new_sem*)semaphore;

  do
  {
    value = atomic_load_explicit(&primitive->data, memory_order_relaxed);

    if ((value & SEM_VALUE_MASK) == SEM_VALUE_MAX)
    {
      // The maximum allowable value for a semaphore would be exceeded
      return -EOVERFLOW;
    }
  }
  while (!atomic_compare_exchange_weak_explicit(&primitive->data, &value, value + 1, memory_order_release, memory_order_relaxed));

  if (value >> SEM_NWAITERS_SHIFT)
  {
    descriptor = AllocateFastRingDescriptor(ring, NULL, NULL);

    if (descriptor == NULL)
    {
      // The available data space is not large enough
      return -ENOMEM;
    }

    io_uring_prep_futex_wake(&descriptor->submission, (uint32_t*)&primitive->data + SEM_VALUE_OFFSET, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | primitive->private ^ FUTEX2_PRIVATE, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  return 0;
}
