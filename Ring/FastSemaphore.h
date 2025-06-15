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

// Asynchronous token handler; return 1 to keep waiting for more tokens, 0 to unregister.
typedef int (*FastSemaphoreFunction)(sem_t* semaphore, void* closure);

struct FastSemaphoreData
{
  sem_t* semaphore;                // The semaphore must remain valid for the entire lifetime of the waiter
  FastSemaphoreFunction function;  // User-provided callback to handle tokens
  void* closure;                   // Opaque user data passed to the callback
  int limit;                       // Maximum number of tokens to process per callback invocation
  int state;                       // Non-zero if inside callback; used to ensure safe destruction
};

// RegisterFastSemaphore() and CancelFastSemaphore() provide a reactive, asynchronous alternative to sem_wait()
struct FastRingDescriptor* RegisterFastSemaphore(struct FastRing* ring, sem_t* semaphore, FastSemaphoreFunction function, void* closure, int limit);
void CancelFastSemaphore(struct FastRingDescriptor* descriptor);

// PostFastSemaphore() acts as a replacement for sem_post(), providing asynchronous wake-up
int PostFastSemaphore(struct FastRing* ring, sem_t* semaphore);

#ifdef __cplusplus
}
#endif

#endif
