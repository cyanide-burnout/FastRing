#define _GNU_SOURCE
#define __USE_GNU

#include "FastRing.h"

#include <malloc.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/resource.h>

#define likely(condition)     __builtin_expect(!!(condition), 1)
#define unlikely(condition)   __builtin_expect(!!(condition), 0)

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
#define RING_POLL_FLAGS(flags)  ((~flags >> RING_POLL_FLAGS_SHIFT) & (IORING_POLL_ADD_MULTI))
#else
#define RING_POLL_FLAGS(flags)  ((~flags >> RING_POLL_FLAGS_SHIFT) & (IORING_POLL_ADD_MULTI | IORING_POLL_ADD_LEVEL))
#endif

// Supplementary

static __attribute__((constructor)) void Initialize()
{
#ifdef __aarch64__
  prctl(PR_SET_TAGGED_ADDR_CTRL, 0UL, 0UL, 0UL, 0UL);
#endif
}

static void* ExpandRingFileList(struct FastRingFileList* list, int handle)
{
  struct FastRingFileEntry* entries;
  uint32_t length;

  length  = ((handle + 1) | (FILE_LIST_INCREASE - 1)) + 1;
  entries = (struct FastRingFileEntry*)realloc(list->entries, length * sizeof(struct FastRingFileEntry));

  if (likely(entries != NULL))
  {
    memset(entries + list->length, 0, (length - list->length) * sizeof(struct FastRingFileEntry));
    list->length  = length;
    list->entries = entries;
  }

  return entries;
}

// FastRing

static inline __attribute__((always_inline)) void ReleaseRingFlusherStack(struct FastRingFlusherStack* stack)
{
  struct FastRingFlusher* current;
  struct FastRingFlusher* next;

  next = atomic_load_explicit(&stack->top, memory_order_acquire);
  while (current = REMOVE_ABA_TAG(struct FastRingFlusher, next, RING_FLUSH_ALIGNMENT))
  {
    next = current->next;
    if (likely(current->state == RING_FLUSH_STATE_PENDING))
    {
      // Before freeing a flusher call related handler to complete all incomplete activity
      current->function(current->closure, RING_REASON_RELEASED);
    }
    free(current);
  }
}

static inline __attribute__((always_inline)) void PushRingFlusher(struct FastRingFlusherStack* stack, struct FastRingFlusher* flusher, int increment)
{
  uint32_t tag;

  tag = atomic_fetch_add_explicit(&flusher->tag, increment, memory_order_relaxed) + increment;

  do flusher->next = atomic_load_explicit(&stack->top, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&stack->top, &flusher->next, ADD_ABA_TAG(flusher, tag, RING_FLUSH_ALIGNMENT), memory_order_release, memory_order_relaxed));
}

static inline __attribute__((always_inline)) struct FastRingFlusher* PopRingFlusher(struct FastRingFlusherStack* stack)
{
  void* pointer;
  struct FastRingFlusher* flusher;

  do pointer = atomic_load_explicit(&stack->top, memory_order_acquire);
  while ((flusher = REMOVE_ABA_TAG(struct FastRingFlusher, pointer, RING_FLUSH_ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&stack->top, &pointer, flusher->next, memory_order_acquire, memory_order_relaxed)));

  return flusher;
}

static inline __attribute__((always_inline)) void ReleaseRingDescriptorHeap(struct FastRingDescriptorSet* set)
{
  struct FastRingDescriptor* current;
  struct FastRingDescriptor* next;

  next = atomic_exchange_explicit(&set->heap, NULL, memory_order_acquire);

  while (current = next)
  {
    next = current->heap;
    if ((current->state    != RING_DESC_STATE_FREE) &&
        (current->function != NULL))
    {
      // Before freeing a descriptor call related handler to complete all incomplete submissions
      current->function(current, NULL, RING_REASON_RELEASED);
    }
    free(current);
  }
}

static inline __attribute__((always_inline)) struct FastRingDescriptor* AllocateRingDescriptor(struct FastRingDescriptorSet* set)
{
  void* pointer;
  struct FastRingDescriptor* descriptor;

  do pointer = atomic_load_explicit(&set->available, memory_order_acquire);
  while ((descriptor = REMOVE_ABA_TAG(struct FastRingDescriptor, pointer, RING_DESC_ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&set->available, &pointer, descriptor->next, memory_order_acquire, memory_order_relaxed)));

  if (unlikely(((descriptor == NULL) ||
                (descriptor->state != RING_DESC_STATE_FREE)) &&
               (descriptor = (struct FastRingDescriptor*)memalign(RING_DESC_ALIGNMENT, sizeof(struct FastRingDescriptor)))))
  {
    memset(descriptor, 0, sizeof(struct FastRingDescriptor));
    do descriptor->heap = atomic_load_explicit(&set->heap, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&set->heap, &descriptor->heap, descriptor, memory_order_release, memory_order_relaxed));
  }

  return descriptor;
}

static inline __attribute__((always_inline)) void ReleaseRingDescriptor(struct FastRingDescriptorSet* set, struct FastRingDescriptor* descriptor)
{
  uint32_t tag;

  tag = atomic_fetch_add_explicit(&descriptor->tag, 1, memory_order_relaxed) + 1;

  do descriptor->next = atomic_load_explicit(&set->available, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&set->available, &descriptor->next, ADD_ABA_TAG(descriptor, tag, RING_DESC_ALIGNMENT), memory_order_release, memory_order_relaxed));
}

static inline __attribute__((always_inline)) void SubmitRingDescriptorRange(struct FastRingDescriptorSet* set, struct FastRingDescriptor* first, struct FastRingDescriptor* last)
{
  struct FastRingDescriptor* descriptor;

  atomic_store_explicit(&last->next, NULL, memory_order_relaxed);
  descriptor = atomic_exchange_explicit(&set->pending, last, memory_order_relaxed);
  atomic_store_explicit(&descriptor->next, first, memory_order_release);
}

static inline __attribute__((always_inline)) void PrepareRingDescriptor(struct FastRingDescriptor* descriptor, int option)
{
  uint32_t tag;

  tag                               = atomic_load_explicit(&descriptor->tag, memory_order_relaxed);
  descriptor->identifier            = (uint64_t)descriptor | ((uint64_t)tag | (uint64_t)tag << 42) & RING_DESC_INTEGRITY_MASK;
  descriptor->submission.user_data  = (uint64_t)descriptor->identifier | (uint64_t)(option & RING_DESC_OPTION_MASK);

  atomic_store_explicit(&descriptor->state, RING_DESC_STATE_PENDING, memory_order_release);
}

static inline __attribute__((always_inline)) int LockPendingRingDescriptor(struct FastRingDescriptor* descriptor)
{
  uint32_t state;

  state = RING_DESC_STATE_PENDING;

  return atomic_compare_exchange_strong_explicit(&descriptor->state, &state, RING_DESC_STATE_LOCKED, memory_order_acquire, memory_order_relaxed);
}

static inline __attribute__((always_inline)) int LockSubmittedRingDescriptor(struct FastRingDescriptor* descriptor)
{
  uint32_t state;

  state = RING_DESC_STATE_SUBMITTED;

  while (!atomic_compare_exchange_strong_explicit(&descriptor->state, &state, RING_DESC_STATE_LOCKED, memory_order_acquire, memory_order_relaxed))
  {
    if (state != RING_DESC_STATE_LOCKED)
    {
      // Unexpected state of descriptor
      return 0;
    }

#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    __asm__ __volatile__("" ::: "memory");
#endif

    state = RING_DESC_STATE_SUBMITTED;
  }

  return 1;
}

static inline __attribute__((hot)) void HandleCompletedRingDescriptor(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  __builtin_prefetch(&descriptor->state);

  if (unlikely(ring->trace.function != NULL))
  {
    // Trace is only for debug purposes, less probable it is in use
    ring->trace.function(RING_TRACE_ACTION_HANDLE, descriptor, completion, reason, ring->trace.closure);
  }

  if (unlikely((completion != NULL) &&
               ((completion->user_data & ~RING_DESC_OPTION_MASK) != descriptor->identifier)))
  {
    // Leaked descriptor: someone was not in good mood and forgot to solve some cases
    // Don't touch the descriptor, it could be still in use somewhere else
    return;
  }

  if (likely(((descriptor->function == NULL) &&
              (( completion == NULL) ||
               (~completion->flags & IORING_CQE_F_MORE)) ||
              (descriptor->function != NULL) &&
              (descriptor->function(descriptor, completion, reason) == 0)) &&
             (atomic_fetch_sub_explicit(&descriptor->references, 1, memory_order_relaxed) == 1)))
  {
    if (unlikely(ring->trace.function != NULL))
    {
      // Trace is only for debug purposes, less probable it is in use
      ring->trace.function(RING_TRACE_ACTION_RELEASE, descriptor, completion, reason, ring->trace.closure);
    }

    if (unlikely((descriptor->next           != NULL) &&
                 (descriptor->next->previous == descriptor)))
    {
      // This case is useful to handle chains with IOSQE_CQE_SKIP_SUCCESS
      descriptor->next->previous = NULL;
    }

    descriptor->closure    = NULL;
    descriptor->function   = NULL;
    descriptor->previous   = NULL;
    descriptor->identifier = 0ULL;

    atomic_store_explicit(&descriptor->state, RING_DESC_STATE_FREE, memory_order_release);
    ReleaseRingDescriptor(&ring->descriptors, descriptor);
  }
}

int __attribute__((hot)) WaitForFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask)
{
  int result;
  unsigned position;
  struct io_uring_cqe* completion;
  struct io_uring_sqe* submission;
  struct __kernel_timespec timeout;

  uint32_t state;
  void* condition;
  struct FastRingFlusher* flusher;
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
    if (likely((result == 0) &&
               (io_uring_cq_ready(&ring->ring) != 0)))
    {
      // It seems like io_uring clears IORING_SQ_CQ_OVERFLOW only on call io_uring_submit_and_wait_timeout()
      goto Handle;
    }
  }

  // Submit pending SQEs

  if (likely((descriptor = atomic_load_explicit(&ring->descriptors.pending, memory_order_relaxed))    &&
             (atomic_load_explicit(&descriptor->next,  memory_order_acquire) == NULL)                 &&
             (atomic_load_explicit(&descriptor->state, memory_order_relaxed) != RING_DESC_STATE_FREE) &&
             (descriptor = AllocateRingDescriptor(&ring->descriptors))))
  {
    // Submit stub descriptor to force submission of last valuable descriptor
    SubmitRingDescriptorRange(&ring->descriptors, descriptor, descriptor);
  }

  condition = NULL;

  while ((descriptor = ring->descriptors.submitting) &&
         (condition  = atomic_load_explicit(&descriptor->next, memory_order_acquire)))
  {
    if (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_FREE)
    {
      ring->descriptors.submitting = atomic_load_explicit(&descriptor->next, memory_order_relaxed);
      ReleaseRingDescriptor(&ring->descriptors, descriptor);
      continue;
    }

    state = RING_DESC_STATE_PENDING;

    __builtin_prefetch(&descriptor->state);
    __builtin_prefetch(&descriptor->submission);
    __builtin_prefetch((uint8_t*)(&descriptor->submission) + sizeof(struct io_uring_sqe));

    if (likely(((descriptor->linked == 0) ||
                (descriptor->linked < io_uring_sq_space_left(&ring->ring))) &&
                (atomic_compare_exchange_strong_explicit(&descriptor->state, &state, RING_DESC_STATE_LOCKED, memory_order_acquire, memory_order_relaxed))))
    {
      submission = io_uring_get_sqe(&ring->ring);

      if (submission == NULL)
      {
        atomic_store_explicit(&descriptor->state, state, memory_order_release);
        break;
      }

      ring->descriptors.submitting = atomic_load_explicit(&descriptor->next, memory_order_relaxed);

      switch (descriptor->length)
      {
        case sizeof(struct io_uring_sqe):       __builtin_memcpy(submission, &descriptor->submission, sizeof(struct io_uring_sqe));       break;
        case sizeof(struct io_uring_sqe) + 64:  __builtin_memcpy(submission, &descriptor->submission, sizeof(struct io_uring_sqe) + 64);  break;
        default:                                          memcpy(submission, &descriptor->submission, descriptor->length);                break;
      }

      atomic_store_explicit(&descriptor->state, RING_DESC_STATE_SUBMITTED, memory_order_release);
      continue;
    }

    break;
  }

  // Submit SQEs and handle CQEs without waiting when at least one pending SQE or CQE exists

  if (likely((condition != NULL) &&
             (io_uring_sq_ready(&ring->ring) > 0) ||
             (io_uring_cq_ready(&ring->ring) > 0)))
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

    // Advance is not so expensive, it is better to release a CQE ASAP
    io_uring_cq_advance(&ring->ring, 1);
  }

  // Call flushers

  while (flusher = PopRingFlusher(&ring->flushers.pending))
  {
    if (atomic_exchange_explicit(&flusher->state, RING_FLUSH_STATE_LOCKED, memory_order_acquire) == RING_FLUSH_STATE_PENDING)
    {
      // Flusher might be canceled by setting state to RING_FLUSH_STATE_FREE
      flusher->function(flusher->closure, RING_REASON_COMPLETE);
    }

    atomic_store_explicit(&flusher->state, RING_FLUSH_STATE_FREE, memory_order_relaxed);
    PushRingFlusher(&ring->flushers.available, flusher, 0);
  }

  return result * (result != -ETIME);
}

void __attribute__((hot)) PrepareFastRingDescriptor(struct FastRingDescriptor* descriptor, int option)
{
  PrepareRingDescriptor(descriptor, option);
}

void __attribute__((hot)) SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option)
{
  struct FastRing* ring;

  ring = descriptor->ring;

  PrepareRingDescriptor(descriptor, option);
  SubmitRingDescriptorRange(&ring->descriptors, descriptor, descriptor);
}

void __attribute__((hot)) SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last)
{
  struct FastRing* ring;

  ring = first->ring;

  SubmitRingDescriptorRange(&ring->descriptors, first, last);
}

struct FastRingDescriptor* __attribute__((hot)) AllocateFastRingDescriptor(struct FastRing* ring, HandleFastRingCompletionFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  descriptor = NULL;

  if (likely((ring != NULL) &&
             (atomic_load_explicit(&ring->descriptors.heap, memory_order_relaxed)) &&
             (descriptor = AllocateRingDescriptor(&ring->descriptors))))
  {
    descriptor->ring     = ring;
    descriptor->length   = sizeof(struct io_uring_sqe);
    descriptor->closure  = closure;
    descriptor->function = function;
    descriptor->previous = NULL;
    descriptor->next     = NULL;
    descriptor->linked   = 0;

    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_nop(&descriptor->submission);
    atomic_store_explicit(&descriptor->references, 1, memory_order_relaxed);
    atomic_store_explicit(&descriptor->state, RING_DESC_STATE_ALLOCATED, memory_order_relaxed);
  }

  return descriptor;
}

int __attribute__((hot)) ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor)
{
  struct FastRing* ring;
  int count;

  count = -1;

  if (likely(descriptor != NULL))
  {
    count = atomic_fetch_sub_explicit(&descriptor->references, 1, memory_order_relaxed) - 1;

    if (likely(count == 0))
    {
      ring = descriptor->ring;

      descriptor->closure    = NULL;
      descriptor->function   = NULL;
      descriptor->previous   = NULL;
      descriptor->identifier = 0ULL;

      atomic_store_explicit(&descriptor->state, RING_DESC_STATE_FREE, memory_order_release);
      ReleaseRingDescriptor(&ring->descriptors, descriptor);
    }
  }

  return count;
}

struct FastRingFlusher* __attribute__((hot)) SetFastRingFlushHandler(struct FastRing* ring, HandleFastRingFlushFunction function, void* closure)
{
  struct FastRingFlusher* flusher;

  flusher = NULL;

  if (likely((ring != NULL) &&
             ((flusher = PopRingFlusher(&ring->flushers.available)) ||
              (flusher = (struct FastRingFlusher*)memalign(RING_FLUSH_ALIGNMENT, sizeof(struct FastRingFlusher))))))
  {
    flusher->function = function;
    flusher->closure  = closure;

    atomic_store_explicit(&flusher->state, RING_FLUSH_STATE_PENDING, memory_order_release);
    PushRingFlusher(&ring->flushers.pending, flusher, 1);
  }

  return flusher;
}

int __attribute__((hot)) RemoveFastRingFlushHandler(struct FastRing* ring, struct FastRingFlusher* flusher)
{
  uint32_t state;

  if (likely((flusher != NULL) &&
             (ring    != NULL)))
  {
    state = RING_FLUSH_STATE_PENDING;
    return atomic_compare_exchange_strong_explicit(&flusher->state, &state, RING_FLUSH_STATE_FREE, memory_order_relaxed, memory_order_relaxed) - 1;  // 0 or -EPERM
  }

  return -EBADF;
}

uint16_t GetFastRingBufferGroup(struct FastRing* ring)
{
  return atomic_fetch_add_explicit(&ring->groups, 1, memory_order_relaxed) + 1;
}

int IsFastRingThread(struct FastRing* ring)
{
  static __thread pid_t thread = 0;

  if (unlikely(thread       == 0))  thread = gettid();
  if (unlikely(ring->thread == 0))  return -1;

  return ring->thread == thread;
}

struct FastRing* CreateFastRing(uint32_t length)
{
  struct FastRing* ring;
  struct utsname name;
  struct rlimit limit;
  pthread_mutexattr_t attribute;

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

#ifdef IORING_ENTER_NO_IOWAIT
    io_uring_set_iowait(&ring->ring, 0);
#endif

    ring->limit = limit.rlim_cur / FILE_REGISTRATION_RATIO;
    ring->limit = ((ring->limit == 0) || (ring->limit > FILE_MAXIMUM_COUNT)) ? FILE_MAXIMUM_COUNT : ring->limit;

    io_uring_register_files_sparse(&ring->ring, ring->limit);
    io_uring_register_file_alloc_range(&ring->ring, ring->limit / 2, ring->limit / 2);

    pthread_mutexattr_init(&attribute);
    pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ring->buffers.lock, &attribute);
    pthread_mutex_init(&ring->files.lock, &attribute);
    pthread_mutexattr_destroy(&attribute);

    ring->probe                  = io_uring_get_probe_ring(&ring->ring);
    ring->thread                 = gettid();
    ring->descriptors.submitting = AllocateRingDescriptor(&ring->descriptors);

    atomic_store_explicit(&ring->descriptors.pending, ring->descriptors.submitting, memory_order_release);
  }

  return ring;
}

void ReleaseFastRing(struct FastRing* ring)
{
  if (ring != NULL)
  {
    ReleaseRingFlusherStack(&ring->flushers.pending);
    ReleaseRingFlusherStack(&ring->flushers.available);
    ReleaseRingDescriptorHeap(&ring->descriptors);
    pthread_mutex_destroy(&ring->buffers.lock);
    pthread_mutex_destroy(&ring->files.lock);
    io_uring_free_probe(ring->probe);
    io_uring_queue_exit(&ring->ring);
    free(ring->buffers.vectors);
    free(ring->files.entries);
    free(ring->files.filters);
    free(ring);
  }
}

// Poll

static int __attribute__((hot)) HandlePollEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  uint32_t condition;

  if (likely(( completion      != NULL) &&
             ( completion->res >= 0)    &&
             (~completion->user_data & RING_DESC_OPTION_IGNORE)))
  {
    do condition = atomic_load_explicit(&descriptor->data.poll.condition, memory_order_relaxed);
    while (!(atomic_compare_exchange_weak_explicit(&descriptor->data.poll.condition, &condition,
              condition & RING_CONDITION_MASK | completion->flags & IORING_CQE_F_MORE | RING_CONDITION_GUARD,
              memory_order_release, memory_order_relaxed)));

    if (likely(~condition & RING_CONDITION_REMOVE))
    {
      //
      descriptor->data.poll.function(descriptor->data.poll.handle, completion->res, descriptor->closure, descriptor->data.poll.flags);
    }

    atomic_fetch_and_explicit(&descriptor->data.poll.condition, ~RING_CONDITION_GUARD, memory_order_release);
  }

  return (completion != NULL) && (completion->flags & IORING_CQE_F_MORE);
}

int AddFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  if (likely((handle >= 0) &&
             (ring   != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if (likely((handle < ring->files.length) ||
               (ExpandRingFileList(&ring->files, handle) != NULL)) &&
               (descriptor = AllocateFastRingDescriptor(ring, HandlePollEvent, closure)))
    {
      ring->files.entries[handle].tag        = atomic_load_explicit(&descriptor->tag, memory_order_relaxed);
      ring->files.entries[handle].descriptor = descriptor;

      pthread_mutex_unlock(&ring->files.lock);

      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_poll_add(&descriptor->submission, handle, flags);
      descriptor->submission.len     = RING_POLL_FLAGS(flags);
      descriptor->data.poll.function = function;
      descriptor->data.poll.handle   = handle;
      descriptor->data.poll.flags    = flags;
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      atomic_store_explicit(&descriptor->data.poll.condition, 0, memory_order_relaxed);
      SubmitFastRingDescriptor(descriptor, 0);
      return 0;
    }

    pthread_mutex_unlock(&ring->files.lock);
  }

  return -ENOMEM;
}

int ModifyFastRingPoll(struct FastRing* ring, int handle, uint64_t flags)
{
  struct FastRingDescriptor* descriptor;
  uint32_t condition;

  if (likely((handle >= 0) &&
             (ring != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if (likely((handle < ring->files.length) &&
               (descriptor = ring->files.entries[handle].descriptor) &&
               (atomic_load_explicit(&descriptor->tag, memory_order_acquire) == ring->files.entries[handle].tag)))
    {
      pthread_mutex_unlock(&ring->files.lock);

      descriptor->data.poll.flags = flags;
      condition = atomic_load_explicit(&descriptor->data.poll.condition, memory_order_relaxed);

      if (unlikely(LockPendingRingDescriptor(descriptor)))
      {
        if (unlikely(descriptor->submission.opcode == IORING_OP_POLL_ADD))
        {
          descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
          descriptor->submission.len           = RING_POLL_FLAGS(flags);
          atomic_store_explicit(&descriptor->state, RING_DESC_STATE_PENDING, memory_order_release);
          return 0;
        }

        descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
        descriptor->submission.len           = IORING_POLL_UPDATE_USER_DATA | IORING_POLL_UPDATE_EVENTS | RING_POLL_FLAGS(flags);
        atomic_store_explicit(&descriptor->state, RING_DESC_STATE_PENDING, memory_order_release);
        return 0;
      }

      if (unlikely((~condition & IORING_CQE_F_MORE) ||
                   (atomic_load_explicit(&descriptor->references, memory_order_relaxed) == 1)))
      {
        if (likely(LockSubmittedRingDescriptor(descriptor)))
        {
          io_uring_initialize_sqe(&descriptor->submission);
          io_uring_prep_poll_add(&descriptor->submission, handle, flags);
          descriptor->submission.len = RING_POLL_FLAGS(flags);
          atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
          SubmitFastRingDescriptor(descriptor, 0);
          return 0;
        }

        return -EBUSY;
      }

      if (likely(LockSubmittedRingDescriptor(descriptor)))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_poll_update(&descriptor->submission, descriptor->identifier, descriptor->identifier, flags, IORING_POLL_UPDATE_USER_DATA | IORING_POLL_UPDATE_EVENTS | RING_POLL_FLAGS(flags));
        atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
        SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
        return 0;
      }

      return -EBUSY;
    }

    pthread_mutex_unlock(&ring->files.lock);
  }

  return -EBADF;
}

int RemoveFastRingPoll(struct FastRing* ring, int handle)
{
  struct FastRingDescriptor* descriptor;

  if (likely((handle >= 0) &&
             (ring   != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if (likely((handle < ring->files.length) &&
               (descriptor = ring->files.entries[handle].descriptor) &&
               (atomic_load_explicit(&descriptor->tag, memory_order_acquire) == ring->files.entries[handle].tag)))
    {
      atomic_fetch_or_explicit(&descriptor->data.poll.condition, RING_CONDITION_REMOVE, memory_order_release);
      ring->files.entries[handle].descriptor = NULL;
      ring->files.entries[handle].tag        = 0;

      pthread_mutex_unlock(&ring->files.lock);

      if (ReleaseFastRingDescriptor(descriptor) == 0)
      {
        // Descriptor has already been released by HandlePollEvent()
        return 0;
      }

      if (unlikely(LockPendingRingDescriptor(descriptor)))
      {
        if (unlikely(descriptor->submission.opcode == IORING_OP_POLL_ADD))
        {
          io_uring_initialize_sqe(&descriptor->submission);
          io_uring_prep_nop(&descriptor->submission);
          PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
          return 0;
        }

        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_poll_remove(&descriptor->submission, descriptor->identifier);
        PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
        return 0;
      }

      if (likely(LockSubmittedRingDescriptor(descriptor)))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_poll_remove(&descriptor->submission, descriptor->identifier);
        atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
        SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
        return 0;
      }

      return -EBADF;
    }

    pthread_mutex_unlock(&ring->files.lock);
  }

  return -EBADF;
}

void DestroyFastRingPoll(struct FastRing* ring, HandleFastRingPollFunction function, void* closure)
{
  struct FastRingFileEntry* entry;
  struct FastRingFileEntry* limit;
  struct FastRingDescriptor* descriptor;

  if (likely(ring != NULL))
  {
    pthread_mutex_lock(&ring->files.lock);

    entry = ring->files.entries;
    limit = ring->files.entries + ring->files.length;

    while (entry < limit)
    {
      if (unlikely((descriptor = entry->descriptor) &&
                   (atomic_load_explicit(&descriptor->tag, memory_order_acquire) == entry->tag) &&
                   (descriptor->data.poll.function == function) &&
                   (descriptor->closure            == closure)))
      {
        // Remove handler and submit cancel request
        RemoveFastRingPoll(ring, descriptor->data.poll.handle);
      }

      entry ++;
    }

    pthread_mutex_unlock(&ring->files.lock);
  }
}

int ManageFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandleFastRingPollFunction function, void* closure)
{
  int result;

  result = (flags == 0ULL) ?
    RemoveFastRingPoll(ring, handle) :
    ModifyFastRingPoll(ring, handle, flags);

  result = (result == -EBADF) && (flags != 0ULL) ?
    AddFastRingPoll(ring, handle, flags, function, closure) :
    result;

  return result;
}

struct FastRingDescriptor* GetFastRingPollDescriptor(struct FastRing* ring, int handle)
{
  struct FastRingDescriptor* descriptor;

  descriptor = NULL;

  if (likely((handle >= 0) &&
             (ring   != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if ((handle < ring->files.length) &&
        (descriptor = ring->files.entries[handle].descriptor) &&
        (atomic_load_explicit(&descriptor->tag, memory_order_acquire) != ring->files.entries[handle].tag))
    {
      // Existing descriptor could change purpose and ownership
      descriptor = NULL;
    }

    pthread_mutex_unlock(&ring->files.lock);
  }

  return descriptor;
}

// Watch

static int __attribute__((hot)) HandleWatchEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if (likely(( completion != NULL) &&
             (~completion->user_data & RING_DESC_OPTION_IGNORE)))
  {
    if (likely(~atomic_fetch_or_explicit(&descriptor->data.watch.condition, RING_CONDITION_GUARD, memory_order_release) & RING_CONDITION_REMOVE))
    {
      //
      descriptor->data.watch.function(descriptor, completion->res);
    }

    if (likely(~atomic_fetch_and_explicit(&descriptor->data.watch.condition, ~RING_CONDITION_GUARD, memory_order_acq_rel) & RING_CONDITION_REMOVE))
    {
      if (likely(LockPendingRingDescriptor(descriptor)))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_poll_add(&descriptor->submission, descriptor->data.watch.handle, descriptor->data.watch.flags);
        PrepareFastRingDescriptor(descriptor, 0);
        return 1;
      }

      if (likely(LockSubmittedRingDescriptor(descriptor)))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_poll_add(&descriptor->submission, descriptor->data.watch.handle, descriptor->data.watch.flags);
        SubmitFastRingDescriptor(descriptor, 0);
        return 1;
      }
    }
  }

  return 0;
}

struct FastRingDescriptor* AddFastRingWatch(struct FastRing* ring, int handle, uint32_t flags, HandleFastRingWatchFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  if (likely((handle   >= 0) &&
             (flags    != 0) &&
             (ring     != NULL) &&
             (function != NULL) &&
             (descriptor = AllocateFastRingDescriptor(ring, HandleWatchEvent, closure))))
  {
    descriptor->data.watch.flags    = flags;
    descriptor->data.watch.handle   = handle;
    descriptor->data.watch.function = function;
    atomic_store_explicit(&descriptor->data.watch.condition, 0, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_poll_add(&descriptor->submission, handle, flags);
    SubmitFastRingDescriptor(descriptor, 0);
    return descriptor;
  }

  return NULL;
}

void UpdateFastRingWatch(struct FastRingDescriptor* descriptor, uint32_t flags)
{
  if (likely((flags != 0) &&
             (descriptor != NULL) &&
             (descriptor->data.watch.flags != flags)))
  {
    descriptor->data.watch.flags = flags;

    if (atomic_load_explicit(&descriptor->data.watch.condition, memory_order_acquire) & RING_CONDITION_GUARD)
    {
      // We are inside a call to HandleWatchEvent()
      return;
    }

    if (likely(LockPendingRingDescriptor(descriptor)))
    {
      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
      atomic_store_explicit(&descriptor->state, RING_DESC_STATE_PENDING, memory_order_release);
      return;
    }

    if (likely(LockSubmittedRingDescriptor(descriptor)))
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_poll_update(&descriptor->submission, descriptor->identifier, descriptor->identifier, flags, IORING_POLL_UPDATE_USER_DATA | IORING_POLL_UPDATE_EVENTS);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      return;
    }
  }
}

void RemoveFastRingWatch(struct FastRingDescriptor* descriptor)
{
  if (likely((descriptor != NULL) &&
             (~atomic_fetch_or_explicit(&descriptor->data.watch.condition, RING_CONDITION_REMOVE, memory_order_acq_rel) & RING_CONDITION_GUARD)))
  {
    if (LockPendingRingDescriptor(descriptor))
    {
      if (descriptor->submission.opcode == IORING_OP_POLL_ADD)
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_nop(&descriptor->submission);
      }
      else
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      }

      PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      return;
    }

    if (LockSubmittedRingDescriptor(descriptor))
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    }
  }
}

struct FastRingDescriptor* SetFastRingWatch(struct FastRing* ring, struct FastRingDescriptor* descriptor, int handle, uint32_t flags, HandleFastRingWatchFunction function, void* closure)
{
  if ((flags      == 0) &&
      (descriptor != NULL))
  {
    RemoveFastRingWatch(descriptor);
    return NULL;
  }

  if ((flags      != 0) &&
      (descriptor != NULL))
  {
    UpdateFastRingWatch(descriptor, flags);
    return descriptor;
  }

  return AddFastRingWatch(ring, handle, flags, function, closure);
}

// Timeout

static int __attribute__((hot)) HandleTimeoutEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  uint32_t condition;

  if (likely((completion != NULL) &&
             (completion->res == -ETIME)))
  {
    do condition = atomic_load_explicit(&descriptor->data.timeout.condition, memory_order_relaxed);
    while (!(atomic_compare_exchange_weak_explicit(&descriptor->data.timeout.condition, &condition,
              condition & RING_CONDITION_MASK | completion->flags & IORING_CQE_F_MORE | RING_CONDITION_GUARD,
              memory_order_release, memory_order_relaxed)));

    if (likely(~condition & RING_CONDITION_REMOVE))
    {
      //
      descriptor->data.timeout.function(descriptor);
    }

    if (unlikely((~atomic_fetch_and_explicit(&descriptor->data.timeout.condition, ~RING_CONDITION_GUARD, memory_order_acq_rel) & RING_CONDITION_REMOVE) &&
                 (descriptor->data.timeout.flags & TIMEOUT_FLAG_REPEAT) &&
                 (~completion->flags & IORING_CQE_F_MORE)))
    {
      if (likely(LockSubmittedRingDescriptor(descriptor)))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, descriptor->data.timeout.flags);
        SubmitFastRingDescriptor(descriptor, 0);
      }

      return 1;
    }
  }

  return (completion != NULL) && (completion->flags & IORING_CQE_F_MORE);
}

static void CreateTimeout(struct FastRingDescriptor* descriptor, HandleFastRingTimeoutFunction function, void* closure, uint64_t flags)
{
  descriptor->data.timeout.flags    = flags;
  descriptor->data.timeout.function = function;
  descriptor->closure               = closure;

  atomic_store_explicit(&descriptor->data.timeout.condition, 0, memory_order_relaxed);
  io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, flags);
  SubmitFastRingDescriptor(descriptor, 0);
}

static void UpdateTimeout(struct FastRingDescriptor* descriptor)
{
  uint32_t condition;

  condition = atomic_load_explicit(&descriptor->data.timeout.condition, memory_order_acquire);

  if (likely(((~condition & RING_CONDITION_GUARD) ||
              ( condition & IORING_CQE_F_MORE))   &&
             (LockSubmittedRingDescriptor(descriptor))))
  {
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_timeout_update(&descriptor->submission, &descriptor->data.timeout.interval, descriptor->identifier, 0);
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

static void RemoveTimeout(struct FastRingDescriptor* descriptor)
{
  uint32_t condition;

  condition = atomic_fetch_or_explicit(&descriptor->data.timeout.condition, RING_CONDITION_REMOVE, memory_order_acq_rel);

  if (likely((~condition & RING_CONDITION_GUARD) ||
             ( condition & IORING_CQE_F_MORE)))
  {
    if (unlikely(LockPendingRingDescriptor(descriptor)))
    {
      if (unlikely(descriptor->submission.opcode == IORING_OP_TIMEOUT))
      {
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_nop(&descriptor->submission);
        PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
        return;
      }

      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_timeout_remove(&descriptor->submission, descriptor->identifier, 0);
      PrepareFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      return;
    }

    if (unlikely(LockSubmittedRingDescriptor(descriptor)))
    {
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_timeout_remove(&descriptor->submission, descriptor->identifier, 0);
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      return;
    }
  }
}

struct FastRingDescriptor* SetFastRingTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, int64_t interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure)
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
    UpdateTimeout(descriptor);
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

struct FastRingDescriptor* SetFastRingCertainTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timeval* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure)
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
    UpdateTimeout(descriptor);
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

struct FastRingDescriptor* SetFastRingPreciseTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timespec* interval, uint64_t flags, HandleFastRingTimeoutFunction function, void* closure)
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
    UpdateTimeout(descriptor);
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

// Event

struct FastRingDescriptor* CreateFastRingEvent(struct FastRing* ring, HandleFastRingCompletionFunction function, void* closure)
{
  struct FastRingDescriptor* descriptor;

  if (descriptor = AllocateFastRingDescriptor(ring, function, closure))
  {
    PrepareFastRingDescriptor(descriptor, 0);
    return descriptor;
  }

  return NULL;
}

int SubmitFastRingEvent(struct FastRing* ring, struct FastRingDescriptor* event, uint32_t parameter, int option)
{
  struct FastRingDescriptor* descriptor;

  if ((event != NULL) &&
      (descriptor = AllocateFastRingDescriptor(ring, NULL, NULL)))
  {
    io_uring_prep_msg_ring(&descriptor->submission, event->ring->ring.ring_fd, parameter, event->identifier | (uint64_t)(option & RING_DESC_OPTION_MASK), 0);
    SubmitFastRingDescriptor(descriptor, 0);
    return 0;
  }

  return -EINVAL;
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

  if (likely((handle >= 0) &&
             (ring   != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if (likely((handle < ring->files.length) ||
               (ExpandRingFileList(&ring->files, handle) != NULL)))
    {
      entry = ring->files.entries + handle;

      if (likely(entry->references > 0))
      {
        entry->references ++;
        pthread_mutex_unlock(&ring->files.lock);
        return entry->index;
      }

      index = 0;
      limit = ring->limit / 2;

      if (unlikely(ring->files.filters == NULL))
      {
        // Upper part is reserved for allocation by kernel, a half will be enough
        ring->files.filters = (uint32_t*)calloc((limit >> 5) + 1, sizeof(uint32_t));

        if (unlikely(ring->files.filters == NULL))
        {
          pthread_mutex_unlock(&ring->files.lock);
          return -ENOMEM;
        }
      }

      while ((index < limit) &&
             (ring->files.filters[index >> 5] == UINT32_MAX))
        index += 32;

      index += __builtin_ffs(~ring->files.filters[index >> 5]) - 1;

      if (unlikely(index >= limit))
      {
        // All registered files are already in use
        pthread_mutex_unlock(&ring->files.lock);
        return -EOVERFLOW;
      }

      result = io_uring_register_files_update(&ring->ring, index, &handle, 1);

      if (likely(result >= 0))
      {
        ring->files.filters[index >> 5] |= 1 << (index & 31);
        result             = index;
        entry->index       = index;
        entry->references ++;
      }
    }

    pthread_mutex_unlock(&ring->files.lock);
  }

  return result;
}

void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle)
{
  struct FastRingFileEntry* entry;

  if (likely((handle >= 0) &&
             (ring  != NULL)))
  {
    pthread_mutex_lock(&ring->files.lock);

    if (likely((handle < ring->files.length)          &&
               (entry = ring->files.entries + handle) &&
               (entry->references > 0)                &&
               ((-- entry->references) == 0)))
    {
      handle = -1;
      ring->files.filters[entry->index >> 5] &= ~(1 << (entry->index & 31));
      io_uring_register_files_update(&ring->ring, entry->index, &handle, 1);
    }

    pthread_mutex_unlock(&ring->files.lock);
  }
}

// Registered Buffer

int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length)
{
  __u64 tag;
  int result;
  struct iovec* vector;

  result = -EINVAL;

  if (likely((address != NULL) &&
             (ring    != NULL)))
  {
    pthread_mutex_lock(&ring->buffers.lock);

    if (unlikely(ring->buffers.vectors == NULL))
    {
      ring->buffers.length  = BUFFER_MAXIMUM_COUNT;
      ring->buffers.vectors = (struct iovec*)calloc(sizeof(struct iovec), ring->buffers.length);

      if (unlikely(ring->buffers.vectors == NULL))
      {
        pthread_mutex_unlock(&ring->buffers.lock);
        return -ENOMEM;
      }

      io_uring_register_buffers_sparse(&ring->ring, ring->buffers.length);
    }

    if (unlikely(ring->buffers.count == ring->buffers.length))
    {
      // All registered files are already in use
      pthread_mutex_unlock(&ring->buffers.lock);
      return -EOVERFLOW;
    }

    while (ring->buffers.vectors[ring->buffers.position].iov_base != NULL)
    {
      ring->buffers.position ++;
      ring->buffers.position &= ring->buffers.length - 1;
    }

    vector           = ring->buffers.vectors + ring->buffers.position;
    vector->iov_base = address;
    vector->iov_len  = length;

    tag    = 0;
    result = io_uring_register_buffers_update_tag(&ring->ring, ring->buffers.position, vector, &tag, 1);

    if (likely(result >= 0))
    {
      ring->buffers.count ++;
      result = ring->buffers.position;
    }
    else
    {
      vector->iov_base = NULL;
      vector->iov_len  = 0;
    }

    pthread_mutex_unlock(&ring->buffers.lock);
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
             (ring  != NULL)))
  {
    pthread_mutex_lock(&ring->buffers.lock);

    vector           = ring->buffers.vectors + index;
    vector->iov_base = address;
    vector->iov_len  = length;

    tag    = 0;
    result = io_uring_register_buffers_update_tag(&ring->ring, index, vector, &tag, 1);

    if (likely(result >= 0))
    {
      result = index;

      if (address == NULL)
      {
        ring->buffers.count --;
        result = INT32_MIN;
      }
    }

    pthread_mutex_unlock(&ring->buffers.lock);
  }

  return result;
}
