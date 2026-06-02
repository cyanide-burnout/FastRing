#ifndef LATCHSERVER_H
#define LATCHSERVER_H

#include "FastRing.h"
#include "Latch.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct LatchServer
{
  int handle;
  struct Latch* latch;
  struct FastRingDescriptor* descriptor;
};

struct LatchServer* CreateLatchServer(struct FastRing* ring, int handle);
void ReleaseLatchServer(struct LatchServer* server);

#ifdef __cplusplus
}
#endif

#endif
