#ifndef FASTUVLOOP_H
#define FASTUVLOOP_H

#include <uv.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct FastUVLoop
{
  uv_loop_t* loop;
  struct FastRing* ring;
  struct FastRingFlusher* flush;
  struct FastRingDescriptor* poll;
  struct FastRingDescriptor* timeout;
  uv_loop_t context;
  int interval;
};

struct FastUVLoop* CreateFastUVLoop(struct FastRing* ring, int interval);
void ReleaseFastUVLoop(struct FastUVLoop* loop);
void TouchFastUVLoop(struct FastUVLoop* loop);
void StopFastUVLoop(struct FastUVLoop* loop, uint64_t timeout, uint64_t force);

#ifdef __cplusplus
}
#endif

#endif
