#include "SambarAdapter.h"

#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define SAMBAR_TIMEOUT_INTERVAL  1000
#define SAMBAR_ACCEPT_PAUSE      100   // milliseconds

static const struct __kernel_timespec delay = { 0, SAMBAR_ACCEPT_PAUSE * 1000000L };

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

void AttachSambarOpaque(struct SambarOpaque* opaque, struct FastRing* ring, struct smb2_context* context, int force)
{
  int handle;
  uint64_t mask;

  // An update would keep the context of a previous attach in the timeout, so a timeout left from one is removed
  opaque->timeout = SetFastRingTimeout(opaque->ring, opaque->timeout, -1, 0, NULL, NULL);
  opaque->ring    = ring;
  opaque->timeout = SetFastRingTimeout(ring, NULL, SAMBAR_TIMEOUT_INTERVAL, TIMEOUT_FLAG_REPEAT, HandleTimeoutEvent, context);

  smb2_set_opaque(context, opaque);
  smb2_fd_event_callbacks(context, HandleDescriptorChange, HandleEventChange);

  if (force != 0)
  {
    handle  = smb2_get_fd(context);
    mask    = smb2_which_events(context);
    mask   |= (mask != 0ULL) * (RING_POLL_ERROR | RING_POLL_HANGUP | RING_POLL_SHOT | RING_POLL_REPEAT);
    SetFastRingPoll(opaque->ring, handle, mask, HandlePollEvent, context);
  }
}

void DetachSambarOpaque(struct SambarOpaque* opaque, struct smb2_context* context)
{
  int handle;

  if (context != NULL)
  {
    handle = smb2_get_fd(context);
    SetFastRingPoll(opaque->ring, handle, 0ULL, NULL, NULL);
  }

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

static int HandleAcceptCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct SambarListener* listener;
  struct smb2_context* context;

  if ((completion != NULL) &&
      (listener = (struct SambarListener*)descriptor->closure))
  {
    if (descriptor->submission.opcode == IORING_OP_TIMEOUT)
    {
      // The pause is over
      descriptor->data.socket.length = sizeof(struct sockaddr_storage);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_accept(&descriptor->submission, listener->handle, (struct sockaddr*)&descriptor->data.socket.address, &descriptor->data.socket.length, SOCK_CLOEXEC | SOCK_NONBLOCK);
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    if ((completion->res == -EMFILE)  ||
        (completion->res == -ENFILE)  ||
        (completion->res == -ENOBUFS) ||
        (completion->res == -ENOMEM))
    {
      // Out of descriptors or memory: accepting again at once would spin until some are freed
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_timeout(&descriptor->submission, (struct __kernel_timespec*)&delay, 0, 0);
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    if (completion->res >= 0)
    {
      if (context = smb2_init_context())
      {
        // libsmb2 stores its transport fd in the first int-sized field of the opaque context
        // This code is a complete copy of smb2_serve_port_async() behavior
        *(int*)context = completion->res;
        listener->function(context, listener->closure);
      }
      else
      {
        // Failed to initialize smb2_context
        close(completion->res);
      }
    }

    descriptor->data.socket.length = sizeof(struct sockaddr_storage);
    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

static int HandleDiscardCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  if ((completion != NULL) &&
      (completion->res >= 0) &&
      (~completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    // The accept completed before its cancellation
    close(completion->res);
  }

  return 0;
}

struct SambarListener* OpenSambarListener(struct FastRing* ring, uint16_t port, smb2_client_connection function, void* closure)
{
  int value;
  struct sockaddr_in6 address;
  struct SambarListener* listener;
  struct FastRingDescriptor* descriptor;

  if (listener = (struct SambarListener*)calloc(1, sizeof(struct SambarListener)))
  {
    memset(&address, 0, sizeof(struct sockaddr_in6));

    address.sin6_family = AF_INET6;
    address.sin6_port   = htons(port);
    value               = 1;

    listener->handle = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);

    if ((listener->handle < 0) ||
        (setsockopt(listener->handle, SOL_SOCKET,  SO_REUSEADDR,     &value, sizeof(int)) < 0) ||
        (setsockopt(listener->handle, SOL_SOCKET,  SO_REUSEPORT,     &value, sizeof(int)) < 0) ||
        (setsockopt(listener->handle, SOL_TCP,     TCP_NODELAY,      &value, sizeof(int)) < 0) ||
        (setsockopt(listener->handle, IPPROTO_TCP, TCP_DEFER_ACCEPT, &value, sizeof(int)) < 0) ||
        (bind(listener->handle, (struct sockaddr*)&address, sizeof(struct sockaddr_in6))  < 0) ||
        (listen(listener->handle, SOMAXCONN) < 0)                                              ||
        !(descriptor = AllocateFastRingDescriptor(ring, HandleAcceptCompletion, listener)))
    {
      close(listener->handle);
      free(listener);
      return NULL;
    }

    listener->closure  = closure;
    listener->function = function;
    listener->accept   = descriptor;

    descriptor->data.socket.length = sizeof(struct sockaddr_storage);
    io_uring_prep_accept(&descriptor->submission, listener->handle, (struct sockaddr*)&descriptor->data.socket.address, &descriptor->data.socket.length, SOCK_CLOEXEC | SOCK_NONBLOCK);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  return listener;
}

void CloseSambarListener(struct SambarListener* listener)
{
  struct FastRingDescriptor* descriptor;

  if (listener != NULL)
  {
    if (descriptor = listener->accept)
    {
      // Closing the handle does not stop the accept held by the kernel, so cancel it and close a connection it may still deliver
      DiscardFastRingDescriptor(descriptor, HandleDiscardCompletion);
    }

    close(listener->handle);
    free(listener);
  }
}
