#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct WatchDog
{
  int state;
  uint64_t interval;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

struct WatchDog* CreateWatchDog(struct FastRing* ring);
void ReleaseWatchDog(struct WatchDog* state);

#ifdef __cplusplus
}
#endif

#endif
