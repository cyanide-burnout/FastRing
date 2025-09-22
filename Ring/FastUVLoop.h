#ifndef FASTUVLOOP_H
#define FASTUVLOOP_H

#include <uv.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define UVLOOP_KICK_UNREF(type)  (1ULL << type)
#define UVLOOP_KICK_POKE_TIMER   (1ULL << 60)

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
void StopFastUVLoop(struct FastUVLoop* loop, int timeout, uint64_t kick);

#ifdef __cplusplus
}
#endif

#endif
