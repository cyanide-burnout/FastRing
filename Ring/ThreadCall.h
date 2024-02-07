#ifndef THREADCALL_H
#define THREADCALL_H

#include <stdarg.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define TC_ROLE_CALLER      (1 << 0)
#define TC_ROLE_HANDLER     (1 << 30)

#define TC_RESULT_PREPARED  0
#define TC_RESULT_CALLED    1
#define TC_RESULT_CANCELED  2

typedef void (*HandleThreadCallFunction)(void* closure, va_list arguments);

struct ThreadCallState
{
  ATOMIC(struct ThreadCallState*) next;
  ATOMIC(uint32_t) result;
  va_list arguments;
};

struct ThreadCall
{
  HandleThreadCallFunction function;
  void* closure;

  ATOMIC(int) weight;
  ATOMIC(uint32_t) tag;
  ATOMIC(struct ThreadCallState*) stack;

  int index;
  int handle;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
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