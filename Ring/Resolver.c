#include "Resolver.h"

#include <alloca.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>

#define RESOLVER_CONDITION_GUARD    (1 << 0)
#define RESOLVER_CONDITION_RELEASE  (1 << 1)

static void DestroyResolver(struct ResolverState* state)
{
  SetFastRingTimeout(state->ring, state->descriptor, -1, 0, NULL, NULL);
  ares_destroy(state->channel);
  free(state);
}

static void HandleSocketEvent(int handle, uint32_t flags, void* closure, uint64_t options)
{
  struct ResolverState* state;
  int handle1;
  int handle2;

  state   = (struct ResolverState*)closure;
  handle1 = handle | ARES_SOCKET_BAD * ((flags & (POLLIN | POLLERR | POLLHUP)) == 0);
  handle2 = handle | ARES_SOCKET_BAD * ((flags & (POLLOUT                   )) == 0);

  state->condition |=  RESOLVER_CONDITION_GUARD;
  ares_process_fd(state->channel, handle1, handle2);
  state->condition &= ~RESOLVER_CONDITION_GUARD;

  if (state->condition & RESOLVER_CONDITION_RELEASE)
  {
    DestroyResolver(state);
    return;
  }

  UpdateResolverTimer(state);
}

static void HandleTimerEvent(struct FastRingDescriptor* descriptor)
{
  struct ResolverState* state;

  state             = (struct ResolverState*)descriptor->closure;
  state->descriptor = NULL;

  state->condition |=  RESOLVER_CONDITION_GUARD;
  ares_process_fd(state->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  state->condition &= ~RESOLVER_CONDITION_GUARD;

  if (state->condition & RESOLVER_CONDITION_RELEASE)
  {
    DestroyResolver(state);
    return;
  }

  UpdateResolverTimer(state);
}

static void ManageResolverHandler(void* data, ares_socket_t handle, int readable, int writable)
{
  struct ResolverState* state;
  uint64_t flags;

  state = (struct ResolverState*)data;
  flags =
    ((!!readable) * (POLLIN |           POLLERR | POLLHUP)) |
    ((!!writable) * (POLLIN | POLLOUT | POLLERR | POLLHUP));

  SetFastRingPoll(state->ring, handle, flags, HandleSocketEvent, data);
}

struct ResolverState* CreateResolver(struct FastRing* ring)
{
  struct ResolverState* state;
  struct ares_options options;

  state = (struct ResolverState*)calloc(1, sizeof(struct ResolverState));

  options.flags              = ARES_FLAG_STAYOPEN;
  options.sock_state_cb      = ManageResolverHandler;
  options.sock_state_cb_data = state;

  if ((state == NULL) ||
      (ares_init_options(&state->channel, &options, ARES_OPT_FLAGS | ARES_OPT_SOCK_STATE_CB) != ARES_SUCCESS))
  {
    free(state);
    return NULL;
  }

  state->ring       = ring;
  state->descriptor = NULL;

  return state;
}

void UpdateResolverTimer(struct ResolverState* state)
{
  struct timeval* interval;

  interval          = ares_timeout(state->channel, NULL, (struct timeval*)alloca(sizeof(struct timeval)));
  state->descriptor = SetFastRingCertainTimeout(state->ring, state->descriptor, interval, 0, HandleTimerEvent, state);
}

void ReleaseResolver(struct ResolverState* state)
{
  if ((state != NULL) &&
      (~state->condition & RESOLVER_CONDITION_RELEASE))
  {
    // ares_destroy() reports pending queries as ARES_EDESTRUCTION and a handler may call
    // this function again, so the flag has to be raised before the channel is destroyed
    state->condition |= RESOLVER_CONDITION_RELEASE;

    if (~state->condition & RESOLVER_CONDITION_GUARD)
    {
      //
      DestroyResolver(state);
    }
  }
}
