#include "ThreadCall.h"

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#define ALIGNMENT  64ULL

// futex(address, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, value, NULL, NULL, FUTEX_BITSET_MATCH_ANY);
// futex(address, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, count, NULL, NULL, FUTEX_BITSET_MATCH_ANY);

static inline int __attribute__((always_inline)) futex(uint32_t* address1, int operation, uint32_t value1, const struct timespec* timeout, uint32_t* address2, uint32_t value2)
{
  return syscall(SYS_futex, address1, operation, value1, timeout, address2, value2);
}

static struct ThreadCallState* GetThreadCallState()
{
  static __thread uint8_t buffer[sizeof(struct ThreadCallState) + ALIGNMENT] = { };

  struct ThreadCallState* state;
  void* pointer;

  pointer = buffer;
  state   = (struct ThreadCallState*)(((uintptr_t)pointer + ALIGNMENT - 1ULL) & (~(ALIGNMENT - 1ULL)));

  return state;
}

static struct ThreadCallState* PeekThreadCallState(struct ThreadCall* call)
{
  void* pointer;
  struct ThreadCallState* state;

  do pointer = atomic_load_explicit(&call->stack, memory_order_acquire);
  while ((state = REMOVE_ABA_TAG(struct ThreadCallState, pointer, ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&call->stack, &pointer, state->next, memory_order_acquire, memory_order_relaxed)));

  return state;
}

static void SubmitThreadCallState(struct ThreadCall* call, struct ThreadCallState* state)
{
  uint64_t count;

  do state->next = atomic_load_explicit(&call->stack, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&call->stack, &state->next, ADD_ABA_TAG(state, state->tag, 0, ALIGNMENT), memory_order_release, memory_order_relaxed));

  if (atomic_fetch_add_explicit(&call->count, 1, memory_order_relaxed) == 0)
  {
#ifdef TC_FEATURE_RING_FUTEX
    if (call->feature == TC_FEATURE_RING_FUTEX)
    {
      while (futex((uint32_t*)&call->count, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0);
      return;
    }
#endif

    if (call->handle >= 0)
    {
      count = 1ULL;
      while ((write(call->handle, &count, sizeof(uint64_t)) <= 0) &&
             (errno != EBADF));
      return;
    }
  }
}

static void MakeInternalThreadCall(struct ThreadCall* call, struct ThreadCallState* state)
{
  if ((call != NULL) &&
      (atomic_load_explicit(&call->weight, memory_order_relaxed) > TC_ROLE_HANDLER))
  {
    if (IsFastRingThread(call->ring) > 0)
    {
      call->function(call->closure, state->arguments);
      atomic_store_explicit(&state->result, TC_RESULT_CALLED, memory_order_relaxed);
      return;
    }

    atomic_store_explicit(&state->result, TC_RESULT_PREPARED, memory_order_release);

    SubmitThreadCallState(call, state);

    while (atomic_load_explicit(&state->result, memory_order_acquire) == TC_RESULT_PREPARED)
    {
      // Try to wait for futex anyway (EAGAIN / EINTR)
      futex((uint32_t*)&state->result, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, TC_RESULT_PREPARED, NULL, NULL, FUTEX_BITSET_MATCH_ANY);
    }

    atomic_fetch_add_explicit(&state->tag, 1, memory_order_relaxed);
  }
}

static int HandleThreadWakeupCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct ThreadCallState* state;

  state = (struct ThreadCallState*)descriptor->closure;

  if (reason != RING_REASON_COMPLETE)
  {
    while ((descriptor->data.number == atomic_load_explicit(&state->tag, memory_order_relaxed)) &&
           (futex((uint32_t*)&state->result, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0));
    return 0;
  }

  if ((completion              != NULL) &&
      (completion->res         <  0)    &&
      (descriptor->data.number == atomic_load_explicit(&state->tag, memory_order_relaxed)))
  {
    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

static void PostThreadCallResult(struct ThreadCall* call, struct ThreadCallState* state, int result, int wake)
{
  uint32_t tag;
  struct FastRingDescriptor* descriptor;

  tag = atomic_load_explicit(&state->tag, memory_order_relaxed);
  atomic_store_explicit(&state->result, result, memory_order_release);

#ifdef TC_FEATURE_RING_FUTEX
  if ((wake          == TC_WAKE_LAZY)          &&
      (call->feature == TC_FEATURE_RING_FUTEX) &&
      (descriptor     = AllocateFastRingDescriptor(call->ring, HandleThreadWakeupCompletion, state)))
  {
    if (tag == atomic_load_explicit(&state->tag, memory_order_relaxed))
    {
      descriptor->data.number = tag;
      io_uring_prep_futex_wake(&descriptor->submission, (uint32_t*)&state->result, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
      SubmitFastRingDescriptor(descriptor, 0);
      return;
    }

    ReleaseFastRingDescriptor(descriptor);
    return;
  }
#endif

  while ((tag == atomic_load_explicit(&state->tag, memory_order_relaxed)) &&
         (futex((uint32_t*)&state->result, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0));
}

static int HandleThreadCallCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct ThreadCall* call;
  struct ThreadCallState* state;

  if (call = (struct ThreadCall*)descriptor->closure)
  {
    while ((atomic_load_explicit(&call->count, memory_order_relaxed) > 0) &&
           (state = PeekThreadCallState(call)))
    {
      atomic_fetch_sub_explicit(&call->count, 1, memory_order_relaxed);
      call->function(call->closure, state->arguments);
      PostThreadCallResult(call, state, TC_RESULT_CALLED, TC_WAKE_LAZY);
    }

    if (completion != NULL)
    {
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }
  }

  return 0;
}

struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure)
{
  struct ThreadCall* call;
  struct FastRingDescriptor* descriptor;

  call       = (struct ThreadCall*)calloc(1, sizeof(struct ThreadCall));
  descriptor = AllocateFastRingDescriptor(ring, HandleThreadCallCompletion, call);

  if ((call       == NULL) ||
      (descriptor == NULL))
  {
    ReleaseFastRingDescriptor(descriptor);
    free(call);
    return NULL;
  }

  atomic_init(&call->weight, TC_ROLE_HANDLER);

  call->descriptor = descriptor;
  call->handle     = -1;
  call->index      = -1;
  call->function   = function;
  call->closure    = closure;
  call->ring       = ring;

#ifdef TC_FEATURE_RING_FUTEX
  call->feature = io_uring_opcode_supported(ring->probe, IORING_OP_FUTEX_WAIT);

  if (call->feature == TC_FEATURE_RING_FUTEX)
  {
    io_uring_prep_futex_wait(&descriptor->submission, (uint32_t*)&call->count, 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
    SubmitFastRingDescriptor(descriptor, 0);
    return call;
  }
#endif

  call->handle = eventfd(0, EFD_CLOEXEC);
  call->index  = AddFastRingRegisteredFile(ring, call->handle);

  if (call->index >= 0)
  {
    io_uring_prep_read(&descriptor->submission, call->index, &descriptor->data.number, sizeof(uint64_t), 0);
    io_uring_sqe_set_flags(&descriptor->submission, IOSQE_FIXED_FILE);
    SubmitFastRingDescriptor(descriptor, 0);
    return call;
  }

  io_uring_prep_read(&descriptor->submission, call->handle, &descriptor->data.number, sizeof(uint64_t), 0);
  SubmitFastRingDescriptor(descriptor, 0);
  return call;
}

struct ThreadCall* HoldThreadCall(struct ThreadCall* call)
{
  if (call != NULL)
  {
    atomic_fetch_add_explicit(&call->weight, TC_ROLE_CALLER, memory_order_relaxed);
    return call;
  }

  return NULL;
}

void ReleaseThreadCall(struct ThreadCall* call, int role)
{
  int weight;
  uint32_t tag;
  struct ThreadCallState* state;
  struct FastRingDescriptor* descriptor;

  if (call != NULL)
  {
    weight = atomic_fetch_sub_explicit(&call->weight, role, memory_order_relaxed) - role;

    if (role == TC_ROLE_HANDLER)
    {
      while (state = PeekThreadCallState(call))
      {
        tag = atomic_load_explicit(&state->tag, memory_order_relaxed);
        atomic_store_explicit(&state->result, TC_RESULT_CANCELED, memory_order_release);

        while ((tag == atomic_load_explicit(&state->tag, memory_order_relaxed)) &&
               (futex((uint32_t*)&state->result, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY) < 0));
      }

      if (descriptor = call->descriptor)
      {
        atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
        io_uring_initialize_sqe(&descriptor->submission);
        io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
        SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);

        descriptor->function = NULL;
        descriptor->closure  = NULL;
        call->descriptor     = NULL;
      }

      RemoveFastRingRegisteredFile(call->ring, call->handle);
    }

    if (weight == 0)
    {
      close(call->handle);
      free(call);
    }
  }
}

void FreeThreadCall(void* closure)
{
  ReleaseThreadCall((struct ThreadCall*)closure, TC_ROLE_CALLER);
}

int MakeVariadicThreadCall(struct ThreadCall* call, va_list arguments)
{
  struct ThreadCallState* state;

  state = GetThreadCallState();

  va_copy(state->arguments, arguments);
  MakeInternalThreadCall(call, state);
  va_end(state->arguments);

  return atomic_load_explicit(&state->result, memory_order_acquire);
}

int MakeThreadCall(struct ThreadCall* call, ...)
{
  struct ThreadCallState* state;

  state = GetThreadCallState();

  va_start(state->arguments, call);
  MakeInternalThreadCall(call, state);
  va_end(state->arguments);

  return atomic_load_explicit(&state->result, memory_order_acquire);
}

int GetThreadCallWeight(struct ThreadCall* call)
{
  return atomic_load_explicit(&call->weight, memory_order_relaxed);
}
