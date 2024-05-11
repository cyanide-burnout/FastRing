#include "ThreadCall.h"

#include <malloc.h>
#include <alloca.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#define ALIGNMENT  64ULL

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define NOTCH(pointer)  (uint32_t*)((uintptr_t)pointer + sizeof(uintptr_t) - sizeof(uint32_t))
#else
#define NOTCH(pointer)  (uint32_t*)pointer
#endif

#define AllocateThreadCallState()  GetThreadCallState(alloca(sizeof(struct ThreadCallState) + ALIGNMENT))

static inline int __attribute__((always_inline)) futex(uint32_t* address1, int operation, uint32_t value1, const struct timespec* timeout, uint32_t* address2, uint32_t value2)
{
  return syscall(SYS_futex, address1, operation, value1, timeout, address2, value2);
}

static inline struct ThreadCallState* __attribute__((always_inline)) GetThreadCallState(void* pointer)
{
  struct ThreadCallState* state;

  state = (struct ThreadCallState*)(((uintptr_t)pointer + ALIGNMENT - 1ULL) & (~(ALIGNMENT - 1ULL)));
  memset(state, 0, sizeof(struct ThreadCallState));

  return state;
}

static inline struct ThreadCallState* __attribute__((always_inline)) PeekThreadCallState(struct ThreadCall* call)
{
  void* _Atomic pointer;
  struct ThreadCallState* state;

  do pointer = atomic_load_explicit(&call->stack, memory_order_acquire);
  while ((state = REMOVE_ABA_TAG(struct ThreadCallState, pointer, ALIGNMENT)) &&
         (!atomic_compare_exchange_weak_explicit(&call->stack, &pointer, state->next, memory_order_relaxed, memory_order_relaxed)));

  return state;
}

static inline void __attribute__((always_inline)) SubmitThreadCallState(struct ThreadCall* call, struct ThreadCallState* state)
{
  uint32_t tag;
  uint64_t count;

  tag = atomic_fetch_add_explicit(&call->tag, 1, memory_order_relaxed);

  do state->next = atomic_load_explicit(&call->stack, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&call->stack, &state->next, ADD_ABA_TAG(state, tag, 0, ALIGNMENT), memory_order_release, memory_order_relaxed));

  if (state->next == NULL)
  {
#ifdef TC_FEATURE_RING_FUTEX
    if (state->feature == TC_FEATURE_RING_FUTEX)
    {
      futex(NOTCH(&call->stack), FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
      return;
    }
#endif

    count = 1;
    write(call->handle, &count, sizeof(uint64_t));
  }
}

static inline void __attribute__((always_inline)) MakeInternalThreadCall(struct ThreadCall* call, struct ThreadCallState* state)
{
  if (atomic_load_explicit(&call->weight, memory_order_relaxed) > TC_ROLE_HANDLER)
  {
    if (IsFastRingThread(call->ring) > 0)
    {
      call->function(call->closure, state->arguments);
      atomic_store_explicit(&state->result, TC_RESULT_CALLED, memory_order_relaxed);
      return;
    }

    SubmitThreadCallState(call, state);
    futex((uint32_t*)&state->result, FUTEX_WAIT_PRIVATE, TC_RESULT_PREPARED, NULL, NULL, 0);
  }
}

static int HandleThreadWakeup(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if (reason != RING_REASON_COMPLETE)
  {
    // IORING_OP_FUTEX_WAKE is not completed, call futex() synchronously instead
    futex((uint32_t*)descriptor->submission.addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }

  return 0;
}

static int HandleThreadCall(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct ThreadCall* call;
  struct ThreadCallState* state;

  call = (struct ThreadCall*)descriptor->closure;

  while (state = PeekThreadCallState(call))
  {
    call->function(call->closure, state->arguments);
    atomic_store_explicit(&state->result, TC_RESULT_CALLED, memory_order_release);

#ifdef TC_FEATURE_RING_FUTEX
    if ((call->feature == TC_FEATURE_RING_FUTEX) &&
        (descriptor = AllocateFastRingDescriptor(call->ring, HandleThreadWakeup, NULL)))
    {
      io_uring_prep_futex_wake(&descriptor->submission, (uint32_t*)&state->result, 1, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
      SubmitFastRingDescriptor(descriptor, 0);
      continue;
    }
#endif

    futex((uint32_t*)&state->result, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }

  if ((completion       != NULL) &&
      (call->descriptor != NULL))
  {
    SubmitFastRingDescriptor(call->descriptor, 0);
    return 1;
  }

  return 0;
}

struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure)
{
  struct ThreadCall* call;
  struct FastRingDescriptor* descriptor;

  call       = (struct ThreadCall*)calloc(1, sizeof(struct ThreadCall));
  descriptor = AllocateFastRingDescriptor(ring, HandleThreadCall, call);

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
    io_uring_prep_futex_wait(&descriptor->submission, NOTCH(&call->stack), 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
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
  atomic_fetch_add_explicit(&call->weight, TC_ROLE_CALLER, memory_order_relaxed);
  return call;
}

void ReleaseThreadCall(struct ThreadCall* call, int role)
{
  int weight;
  struct ThreadCallState* state;
  struct FastRingDescriptor* descriptor;

  weight = atomic_fetch_sub_explicit(&call->weight, role, memory_order_relaxed) - role;

  if (role == TC_ROLE_HANDLER)
  {
    while (state = PeekThreadCallState(call))
    {
      atomic_store_explicit(&state->result, TC_RESULT_CANCELED, memory_order_relaxed);
      futex((uint32_t*)&state->result, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }

    if (descriptor = call->descriptor)
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_prep_cancel(&descriptor->submission, call->descriptor, 0);
      SubmitFastRingDescriptor(descriptor, 0);

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

void FreeThreadCall(void* closure)
{
  ReleaseThreadCall((struct ThreadCall*)closure, TC_ROLE_CALLER);
}

int MakeVariadicThreadCall(struct ThreadCall* call, va_list arguments)
{
  struct ThreadCallState* state;

  state = AllocateThreadCallState();

  va_copy(state->arguments, arguments);
  atomic_thread_fence(memory_order_release);
  MakeInternalThreadCall(call, state);

  return atomic_load_explicit(&state->result, memory_order_acquire);
}

int MakeThreadCall(struct ThreadCall* call, ...)
{
  struct ThreadCallState* state;

  state = AllocateThreadCallState();

  va_start(state->arguments, call);
  atomic_thread_fence(memory_order_release);
  MakeInternalThreadCall(call, state);
  va_end(state->arguments);

  return atomic_load_explicit(&state->result, memory_order_acquire);
}

int GetThreadCallWeight(struct ThreadCall* call)
{
  return atomic_load_explicit(&call->weight, memory_order_relaxed);
}
