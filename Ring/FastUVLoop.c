#include "FastUVLoop.h"

#include <malloc.h>

static int HandlePollCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastUVLoop* loop;

  if ((completion != NULL) &&
      (~completion->flags & RING_DESC_OPTION_IGNORE))
  {
    loop = (struct FastUVLoop*)descriptor->closure;

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
    loop = (struct FastUVLoop*)closure;

    uv_update_time(loop->loop);
    uv_run(loop->loop, UV_RUN_NOWAIT);

    timeout = uv_backend_timeout(loop->loop);

    if ((loop->interval > 0) &&
        ((timeout < 0) ||
         (timeout > loop->interval)))
    {
      //
      timeout = loop->interval;
    }

    loop->timeout = SetFastRingTimeout(loop->ring, loop->timeout, timeout, 0, HandleTimeoutEvent, loop);
    loop->flush   = NULL;
  }
}

struct FastUVLoop* CreateFastUVLoop(struct FastRing* ring, int interval)
{
  struct FastRingDescriptor* descriptor;
  struct FastUVLoop* loop;
  int handle;

  if (loop = (struct FastUVLoop*)calloc(1, sizeof(struct FastUVLoop)))
  {
    uv_loop_init(&loop->context);

    handle         = uv_backend_fd(&loop->context);
    descriptor     = AllocateFastRingDescriptor(ring, HandlePollCompletion, loop);
    loop->ring     = ring;
    loop->poll     = descriptor;
    loop->loop     = &loop->context;
    loop->interval = interval;

    if (descriptor != NULL)
    {
      io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN);
      SubmitFastRingDescriptor(descriptor, 0);
    }

    TouchFastUVLoop(loop);
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

static void HandleWalk(uv_handle_t* handle, void* argument)
{
  if (!uv_is_closing(handle) &&
      ((*(uint64_t*)argument) & (1ULL << handle->type)))
  {
    // Remove handle from the queue
    uv_unref(handle);
  }
}

void StopFastUVLoop(struct FastUVLoop* loop, int timeout, uint64_t kick)
{
  int64_t limit;
  int64_t remain;
  struct pollfd event;

  if (loop != NULL)
  {
    if (kick != 0ULL)
    {
      // Kick everything that might stuck
      uv_walk(loop->loop, HandleWalk, &kick);
    }

    uv_update_time(loop->loop);

    limit        = uv_now(loop->loop) + (int64_t)timeout;
    event.fd     = uv_backend_fd(loop->loop);
    event.events = POLLIN;

    while ((uv_run(loop->loop, UV_RUN_NOWAIT)) &&
           ((remain = limit - (int64_t)uv_now(loop->loop)) > 0))
    {
      timeout = uv_backend_timeout(loop->loop);

      if ((timeout < 0) ||
          (timeout > remain))
      {
        //
        timeout = remain;
      }

      poll(&event, 1, timeout);
    }
  }
}
