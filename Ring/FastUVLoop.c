#include "FastUVLoop.h"

#include <malloc.h>

static int HandlePollCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastUVLoop* loop;

  loop = (struct FastUVLoop*)descriptor->closure;

  if ((completion != NULL) &&
      (~completion->flags & RING_DESC_OPTION_IGNORE))
  {
    TouchFastUVLoop(loop);
    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  struct FastUVLoop* loop;

  loop          = (struct FastUVLoop*)descriptor->closure;
  loop->timeout = NULL;

  TouchFastUVLoop(loop);
}

static void HandleFlushEvent(void* closure, int reason)
{
  struct FastUVLoop* loop;
  int timeout;

  if (reason == RING_REASON_COMPLETE)
  {
    loop        = (struct FastUVLoop*)closure;
    loop->flush = NULL;

    uv_run(loop->loop, UV_RUN_NOWAIT);

    timeout       = uv_backend_timeout(loop->loop);
    loop->timeout = SetFastRingTimeout(loop->ring, loop->timeout, timeout, 0, HandleTimeoutEvent, loop);
  }
}

struct FastUVLoop* CreateFastUVLoop(struct FastRing* ring)
{
  struct FastRingDescriptor* descriptor;
  struct FastUVLoop* loop;
  int handle;

  if (loop = (struct FastUVLoop*)calloc(1, sizeof(struct FastUVLoop)))
  {
    uv_loop_init(&loop->context);

    handle     = uv_backend_fd(&loop->context);
    descriptor = AllocateFastRingDescriptor(ring, HandlePollCompletion, loop);
    loop->ring = ring;
    loop->poll = descriptor;
    loop->loop = &loop->context;

    if (descriptor != NULL)
    {
      io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN);
      SubmitFastRingDescriptor(descriptor, 0);
    }
  }

  return loop;
}

void ReleaseFastUVLoop(struct FastUVLoop* loop)
{
  struct FastRingDescriptor* descriptor;

  if (loop != NULL)
  {
    if (descriptor = loop->poll)
    {
      descriptor->closure  = NULL;
      descriptor->function = NULL;
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    }

    SetFastRingTimeout(loop->ring, loop->timeout, -1, 0, HandleTimeoutEvent, loop);
    RemoveFastRingFlushHandler(loop->ring, loop->flush);
    uv_loop_close(loop->loop);
    free(loop);
  }
}

void TouchFastUVLoop(struct FastUVLoop* loop)
{
  if ((loop        != NULL) &&
      (loop->flush == NULL))
  {
    // HandleFlushEvent should be only set once per cycle
    loop->flush = SetFastRingFlushHandler(loop->ring, HandleFlushEvent, loop);
  }
}
