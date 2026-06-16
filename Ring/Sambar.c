#include "Sambar.h"

#include <malloc.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define SAMBAR_TIMEOUT_INTERVAL  1000

static void HandlePollEvent(int handle, uint32_t events, void* closure, uint64_t options)
{
  smb2_service_fd((struct smb2_context*)closure, handle, events);
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  smb2_service_fd((struct smb2_context*)descriptor->closure, -1, 0);
}

static void HandleDescriptorChange(struct smb2_context* context, t_socket handle, int command)
{
  struct SambarOpaque* opaque;
  uint64_t mask;

  if (opaque = (struct SambarOpaque*)smb2_get_opaque(context))
  {
    switch (command)
    {
      case SMB2_ADD_FD:
        mask  = smb2_which_events(context);
        mask |= (mask != 0ULL) * (RING_POLL_ERROR | RING_POLL_HANGUP | RING_POLL_SHOT | RING_POLL_REPEAT);
        SetFastRingPoll(opaque->ring, handle, mask, HandlePollEvent, context);
        return;

      case SMB2_DEL_FD:
        SetFastRingPoll(opaque->ring, handle, 0ULL, HandlePollEvent, context);
        return;
    }
  }
}

static void HandleEventChange(struct smb2_context* context, t_socket handle, int events)
{
  struct SambarOpaque* opaque;
  uint64_t mask;

  if (opaque = (struct SambarOpaque*)smb2_get_opaque(context))
  {
    mask  = events;
    mask |= (mask != 0ULL) * (RING_POLL_ERROR | RING_POLL_HANGUP | RING_POLL_SHOT | RING_POLL_REPEAT);
    SetFastRingPoll(opaque->ring, handle, mask, HandlePollEvent, context);
  }
}

void AttachSambarOpaque(struct SambarOpaque* opaque, struct FastRing* ring, struct smb2_context* context)
{
  opaque->ring    = ring;
  opaque->timeout = SetFastRingTimeout(ring, opaque->timeout, SAMBAR_TIMEOUT_INTERVAL, TIMEOUT_FLAG_REPEAT, HandleTimeoutEvent, context);

  smb2_set_opaque(context, opaque);
  smb2_fd_event_callbacks(context, HandleDescriptorChange, HandleEventChange);
}

void DetachSambarOpaque(struct SambarOpaque* opaque)
{
  opaque->timeout = SetFastRingTimeout(opaque->ring, opaque->timeout, -1, 0, NULL, NULL);
}

void* GetSambarClosure(struct smb2_context* context, int offset)
{
  uint8_t* opaque;

  if (opaque = (uint8_t*)smb2_get_opaque(context))
  {
    // SambarOpaque must be embedded in the user's closure at the given offsetof()
    return opaque - offset;
  }

  return NULL;
}

static void HandleAcceptEvent(int handle, uint32_t events, void* closure, uint64_t options)
{
  struct SambarServer* server;
  struct smb2_context* context;

  server = (struct SambarServer*)closure;

  if ((events & (POLLERR | POLLHUP)) == 0)
  {
    while ((smb2_serve_port_async(handle, 0, &context) == 0) &&
           (context != NULL))
    {
      // libsmb2 hides accept here and returns a ready smb2_context for each pending connection
      server->function(context, server->closure);
    }
  }
}

struct SambarServer* CreateSambarServer(struct FastRing* ring, uint16_t port, HandleSambarAcceptFunction function, void* closure)
{
  struct sockaddr_in6 address;
  struct SambarServer* server;
  int value;

  if (server = (struct SambarServer*)calloc(1, sizeof(struct SambarServer)))
  {
    memset(&address, 0, sizeof(struct sockaddr_in6));

    address.sin6_family = AF_INET6;
    address.sin6_port   = htons(port);
    value               = 1;

    server->handle = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);

    if ((server->handle < 0) ||
        (setsockopt(server->handle, SOL_SOCKET,  SO_REUSEADDR,     &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, SOL_SOCKET,  SO_REUSEPORT,     &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, SOL_TCP,     TCP_NODELAY,      &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, IPPROTO_TCP, TCP_DEFER_ACCEPT, &value, sizeof(int)) < 0) ||
        (bind(server->handle, (struct sockaddr*)&address, sizeof(struct sockaddr_in6))  < 0) ||
        (listen(server->handle, SOMAXCONN) < 0))
    {
      close(server->handle);
      free(server);
      return NULL;
    }

    server->ring     = ring;
    server->closure  = closure;
    server->function = function;

    if (SetFastRingPoll(ring, server->handle, RING_POLL_READ | RING_POLL_SHOT | RING_POLL_REPEAT | RING_POLL_ERROR | RING_POLL_HANGUP, HandleAcceptEvent, server) != 0)
    {
      close(server->handle);
      free(server);
      return NULL;
    }
  }

  return server;
}

void ReleaseSambarServer(struct SambarServer* server)
{
  if (server != NULL)
  {
    RemoveFastRingPoll(server->ring, server->handle);
    close(server->handle);
    free(server);
  }
}
