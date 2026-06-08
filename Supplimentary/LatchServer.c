#include "LatchServer.h"

#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#define LATCH_TIMEOUT  100  // milliseconds

static inline int __attribute__((always_inline)) futex(uint32_t* address1, int operation, uint32_t value1, const struct timespec* timeout, uint32_t* address2, uint32_t value2)
{
  return syscall(SYS_futex, address1, operation, value1, timeout, address2, value2);
}

static int HandleCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct LatchServer* server;
  struct timespec time;
  struct Latch* latch;
  uint64_t temporary;
  uint64_t value;

  if ((completion != NULL) &&
      (server = (struct LatchServer*)descriptor->closure) &&
      (latch  = server->latch))
  {
    if (value = atomic_load_explicit(&latch->value, memory_order_acquire))
    {
      if ((value & LATCH_STATE_MASK) == LATCH_STATE_LOCK_REQUESTED)
      {
        temporary = value;
        value     = (value & ~LATCH_STATE_MASK) | LATCH_STATE_LOCK_GRANTED;

        if (atomic_compare_exchange_strong_explicit(&latch->value, &temporary, value, memory_order_acq_rel, memory_order_acquire))
        {
          // LockLatch() may roll back request state on timeout
          while (futex(LATCH(&latch->value), FUTEX_WAKE_BITSET, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
        }
      }

      while (((value = atomic_load_explicit(&latch->value, memory_order_acquire)) & LATCH_STATE_MASK) == LATCH_STATE_LOCK_GRANTED)
      {
        clock_gettime(CLOCK_MONOTONIC, &time);
        time.tv_nsec += LATCH_TIMEOUT * 1000000L;

        if (time.tv_nsec >= 1000000000L)
        {
          time.tv_sec  += 1;
          time.tv_nsec -= 1000000000L;
        }

        if ((futex(LATCH(&latch->value), FUTEX_WAIT_BITSET, (uint32_t)value, &time, NULL, FUTEX_BITSET_MATCH_ANY) < 0) && (errno == ETIMEDOUT) &&
            (kill(value >> LATCH_PID_SHIFT, 0)                                                                    < 0) && (errno == ESRCH))
        {
          temporary = value;

          if (atomic_compare_exchange_strong_explicit(&latch->value, &temporary, 0ULL, memory_order_acq_rel, memory_order_acquire))
          {
            // Client process disappeared after grant; release the latch on its behalf
            while (futex(LATCH(&latch->value), FUTEX_WAKE_BITSET, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
          }
        }
      }
    }

    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

struct LatchServer* CreateLatchServer(struct FastRing* ring, int handle)
{
  struct Latch* latch;
  struct LatchServer* server;
  struct FastRingDescriptor* descriptor;

  latch      = NULL;
  server     = NULL;
  descriptor = NULL;

  if ((server     = (struct LatchServer*)calloc(1, sizeof(struct LatchServer))) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleCompletion, server)) &&
      (ftruncate(handle, sizeof(struct Latch)) >= 0) &&
      (latch  = (struct Latch*)mmap(NULL, sizeof(struct Latch), PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0)) &&
      (latch != MAP_FAILED))
  {
    server->latch      = latch;
    server->handle     = handle;
    server->descriptor = descriptor;

    atomic_store_explicit(&latch->value, 0, memory_order_relaxed);

    io_uring_prep_futex_wait(&descriptor->submission, LATCH(&latch->value), 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
    SubmitFastRingDescriptor(descriptor, 0);

    return server;
  }

  ReleaseFastRingDescriptor(descriptor);
  free(server);

  return NULL;
}

void ReleaseLatchServer(struct LatchServer* server)
{
  struct FastRingDescriptor* descriptor;

  if (server != NULL)
  {
    if ((descriptor = server->descriptor) &&
        (descriptor->function == HandleCompletion) &&
        (descriptor->submission.opcode == IORING_OP_FUTEX_WAIT))
    {
      descriptor->function = NULL;
      descriptor->closure  = NULL;

      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    }

    munmap(server->latch, sizeof(struct Latch));
    close(server->handle);
    free(server);
  }
}
