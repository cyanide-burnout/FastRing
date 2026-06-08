#ifndef LATCHCLIENT_H
#define LATCHCLIENT_H

#include "Latch.h"

#include <time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct LatchClient
{
  int handle;
  uint64_t value;
  struct Latch* latch;
};

int LockLatch(struct LatchClient* client, struct timespec* timeout);
void UnlockLatch(struct LatchClient* client);

struct LatchClient* CreateLatchClient(int handle);
void ReleaseLatchClient(struct LatchClient* client);

#ifdef __cplusplus
}
#endif

#endif
