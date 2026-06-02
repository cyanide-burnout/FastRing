#define _GNU_SOURCE

#include "LatchClient.h"

#include <errno.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/futex.h>

static inline int __attribute__((always_inline)) futex(uint32_t* address1, int operation, uint32_t value1, const struct timespec* timeout, uint32_t* address2, uint32_t value2)
{
  return syscall(SYS_futex, address1, operation, value1, timeout, address2, value2);
}

int LockLatch(struct LatchClient* client, struct timespec* timeout)
{
  struct Latch* latch;
  pid_t identifier;
  uint64_t request;
  uint64_t grant;
  uint64_t value;
  int error;

  if ((client != NULL) &&
      (latch   = client->latch))
  {
    identifier = gettid();
    grant      = ((uint64_t)identifier << LATCH_ID_SHIFT) | LATCH_STATE_LOCK_GRANTED;
    request    = ((uint64_t)identifier << LATCH_ID_SHIFT) | LATCH_STATE_LOCK_REQUESTED;

    for ( ; ; )
    {
      value = atomic_load_explicit(&latch->value, memory_order_acquire);

      if (value == grant)
      {
        client->identifier = identifier;
        return 0;
      }

      if ((value == 0ULL) &&
          atomic_compare_exchange_weak_explicit(&latch->value, &value, request, memory_order_acq_rel, memory_order_acquire))
      {
        while (futex(LATCH(&latch->value), FUTEX_WAKE_BITSET, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
        continue;
      }

      if ((futex(LATCH(&latch->value), FUTEX_WAIT_BITSET, (uint32_t)value, timeout, NULL, FUTEX_BITSET_MATCH_ANY) < 0) &&
          (errno != EAGAIN) &&
          (errno != EINTR))
      {
        error = -errno;
        value = request;

        if (atomic_compare_exchange_strong_explicit(&latch->value, &value, 0ULL, memory_order_acq_rel, memory_order_acquire))
        {
          // Revert pending lock request on timeout/error
          while (futex(LATCH(&latch->value), FUTEX_WAKE_BITSET, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
        }

        return error;
      }
    }
  }

  return -EINVAL;
}

void UnlockLatch(struct LatchClient* client)
{
  struct Latch* latch;
  uint64_t value;

  if ((client != NULL)          &&
      (latch   = client->latch) &&
      (value   = atomic_load_explicit(&latch->value, memory_order_relaxed)) &&
      ((value &  LATCH_STATE_MASK) == LATCH_STATE_LOCK_GRANTED)             &&
      ((value >> LATCH_ID_SHIFT)   == client->identifier))
  {
    atomic_store_explicit(&latch->value, 0, memory_order_relaxed);
    while (futex(LATCH(&latch->value), FUTEX_WAKE_BITSET, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
  }
}

struct LatchClient* CreateLatchClient(int handle)
{
  struct LatchClient* client;
  struct Latch* latch;
  struct stat status;

  if ((client = (struct LatchClient*)calloc(1, sizeof(struct LatchClient))) &&
      (fstat(handle, &status) >= 0)            &&
      (status.st_size == sizeof(struct Latch)) &&
      (latch  = (struct Latch*)mmap(NULL, sizeof(struct Latch), PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0)) &&
      (latch != MAP_FAILED))
  {
    client->latch  = latch;
    client->handle = handle;
    return client;
  }

  free(client);
  return NULL;
}

void ReleaseLatchClient(struct LatchClient* client)
{
  if (client != NULL)
  {
    UnlockLatch(client);
    munmap(client->latch, sizeof(struct Latch));
    close(client->handle);
    free(client);
  }

}
