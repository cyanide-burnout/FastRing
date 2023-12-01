#ifndef THREADCALL_H
#define THREADCALL_H

#include <stdarg.h>

#ifndef __cplusplus
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#endif

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define TC_ROLE_CALLER      (1 << 0)
#define TC_ROLE_HANDLER     (1 << 30)

#define TC_RESULT_CANCELED  0
#define TC_RESULT_CALLED    1

typedef void (*HandleThreadCallFunction)(void* closure, va_list arguments);

struct ThreadCall
{
  HandleThreadCallFunction function;
  void* closure;
#ifndef __cplusplus
  int index;
  int handle;
  sem_t semaphore;
  atomic_int weight;
  pthread_mutex_t lock;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
#endif
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