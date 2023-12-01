#define _GNU_SOURCE
#include <malloc.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "ThreadCall.h"

struct ThreadCallState
{
  va_list arguments;
  atomic_int result;
};

static int HandleThreadCall(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct ThreadCall* call;
  struct ThreadCallState* state;

  call  = (struct ThreadCall*)descriptor->closure;
  state = (struct ThreadCallState*)descriptor->data.number;

  call->function(call->closure, state->arguments);
  atomic_store_explicit(&state->result, TC_RESULT_CALLED, memory_order_release);

  sem_post(&call->semaphore);
  SubmitFastRingDescriptor(descriptor, 0);
  return 1;
}

static inline void __attribute__((always_inline)) MakeInternalThreadCall(struct ThreadCall* call, struct ThreadCallState* state)
{
  uint64_t value;

  pthread_mutex_lock(&call->lock);

  if (atomic_load_explicit(&call->weight, memory_order_relaxed) > TC_ROLE_HANDLER)
  {
    value = (uint64_t)state;
    write(call->handle, &value, sizeof(uint64_t));
    sem_wait(&call->semaphore);
  }

  pthread_mutex_unlock(&call->lock);
}

struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure)
{
  struct ThreadCall* call;
  struct FastRingDescriptor* descriptor;

  call = (struct ThreadCall*)calloc(1, sizeof(struct ThreadCall));

  call->descriptor = AllocateFastRingDescriptor(ring, HandleThreadCall, call);
  call->function   = function;
  call->closure    = closure;
  call->handle     = eventfd(0, EFD_CLOEXEC);
  call->index      = AddFastRingRegisteredFile(ring, call->handle);
  call->ring       = ring;

  sem_init(&call->semaphore, 0, 0);
  pthread_mutex_init(&call->lock, NULL);
  atomic_init(&call->weight, TC_ROLE_HANDLER);

  if ((call->index >= 0) &&
      (descriptor = call->descriptor))
  {
    io_uring_prep_read(&descriptor->submission, call->index, &descriptor->data.number, sizeof(uint64_t), 0);
    io_uring_sqe_set_flags(&descriptor->submission, IOSQE_FIXED_FILE);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  if ((call->index < 0) &&
      (descriptor = call->descriptor))
  {
    io_uring_prep_read(&descriptor->submission, call->handle, &descriptor->data.number, sizeof(uint64_t), 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

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

  weight = atomic_fetch_sub_explicit(&call->weight, role, memory_order_relaxed) - role;

  if (role == TC_ROLE_HANDLER)
  {
    sem_post(&call->semaphore);

    if (call->descriptor != NULL)
    {
      call->descriptor->function = NULL;
      call->descriptor->closure  = NULL;
    }

    RemoveFastRingRegisteredFile(call->ring, call->handle);
  }

  if (weight == 0)
  {
    pthread_mutex_destroy(&call->lock);
    sem_destroy(&call->semaphore);
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
  struct ThreadCallState state;

  va_copy(state.arguments, arguments);
  atomic_store_explicit(&state.result, TC_RESULT_CANCELED, memory_order_release);
  MakeInternalThreadCall(call, &state);

  return atomic_load_explicit(&state.result, memory_order_acquire);
}

int MakeThreadCall(struct ThreadCall* call, ...)
{
  struct ThreadCallState state;

  va_start(state.arguments, call);
  atomic_store_explicit(&state.result, TC_RESULT_CANCELED, memory_order_release);
  MakeInternalThreadCall(call, &state);
  va_end(state.arguments);

  return atomic_load_explicit(&state.result, memory_order_acquire);
}

int GetThreadCallWeight(struct ThreadCall* call)
{
  return atomic_load_explicit(&call->weight, memory_order_relaxed);
}
