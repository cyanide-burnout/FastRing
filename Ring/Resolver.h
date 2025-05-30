#ifndef RESOLVER_H
#define RESOLVER_H

#include <ares.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Bindings to FastPoll

struct ResolverState
{
  ares_channel channel;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

struct ResolverState* CreateResolver(struct FastRing* ring);
void UpdateResolverTimer(struct ResolverState* state);
void ReleaseResolver(struct ResolverState* state);

#ifdef __cplusplus
}
#endif

#endif
