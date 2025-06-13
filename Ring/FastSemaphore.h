#ifndef FASTSEMAPHORE_H
#define FASTSEMAPHORE_H

// This module is a hacky and tricky workaround over glibc to enable the implementation of asynchronous handlers for standard sem_t semaphores

#include "FastRing.h"

#include <semaphore.h>
#include <gnu/libc-version.h>

#if !defined(__GLIBC__) || (__GLIBC__ < 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ < 34))
#error Incompatible glibc version: requires glibc >= 2.34
#endif

#if (IO_URING_VERSION_MAJOR < 2) || ((IO_URING_VERSION_MAJOR == 2) && (IO_URING_VERSION_MINOR < 6))
#error Incompatible io_uring version: requires liburing >= 2.6
#endif

#ifdef __cplusplus
extern "C"
{
#endif

typedef int (*FastSemaphoreFunction)(sem_t* semaphore, void* closure);

struct FastSemaphoreData
{
  sem_t* semaphore;
  FastSemaphoreFunction function;
  void* closure;
  int state;
};

struct FastRingDescriptor* SubmitFastSemaphoreWait(struct FastRing* ring, sem_t* semaphore, FastSemaphoreFunction function, void* closure);
void CancelFastSemaphoreWait(struct FastRingDescriptor* descriptor);

int SubmitFastSemaphorePost(struct FastRing* ring, sem_t* semaphore);

#ifdef __cplusplus
}
#endif

#endif
