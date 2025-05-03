#ifndef THREADCALL_H
#define THREADCALL_H

#include <stdarg.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (IO_URING_VERSION_MAJOR > 2) || (IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR >= 6)
#define TC_FEATURE_RING_FUTEX  1
#endif

#define TC_ROLE_CALLER      (1 << 0)
#define TC_ROLE_HANDLER     (1 << 30)

#define TC_RESULT_PREPARED  0
#define TC_RESULT_CALLED    1
#define TC_RESULT_CANCELED  2

#define TC_WAKE_LAZY        0
#define TC_WAKE_HARD        1

typedef void (*HandleThreadCallFunction)(void* closure, va_list arguments);

struct ThreadCallState
{
  ATOMIC(uint32_t) result;
  ATOMIC(uint32_t) tag;
  ATOMIC(struct ThreadCallState*) next;
  va_list arguments;
};

struct ThreadCall
{
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;

  HandleThreadCallFunction function;
  void* closure;

  int index;
  int handle;
  int feature;
  ATOMIC(int) count;
  ATOMIC(int) weight;
  ATOMIC(struct ThreadCallState*) stack;
};

struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure);
struct ThreadCall* HoldThreadCall(struct ThreadCall* call);
void ReleaseThreadCall(struct ThreadCall* call, int role);
void FreeThreadCall(void* closure);
int MakeVariadicThreadCall(struct ThreadCall* call, va_list arguments);
int MakeThreadCall(struct ThreadCall* call, ...);
int GetThreadCallWeight(struct ThreadCall* call);

#ifdef __cplusplus
}
#endif

#endif
