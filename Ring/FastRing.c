#define _GNU_SOURCE

#include "FastRing.h"

#include <malloc.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/resource.h>

#define likely(condition)     __builtin_expect(!!(condition), 1)
#define unlikely(condition)   __builtin_expect(!!(condition), 0)
#define barrier()             asm volatile ("" : : : "memory")
#define CAST(type, value)     (*(type*)&(value))

// https://github.com/torvalds/linux/blob/6e98b09da931a00bf4e0477d0fa52748bf28fcce/io_uring/io_uring.c#L100
#define RING_MAXIMUM_LENGTH      16384
#define RING_COMPLETION_RATIO    4

// https://github.com/torvalds/linux/blob/6e98b09da931a00bf4e0477d0fa52748bf28fcce/io_uring/rsrc.c#L33
#define FILE_MAXIMUM_COUNT       (1U << 20)
#define BUFFER_MAXIMUM_COUNT     (1U << 14)
#define FILE_REGISTRATION_RATIO  2

#define QUEUE_DEFAULT_LENGTH     256
#define FLUSH_LIST_INCREASE      64
#define FILE_LIST_INCREASE       1024

#ifndef USE_RING_LEVEL_TRIGGERING
#define RING_EVENT_FLAGS(flags)  ((~flags >> RING_EVENT_FLAGS_SHIFT) & (IORING_POLL_ADD_MULTI))
#else
#define RING_EVENT_FLAGS(flags)  ((~flags >> RING_EVENT_FLAGS_SHIFT) & (IORING_POLL_ADD_MULTI | IORING_POLL_ADD_LEVEL))
#endif

// Supplementary

static void* ExpandRingFileList(struct FastRingFileList* list, int handle)
{
  struct FastRingFileEntry* data;
  uint32_t length;

  length  = ((handle + 1) | (FILE_LIST_INCREASE - 1)) + 1;
  data    = (struct FastRingFileEntry*)realloc(list->data, length * sizeof(struct FastRingFileEntry));

  if (likely(data != NULL))
  {
    memset(data + list->length, 0, (length - list->length) * sizeof(struct FastRingFileEntry));
    list->length = length;
    list->data   = data;
  }

  return data;
}

static void* ExpandRingFlushList(struct FastRingFlushList* list)
{
  struct FastRingFlushEntry* data;
  uint32_t length;

  length = list->length + FLUSH_LIST_INCREASE;
  data   = (struct FastRingFlushEntry*)realloc(list->data, length * sizeof(struct FastRingFlushEntry));

  if (likely(data != NULL))
  {
    list->length = length;
    list->data   = data;
  }

  return data;
}

// FastRing

static inline __attribute__((always_inline)) void SetRingLock(struct FastRing* ring, int lock)
{
  if (likely(ring->lock.function != NULL))
  {
    // FastRing is not fully thread-safe, in some cases external mutex might me required
    ring->lock.function(lock, ring->lock.closure);
  }
}

static inline __attribute__((always_inline)) struct FastRingDescriptor* AllocateRingDescriptor(struct FastRing* ring)
{
  void* _Atomic pointer;
  struct FastRingDescriptor* descriptor;

  do pointer = atomic_load_explicit(&ring->available, memory_order_acquire);
  while ((descriptor = REMOVE_ABA_TAG(struct FastRingDescriptor, pointer, RING_DESC_ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&ring->available, &pointer, descriptor->next, memory_order_relaxed, memory_order_relaxed)));

  if (unlikely(((descriptor == NULL) ||
                (descriptor->state != RING_DESC_STATE_FREE)) &&
               (descriptor = (struct FastRingDescriptor*)memalign(RING_DESC_ALIGNMENT, sizeof(struct FastRingDescriptor)))))
  {
    memset(descriptor, 0, sizeof(struct FastRingDescriptor));
    do descriptor->heap = atomic_load_explicit(&ring->heap, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&ring->heap, &descriptor->heap, descriptor, memory_order_release, memory_order_acquire));
  }

  return descriptor;
}

static inline __attribute__((always_inline)) void ReleaseRingDescriptor(struct FastRing* ring, struct FastRingDescriptor* descriptor)
{
  uint32_t tag;

  tag = atomic_fetch_add_explicit(&descriptor->tag, 1, memory_order_relaxed) + 1;

  do descriptor->next = atomic_load_explicit(&ring->available, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&ring->available, &descriptor->next, ADD_ABA_TAG(descriptor, tag, 0, RING_DESC_ALIGNMENT), memory_order_release, memory_order_relaxed));
}

static inline __attribute__((always_inline)) void SubmitRingDescriptorRange(struct FastRing* ring, struct FastRingDescriptor* first, struct FastRingDescriptor* last)
{
  struct FastRingDescriptor* descriptor;

  atomic_store_explicit(&last->next, NULL, memory_order_relaxed);
  descriptor = atomic_exchange_explicit(&ring->pending, last, memory_order_relaxed);
  atomic_store_explicit(&descriptor->next, first, memory_order_relaxed);
}

static inline void HandleCompletedRingDescriptor(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  __builtin_prefetch(&descriptor->state);

  if (unlikely(ring->trace.function != NULL))
  {
    // Trace is only for debug purposes, less probable it is in use
    ring->trace.function(descriptor, completion, reason, ring->trace.closure);
  }

  if (likely(((descriptor->function == NULL) &&
              (( completion == NULL) ||
               (~completion->flags & IORING_CQE_F_MORE)) ||
              (descriptor->function != NULL) &&
              (descriptor->function(descriptor, completion, reason) == 0)) &&
             (atomic_fetch_sub_explicit(&descriptor->references, 1, memory_order_relaxed) == 1)))
  {
    if (descriptor->next != NULL)
    {
      descriptor->next->previous = NULL;
      descriptor->next           = NULL;
    }

    descriptor->function = NULL;
    descriptor->previous = NULL;
    descriptor->closure  = NULL;
    descriptor->state    = RING_DESC_STATE_FREE;

    atomic_thread_fence(memory_order_release);
    ReleaseRingDescriptor(ring, descriptor);
  }
}

int WaitFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask)
{
  int result;
  uint32_t position;
  struct io_uring_cqe* completion;
  struct io_uring_sqe* submission;
  struct __kernel_timespec timeout;
  struct FastRingFlushEntry* flusher;
  struct FastRingDescriptor* previous;
  struct FastRingDescriptor* descriptor;

  if (unlikely((ring == NULL) ||
               (ring->ring.sq.ring_sz == 0)))
  {
    // Ring is not properly initialised
    return -EINVAL;
  }

  // Check and handle CQ overflow

  if (unlikely(io_uring_cq_has_overflow(&ring->ring)))
  {
    result = io_uring_get_events(&ring->ring);
    goto Handle;
  }

  // Submit pending SQEs

  if (unlikely((descriptor = atomic_load_explicit(&ring->pending, memory_order_relaxed)) &&
               (atomic_load_explicit(&descriptor->next, memory_order_acquire) == NULL) &&
               (descriptor->state != RING_DESC_STATE_FREE) &&
               (descriptor = AllocateRingDescriptor(ring))))
  {
    // Try to submit stub descriptor to force submission of last valuable descriptor
    SubmitRingDescriptorRange(ring, descriptor, descriptor);
  }

  while ((descriptor = ring->submitting) &&
         (atomic_load_explicit(&descriptor->next, memory_order_acquire) != NULL))
  {
    if (descriptor->state == RING_DESC_STATE_FREE)
    {
      ring->submitting = atomic_load_explicit(&descriptor->next, memory_order_relaxed);
      ReleaseRingDescriptor(ring, descriptor);
      continue;
    }

    __builtin_prefetch(&descriptor->state);
    __builtin_prefetch(&descriptor->submission);
    __builtin_prefetch((uint8_t*)(&descriptor->submission) + sizeof(struct io_uring_sqe));

    if (likely(((descriptor->linked == 0) ||
                (descriptor->linked < io_uring_sq_space_left(&ring->ring))) &&
               (submission = io_uring_get_sqe(&ring->ring))))
    {
      memcpy(submission, &descriptor->submission, descriptor->length);
      descriptor->state = RING_DESC_STATE_SUBMITTED;
      ring->submitting  = atomic_load_explicit(&descriptor->next, memory_order_relaxed);
      continue;
    }

    break;
  }

  // Submit SQEs and handle CQEs without waiting when at least one CQE exists

  if (io_uring_cq_ready(&ring->ring) > 0)
  {
    result = io_uring_submit(&ring->ring);
    goto Handle;
  }

  // Wait for CQEs

  timeout.tv_sec  =  interval / 1000;
  timeout.tv_nsec = (interval % 1000) * 1000000;

  result = io_uring_submit_and_wait_timeout(&ring->ring, &completion, 1, &timeout, mask);

  // Handle CQEs

  Handle:

  SetRingLock(ring, RING_LOCK);

  io_uring_for_each_cqe(&ring->ring, position, completion)
  {
    if (likely(completion->user_data < RING_DATA_UNDEFINED))
    {
      descriptor = (struct FastRingDescriptor*)(completion->user_data & RING_DATA_ADDRESS_MASK);
      previous   = descriptor->previous;

      HandleCompletedRingDescriptor(ring, descriptor, completion, RING_REASON_COMPLETE);

      while (previous != NULL)
      {
        descriptor = previous;
        previous   = descriptor->previous;
        HandleCompletedRingDescriptor(ring, descriptor, NULL, RING_REASON_COMPLETE);
      }
    }

    // Advance is not so expensive, better to release a CQE ASAP
    io_uring_cq_advance(&ring->ring, 1);
  }

  while (ring->flushers.count > 0)
  {
    flusher = ring->flushers.data + (-- ring->flushers.count);
    flusher->function(ring, flusher->closure);
  }

  SetRingLock(ring, RING_UNLOCK);

  return result * (result != -ETIME);
}

int SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option)
{
  struct FastRing* ring;
  struct io_uring_sqe* submission;

  ring                             = descriptor->ring;
  descriptor->state                = RING_DESC_STATE_PENDING;
  descriptor->submission.user_data = (uintptr_t)descriptor | (uint64_t)(option & RING_DESC_OPTION_MASK);

  SubmitRingDescriptorRange(ring, descriptor, descriptor);
  return 0;
}

void SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last)
{
  SubmitRingDescriptorRange(first->ring, first, last);
}

struct FastRingDescriptor* AllocateFastRingDescriptor(struct FastRing* ring, HandleFastRingEventFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  descriptor = NULL;

  if (likely((ring != NULL) &&
             (descriptor = AllocateRingDescriptor(ring))))
  {
    descriptor->ring     = ring;
    descriptor->state    = RING_DESC_STATE_ALLOCATED;
    descriptor->length   = sizeof(struct io_uring_sqe);
    descriptor->closure  = closure;
    descriptor->function = function;
    descriptor->previous = NULL;
    descriptor->linked   = 0;

    io_uring_prep_nop(&descriptor->submission);
    atomic_store_explicit(&descriptor->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&descriptor->references, 1, memory_order_release);
  }

  return descriptor;
}

void ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor)
{
  if (likely((descriptor != NULL) &&
             (atomic_fetch_sub_explicit(&descriptor->references, 1, memory_order_relaxed) == 1)))
  {
    descriptor->function = NULL;
    descriptor->previous = NULL;
    descriptor->closure  = NULL;
    descriptor->state    = RING_DESC_STATE_FREE;

    atomic_thread_fence(memory_order_release);
    ReleaseRingDescriptor(descriptor->ring, descriptor);
  }
}

void SetFastRingFlushHandler(struct FastRing* ring, HandleFlushFunction function, void* closure)
{
  struct FastRingFlushEntry* flusher;

  if (likely((ring->flushers.count < ring->flushers.length) ||
             (ExpandRingFlushList(&ring->flushers) != NULL)))
  {
    flusher = ring->flushers.data + (ring->flushers.count ++);
    flusher->function = function;
    flusher->closure  = closure;
  }
}

uint16_t GetFastRingBufferGroup(struct FastRing* ring)
{
  return ++ ring->groups;
}

struct FastRing* CreateFastRing(uint32_t length, LockFastRingFunction function, void* closure)
{
  struct FastRing* ring;
  struct utsname name;
  struct rlimit limit;

  if ((uname(&name) < 0) ||
      (name.release[1] == '.') &&
      (strncmp(name.release, "5.18.", 5) < 0))
  {
    // IORING_POLL_ADD_MULTI is supported since kernel version 5.13
    return NULL;
  }

  memset(&limit, 0, sizeof(struct rlimit));
  getrlimit(RLIMIT_NOFILE, &limit);

  ring = (struct FastRing*)calloc(1, sizeof(struct FastRing));

  if (ring != NULL)
  {
    length += (length == 0) * limit.rlim_cur;
    length  = 1 << (32 - __builtin_clz(length - 1));  // Rounding up to next power of 2
    length  = ((length == 0) || (length > RING_MAXIMUM_LENGTH)) ? RING_MAXIMUM_LENGTH : length;

    ring->parameters.flags      = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
    ring->parameters.cq_entries = length * RING_COMPLETION_RATIO;

    io_uring_queue_init_params(length, &ring->ring, &ring->parameters);
    io_uring_ring_dontfork(&ring->ring);

    ring->limit = limit.rlim_cur / FILE_REGISTRATION_RATIO;
    ring->limit = ((ring->limit == 0) || (ring->limit > FILE_MAXIMUM_COUNT)) ? FILE_MAXIMUM_COUNT : ring->limit;

    io_uring_register_files_sparse(&ring->ring, ring->limit);
    io_uring_register_file_alloc_range(&ring->ring, ring->limit / 2, ring->limit / 2);

    ring->probe         = io_uring_get_probe_ring(&ring->ring);
    ring->thread        = gettid();
    ring->lock.closure  = closure;
    ring->lock.function = function;
    ring->submitting    = AllocateRingDescriptor(ring);

    atomic_store_explicit(&ring->pending, ring->submitting, memory_order_release);
  }

  return ring;
}

void ReleaseFastRing(struct FastRing* ring)
{
  struct FastRingDescriptor* current;
  struct FastRingDescriptor* next;

  if (ring != NULL)
  {
    next = atomic_load_explicit(&ring->heap, memory_order_acquire);
    while (current = next)
    {
      next = current->heap;
      if (current->function != NULL)
      {
        // Before freeing a descriptor call related handler to complete all incomplete submissions
        current->function(current, NULL, RING_REASON_RELEASED);
      }
      free(current);
    }

    io_uring_free_probe(ring->probe);
    io_uring_queue_exit(&ring->ring);
    free(ring->flushers.data);
    free(ring->files.data);
    free(ring->filters);
    free(ring->buffers);
    free(ring);
  }
}

// FastPoll

static int HandlePollEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if (likely((completion != NULL) &&
             (completion->res >= 0) &&
             (descriptor->data.poll.function != NULL) &&
             (~completion->user_data & RING_DESC_OPTION_MASK)))
    descriptor->data.poll.function(descriptor->data.poll.handle, completion->res, descriptor->closure, descriptor->data.poll.flags);

  if (likely((atomic_load_explicit(&descriptor->references, memory_order_relaxed) == 1) &&
             ((completion == NULL) ||
              (~completion->flags & IORING_CQE_F_MORE)) &&
             (descriptor == descriptor->ring->files.data[descriptor->data.poll.handle].poll)))
    descriptor->ring->files.data[descriptor->data.poll.handle].poll = NULL;

  return (completion != NULL) && (completion->flags & IORING_CQE_F_MORE);
}

int AddFastRingEventHandler(struct FastRing* ring, int handle, uint64_t flags, HandlePollEventFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  if (likely((ring != NULL) &&
             (handle >= 0)  &&
             ((handle < ring->files.length) ||
              (ExpandRingFileList(&ring->files, handle) != NULL)) &&
             (descriptor = AllocateFastRingDescriptor(ring, HandlePollEvent, closure))))
  {
    ring->files.data[handle].poll = descriptor;

    io_uring_prep_poll_add(&descriptor->submission, handle, flags);
    descriptor->submission.len     = RING_EVENT_FLAGS(flags);
    descriptor->data.poll.function = function;
    descriptor->data.poll.handle   = handle;
    descriptor->data.poll.flags    = flags;

    return SubmitFastRingDescriptor(descriptor, 0);
  }

  return -ENOMEM;
}

int ModifyFastRingEventHandler(struct FastRing* ring, int handle, uint64_t flags)
{
  struct FastRingDescriptor* descriptor;

  if (likely((ring != NULL) &&
             (handle >= 0)  &&
             (handle < ring->files.length) &&
             (descriptor = ring->files.data[handle].poll)))
  {
    descriptor->data.poll.flags = flags;

    if (unlikely((descriptor->state == RING_DESC_STATE_PENDING) &&
                 (descriptor->submission.opcode == IORING_OP_POLL_ADD)))
    {
      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
      descriptor->submission.len           = RING_EVENT_FLAGS(flags);
      return 0;
    }

    if (unlikely(descriptor->state == RING_DESC_STATE_PENDING))
    {
      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
      descriptor->submission.len           = IORING_POLL_UPDATE_EVENTS | RING_EVENT_FLAGS(flags);
      return 0;
    }

    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_poll_update(&descriptor->submission, (uintptr_t)descriptor, (uintptr_t)descriptor, flags, IORING_POLL_UPDATE_EVENTS | RING_EVENT_FLAGS(flags));
    return SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }

  return -EBADF;
}

int RemoveFastRingEventHandler(struct FastRing* ring, int handle)
{
  struct FastRingDescriptor* descriptor;

  if (likely((ring != NULL) &&
             (handle >= 0)  &&
             (handle < ring->files.length) &&
             (descriptor = ring->files.data[handle].poll)))
  {
    descriptor->data.poll.function = NULL;
    ring->files.data[handle].poll  = NULL;

    if (unlikely((descriptor->state == RING_DESC_STATE_PENDING) &&
                 (descriptor->submission.opcode == IORING_OP_POLL_ADD)))
    {
      io_uring_prep_nop(&descriptor->submission);
      descriptor->submission.user_data |= RING_DESC_OPTION_IGNORE;
      return 0;
    }

    if (unlikely(descriptor->state == RING_DESC_STATE_PENDING))
    {
      io_uring_prep_poll_remove(&descriptor->submission, (uintptr_t)descriptor);
      return 0;
    }

    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_poll_remove(&descriptor->submission, (uintptr_t)descriptor);
    return SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }

  return -EBADF;
}

void DestroyFastRingEventHandler(struct FastRing* ring, HandlePollEventFunction function, void* closure)
{
  struct FastRingFileEntry* entry;
  struct FastRingFileEntry* limit;
  struct FastRingDescriptor* descriptor;

  if (likely(ring != NULL))
  {
    entry = ring->files.data;
    limit = ring->files.data + ring->files.length;

    while (entry < limit)
    {
      descriptor = (entry ++)->poll;
      if (unlikely((descriptor                     != NULL)            &&
                   (descriptor->function           == HandlePollEvent) &&
                   (descriptor->data.poll.function == function)        &&
                   (descriptor->closure            == closure)))
      {
        // Remove handler and submit cancel request
        RemoveFastRingEventHandler(ring, descriptor->data.poll.handle);
      }
    }
  }
}

int ManageFastRingEventHandler(struct FastRing* ring, int handle, uint64_t flags, HandlePollEventFunction function, void* closure)
{
  int result;

  result = (flags == 0) ? RemoveFastRingEventHandler(ring, handle) : ModifyFastRingEventHandler(ring, handle, flags);
  return (result == -EBADF) && (flags != 0) ? AddFastRingEventHandler(ring, handle, flags, function, closure) : result;
}

void* GetFastRingEventHandlerData(struct FastRing* ring, int handle)
{
  return
    (ring != NULL) && (handle < ring->files.length) && (ring->files.data[handle].poll != NULL) ?
    ring->files.data[handle].poll->closure : NULL;
}

int IsFastRingThread(struct FastRing* ring)
{
  static __thread pid_t thread = 0;

  if (unlikely(thread == 0))        thread = gettid();
  if (unlikely(ring->thread == 0))  return -1;

  return ring->thread == thread;
}

// Timeout

static int HandleTimeoutEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if (unlikely((completion != NULL) &&
               (completion->res == -ETIME) &&
               (descriptor->data.timeout.function != NULL)))
  {
    descriptor->data.timeout.function(descriptor);

#ifdef IORING_TIMEOUT_MULTISHOT
    if (likely((completion->flags & IORING_CQE_F_MORE) &&
               ((descriptor->data.timeout.flags & TIMEOUT_FLAG_REPEAT) != 0ULL)))
    {
      // Since liburing 2.5 there are embedded capabilities for multi-shot timeouts
      return 1;
    }
#endif

    if (likely((descriptor->data.timeout.flags & TIMEOUT_FLAG_REPEAT) != 0ULL))
    {
      io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, descriptor->data.timeout.flags);
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }
  }

  return 0;
}

static void CreateTimeout(struct FastRingDescriptor* descriptor, HandleTimeoutFunction function, void* closure, uint64_t flags)
{
  descriptor->data.timeout.flags    = flags;
  descriptor->data.timeout.function = function;
  descriptor->closure               = closure;

  io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, flags);
  SubmitFastRingDescriptor(descriptor, 0);
}

static void UpdateTimeout(struct FastRingDescriptor* descriptor, HandleTimeoutFunction function, void* closure)
{
  descriptor->data.timeout.function = function;
  descriptor->closure               = closure;

  if (likely(descriptor->state != RING_DESC_STATE_PENDING))
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_timeout_update(&descriptor->submission, &descriptor->data.timeout.interval, (uintptr_t)descriptor, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

static void RemoveTimeout(struct FastRingDescriptor* descriptor)
{
  descriptor->data.timeout.flags    = 0;
  descriptor->data.timeout.function = NULL;
  descriptor->closure               = NULL;

  if (unlikely((descriptor->state == RING_DESC_STATE_PENDING) &&
               (descriptor->submission.opcode == IORING_OP_TIMEOUT)))
  {
    io_uring_prep_nop(&descriptor->submission);
    descriptor->submission.user_data |= (uint64_t)RING_DESC_OPTION_IGNORE;
    return;
  }

  if (unlikely(descriptor->state == RING_DESC_STATE_PENDING))
  {
    io_uring_prep_timeout_remove(&descriptor->submission, (uintptr_t)descriptor, 0);
    return;
  }

  atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
  io_uring_prep_timeout_remove(&descriptor->submission, (uintptr_t)descriptor, 0);
  SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
}

struct FastRingDescriptor* SetFastRingTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, int64_t interval, uint64_t flags, HandleTimeoutFunction function, void* closure)
{
  if ((interval < 0) &&
      (descriptor != NULL))
  {
    RemoveTimeout(descriptor);
    return NULL;
  }

  if ((interval >= 0) &&
      (descriptor != NULL))
  {
    descriptor->data.timeout.interval.tv_sec  =  interval / 1000;
    descriptor->data.timeout.interval.tv_nsec = (interval % 1000) * 1000000;
    UpdateTimeout(descriptor, function, closure);
    return descriptor;
  }

  if ((ring != NULL) &&
      (interval >= 0) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleTimeoutEvent, NULL)))
  {
    descriptor->data.timeout.interval.tv_sec  =  interval / 1000;
    descriptor->data.timeout.interval.tv_nsec = (interval % 1000) * 1000000;
    CreateTimeout(descriptor, function, closure, flags);
    return descriptor;
  }

  return NULL;
}

struct FastRingDescriptor* SetFastRingCertainTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timeval* interval, uint64_t flags, HandleTimeoutFunction function, void* closure)
{
  if ((interval == NULL) &&
      (descriptor != NULL))
  {
    RemoveTimeout(descriptor);
    return NULL;
  }

  if ((interval != NULL) &&
      (descriptor != NULL))
  {
    descriptor->data.timeout.interval.tv_sec  = interval->tv_sec;
    descriptor->data.timeout.interval.tv_nsec = interval->tv_usec * 1000;
    UpdateTimeout(descriptor, function, closure);
    return descriptor;
  }

  if ((ring != NULL) &&
      (interval != NULL) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleTimeoutEvent, NULL)))
  {
    descriptor->data.timeout.interval.tv_sec  = interval->tv_sec;
    descriptor->data.timeout.interval.tv_nsec = interval->tv_usec * 1000;
    CreateTimeout(descriptor, function, closure, flags);
    return descriptor;
  }

  return NULL;
}

struct FastRingDescriptor* SetFastRingPreciseTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timespec* interval, uint64_t flags, HandleTimeoutFunction function, void* closure)
{
  if ((interval == NULL) &&
      (descriptor != NULL))
  {
    RemoveTimeout(descriptor);
    return NULL;
  }

  if ((interval != NULL) &&
      (descriptor != NULL))
  {
    descriptor->data.timeout.interval.tv_sec  = interval->tv_sec;
    descriptor->data.timeout.interval.tv_nsec = interval->tv_nsec;
    UpdateTimeout(descriptor, function, closure);
    return descriptor;
  }

  if ((ring != NULL) &&
      (interval != NULL) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleTimeoutEvent, NULL)))
  {
    descriptor->data.timeout.interval.tv_sec  = interval->tv_sec;
    descriptor->data.timeout.interval.tv_nsec = interval->tv_nsec;
    CreateTimeout(descriptor, function, closure, flags);
    return descriptor;
  }

  return NULL;
}

// Buffer Provider

struct FastRingBufferProvider* CreateFastRingBufferProvider(struct FastRing* ring, uint16_t group, uint16_t count, uint32_t length, CreateRingBufferFunction function, void* closure)
{
  size_t alignment;
  struct io_uring_buf* buffer;
  struct io_uring_buf_ring* data;
  struct FastRingBufferProvider* provider;

  alignment  = getpagesize();
  count     += (count == 0) * ring->ring.cq.ring_entries;     // By default use count of CQE
  count      = 1 << (32 - __builtin_clz(count - 1));          // Rounding up to next power of 2
  group      = group ? group : GetFastRingBufferGroup(ring);  // Group number can be predefined
  provider   = (struct FastRingBufferProvider*)calloc(1, sizeof(struct FastRingBufferProvider) + count * sizeof(uintptr_t));
  data       = (struct io_uring_buf_ring*)memalign(alignment, sizeof(struct io_uring_buf_ring) + count * sizeof(struct io_uring_buf));

  if ((data     == NULL) ||
      (provider == NULL))
  {
    free(data);
    free(provider);
    return NULL;
  }

  provider->ring   = ring;
  provider->data   = data;
  provider->length = length;

  provider->registration.ring_addr    = (uintptr_t)data;
  provider->registration.ring_entries = count;
  provider->registration.bgid         = group;

  io_uring_buf_ring_init(provider->data);

  if (io_uring_register_buf_ring(&ring->ring, &provider->registration, 0) != 0)
  {
    free(data);
    free(provider);
    return NULL;
  }

  while (count > 0)
  {
    provider->map[-- count] = (uintptr_t)function(length, closure);

    buffer       = provider->data->bufs + count;
    buffer->addr = provider->map[count];
    buffer->len  = length;
    buffer->bid  = count;
  }

  atomic_store_explicit((_Atomic __u16*)&data->tail, provider->registration.ring_entries, memory_order_release);

  return provider;
}

void ReleaseFastRingBufferProvider(struct FastRingBufferProvider* provider, ReleaseRingBufferFunction function)
{
  if (provider != NULL)
  {
    io_uring_unregister_buf_ring(&provider->ring->ring, provider->registration.bgid);

    while ((function != NULL) && (provider->registration.ring_entries > 0))
    {
      provider->registration.ring_entries --;
      function((void*)provider->map[provider->registration.ring_entries]);
    }

    free(provider->data);
    free(provider);
  }
}

void PrepareFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_sqe* submission)
{
  submission->flags     |= IOSQE_BUFFER_SELECT;
  submission->buf_group  = provider->registration.bgid;
}

uint8_t* GetFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion)
{
  return
    likely((completion != NULL) && (completion->flags & IORING_CQE_F_BUFFER)) ? 
    (uint8_t*)provider->map[completion->flags >> IORING_CQE_BUFFER_SHIFT] :
    NULL;
}

void AdvanceFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion, CreateRingBufferFunction function, void* closure)
{
  uint16_t tail;
  uint16_t index;
  struct io_uring_buf* buffer;

  if (likely((completion != NULL) &&
             (completion->flags & IORING_CQE_F_BUFFER)))
  {
    index = completion->flags >> IORING_CQE_BUFFER_SHIFT;

    if (likely(function != NULL))
    {
      // Replace existing buffer with new supplied
      provider->map[index] = (uintptr_t)function(provider->length, closure);
    }

    tail  = atomic_load_explicit((_Atomic __u16*)&provider->data->tail, memory_order_relaxed);
    tail &= provider->registration.ring_entries - 1;
  
    buffer       = provider->data->bufs + tail;
    buffer->addr = provider->map[index];
    buffer->len  = provider->length;
    buffer->bid  = index;

    atomic_fetch_add_explicit((_Atomic __u16*)&provider->data->tail, 1, memory_order_release);
  }
}

// Registered File

int AddFastRingRegisteredFile(struct FastRing* ring, int handle)
{
  int result;
  uint32_t limit;
  uint32_t index;
  struct FastRingFileEntry* entry;

  result = -EBADF;

  if (likely((ring != NULL) &&
             (handle >= 0)  &&
             ((handle < ring->files.length) ||
              (ExpandRingFileList(&ring->files, handle) != NULL))))
  {
    entry = ring->files.data + handle;

    if (likely(entry->references > 0))
    {
      entry->references ++;
      return entry->index;
    }

    index = 0;
    limit = ring->limit / 2;

    if (unlikely(ring->filters == NULL))
    {
      // Upper part is reserved for allocation by kernel, a half will be enough
      ring->filters = (uint32_t*)calloc((limit >> 5) + 1, sizeof(uint32_t));
    }

    while ((index < limit) &&
           (ring->filters[index >> 5] == UINT32_MAX))
      index += 32;

    index += __builtin_ffs(~ring->filters[index >> 5]) - 1;

    if (unlikely(index >= limit))
    {
      // All registered files are already in use
      return -EOVERFLOW;
    }

    result = io_uring_register_files_update(&ring->ring, index, &handle, 1);

    if (likely(result >= 0))
    {
      ring->filters[index >> 5] |= 1 << (index & 31);
      entry->index       = index;
      entry->references ++;
      return index;
    }
  }

  return result;
}

void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle)
{
  struct FastRingFileEntry* entry;

  if (likely((ring != NULL) &&
             (handle >= 0)  &&
             (handle < ring->files.length)       &&
             (entry = ring->files.data + handle) &&
             (entry->references > 0)             &&
             ((-- entry->references) == 0)))
  {
    handle = -1;
    ring->filters[entry->index >> 5] &= ~(1 << (entry->index & 31));
    io_uring_register_files_update(&ring->ring, entry->index, &handle, 1);
  }
}

// Registered Buffer

int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length)
{
  __u64 tag;
  int result;
  struct iovec* vector;
  struct FastRingBufferList* buffers;

  result = -EINVAL;

  if (likely((ring != NULL) &&
             (address != NULL)))
  {
    buffers = ring->buffers;

    if (unlikely(buffers == NULL))
    {
      buffers         = (struct FastRingBufferList*)calloc(1, sizeof(struct FastRingBufferList) + sizeof(struct iovec) * BUFFER_MAXIMUM_COUNT);
      ring->buffers   = buffers;
      buffers->length = BUFFER_MAXIMUM_COUNT;
      io_uring_register_buffers_sparse(&ring->ring, buffers->length);
    }

    if (unlikely(buffers->count == buffers->length))
    {
      // All registered files are already in use
      return -EOVERFLOW;
    }

    while (buffers->vectors[buffers->position].iov_base != NULL)
    {
      buffers->position ++;
      buffers->position &= buffers->length - 1;
    }

    vector           = buffers->vectors + buffers->position;
    vector->iov_base = address;
    vector->iov_len  = length;

    tag    = 0;
    result = io_uring_register_buffers_update_tag(&ring->ring, buffers->position, vector, &tag, 1);

    if (likely(result >= 0))
    {
      buffers->count ++;
      return buffers->position;
    }

    vector->iov_base = NULL;
    vector->iov_len  = 0;
  }

  return result;
}

int UpdateFastRingRegisteredBuffer(struct FastRing* ring, int index, void* address, size_t length)
{
  __u64 tag;
  int result;
  struct iovec* vector;
  struct FastRingBufferList* buffers;

  result = -EINVAL;

  if (likely((index >= 0) &&
             (ring != NULL) &&
             (buffers = ring->buffers)))
  {
    vector           = buffers->vectors + index;
    vector->iov_base = address;
    vector->iov_len  = length;

    tag    = 0;
    result = io_uring_register_buffers_update_tag(&ring->ring, index, vector, &tag, 1);

    if (likely(result >= 0))
    {
      buffers->count -= (address == NULL);
      return buffers->position;
    }
  }

  return result;
}
