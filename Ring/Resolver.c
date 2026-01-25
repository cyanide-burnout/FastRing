#include "Resolver.h"

#include <alloca.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>

static void HandleSocketEvent(int handle, uint32_t flags, void* closure, uint64_t options)
{
  struct ResolverState* state;
  int handle1;
  int handle2;

  state   = (struct ResolverState*)closure;
  handle1 = handle | ARES_SOCKET_BAD * ((flags & (POLLIN | POLLERR | POLLHUP)) == 0);
  handle2 = handle | ARES_SOCKET_BAD * ((flags & (POLLOUT                   )) == 0);

  ares_process_fd(state->channel, handle1, handle2);
  UpdateResolverTimer(state);
}

static void HandleTimerEvent(struct FastRingDescriptor* descriptor)
{
  struct ResolverState* state;

  state             = (struct ResolverState*)descriptor->closure;
  state->descriptor = NULL;

  ares_process_fd(state->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
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
  if (state != NULL)
  {
    SetFastRingTimeout(state->ring, state->descriptor, -1, 0, NULL, NULL);
    ares_destroy(state->channel);
    free(state);
  }
}
