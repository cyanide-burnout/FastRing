#include "FastAvahiPoll.h"

#include <malloc.h>

struct Watch
{
  int handle;
  AvahiWatchEvent events;
  AvahiWatchCallback function;
  void* closure;

  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

static int HandlePollCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct Watch* private;
  AvahiWatchEvent events;

  if ((completion != NULL) &&
      (~completion->user_data & RING_DESC_OPTION_IGNORE) &&
      (private            = (struct Watch*)descriptor->closure) &&
      (private->function != NULL))
  {
    descriptor->data.number = 1;
    events                  =
      ((completion->res > 0) *
       ((((completion->res & POLLIN)  != 0) * AVAHI_WATCH_IN)    |
        (((completion->res & POLLOUT) != 0) * AVAHI_WATCH_OUT)   |
        (((completion->res & POLLERR) != 0) * AVAHI_WATCH_ERR)   |
        (((completion->res & POLLHUP) != 0) * AVAHI_WATCH_HUP))) |
      ((completion->res < 0) * AVAHI_WATCH_ERR);

    private->function((AvahiWatch*)private, private->handle, events, private->closure);

    if (descriptor->data.number != 0)
    {
      // Watch didn't clear resubmission flag
      // Clear flag to indicate an exit from HandlePollCompletion()
      descriptor->data.number = 0;

      if (descriptor->submission.opcode != IORING_OP_POLL_ADD)
      {
        // Last SQE was about io_uring_prep_poll_update()
        io_uring_prep_rw(IORING_OP_POLL_ADD, &descriptor->submission, -1, NULL, 0, 0);
        descriptor->submission.fd         = private->handle;
        descriptor->submission.user_data &= ~RING_DESC_OPTION_IGNORE;
      }

      if (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_SUBMITTED)
      {
        SubmitFastRingDescriptor(descriptor, 0);
        return 1;
      }
    }
  }

  return 0;
}

static AvahiWatch* HandleiWatchNew(const AvahiPoll* poll, int handle, AvahiWatchEvent events, AvahiWatchCallback function, void* closure)
{
  __u32 mask;
  struct Watch* private;
  struct FastRingDescriptor* descriptor;

  if (private = (struct Watch*)calloc(1, sizeof(struct Watch)))
  {
    private->ring       = (struct FastRing*)poll->userdata;
    private->handle     = handle;
    private->events     = events;
    private->closure    = closure;
    private->function   = function;
    private->descriptor = AllocateFastRingDescriptor(private->ring, HandlePollCompletion, private);

    if (private->descriptor == NULL)
    {
      free(private);
      return NULL;
    }

    descriptor = private->descriptor;
    mask       =
      (((events & AVAHI_WATCH_IN)  != 0) * POLLIN)  |
      (((events & AVAHI_WATCH_OUT) != 0) * POLLOUT) |
      (((events & AVAHI_WATCH_ERR) != 0) * POLLERR) |
      (((events & AVAHI_WATCH_HUP) != 0) * POLLHUP);

    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_poll_add(&descriptor->submission, handle, mask);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  return (AvahiWatch*)private;
}

static void HandleWatchUpdate(AvahiWatch* watch, AvahiWatchEvent events)
{
  __u32 mask;
  struct Watch* private;
  struct FastRingDescriptor* descriptor;

  if (private = (struct Watch*)watch)
  {
    private->events = events;
    descriptor      = private->descriptor;
    mask            =
      (((events & AVAHI_WATCH_IN)  != 0) * POLLIN)  |
      (((events & AVAHI_WATCH_OUT) != 0) * POLLOUT) |
      (((events & AVAHI_WATCH_ERR) != 0) * POLLERR) |
      (((events & AVAHI_WATCH_HUP) != 0) * POLLHUP);

    if ((descriptor->data.number != 0) ||                                                               // We are inside a call to HandleWatchEvent()
        (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_PENDING) ||  // There is a pending io_uring_prep_poll_add() or io_uring_prep_poll_update()
        (descriptor->submission.poll32_events == __io_uring_prep_poll_mask(mask)))                      // Poll mask has no changes
    {
      // Existing io_uring_prep_poll_add() or io_uring_prep_poll_update() is not yet submitted to kernel
      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(mask);
      return;
    }

    // Update any incomplete io_uring_prep_poll_add()
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_poll_update(&descriptor->submission, descriptor->identifier, descriptor->identifier, mask, IORING_POLL_UPDATE_USER_DATA | IORING_POLL_UPDATE_EVENTS);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

static AvahiWatchEvent HandleWatchGetEvents(AvahiWatch* watch)
{
  struct Watch* private;

  return (private = (struct Watch*)watch) ? private->events : (AvahiWatchEvent)0;
}

static void HandleWatchFree(AvahiWatch* watch)
{
  struct Watch* private;
  struct FastRingDescriptor* descriptor;

  if (private = (struct Watch*)watch)
  {
    descriptor           = private->descriptor;
    descriptor->function = NULL;
    descriptor->closure  = NULL;

    free(private);

    if ((descriptor->data.number != 0) ||                                                               // We are inside a call to HandleWatchEvent()
        (atomic_load_explicit(&descriptor->state, memory_order_relaxed) == RING_DESC_STATE_PENDING) &&  //
        (descriptor->submission.opcode == IORING_OP_POLL_ADD))                                          // io_uring_prep_poll_add() is not yet submitted to kernel
    {
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_nop(&descriptor->submission);
      PrepareFastRingDescriptor(descriptor, 0);
      descriptor->data.number = 0;
      return;
    }

    // Cancel any incomplete io_uring_prep_poll_add()
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

struct Timeout
{
  AvahiTimeoutCallback function;
  void* closure;

  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

static void HandleTimeoutCompletion(struct FastRingDescriptor* descriptor)
{
  struct Timeout* private;

  if (private = (struct Timeout*)descriptor->closure)
  {
    private->descriptor = NULL;

    if (private->function != NULL)
    {
      //
      private->function((AvahiTimeout*)private, private->closure);
    }
  }
}

static AvahiTimeout* HandleTimeoutNew(const AvahiPoll* poll, const struct timeval* value, AvahiTimeoutCallback function, void* closure)
{
  struct Timeout* private;

  if (private = (struct Timeout*)calloc(1, sizeof(struct Timeout)))
  {
    private->ring       = (struct FastRing*)poll->userdata;
    private->closure    = closure;
    private->function   = function;
    private->descriptor = SetFastRingCertainTimeout(private->ring, NULL, (struct timeval*)value, 0, HandleTimeoutCompletion, private);
  }

  return (AvahiTimeout*)private;
}

static void HandleTimeoutUpdate(AvahiTimeout* timeout, const struct timeval* value)
{
  struct Timeout* private;

  if (private = (struct Timeout*)timeout)
  {
    //
    private->descriptor = SetFastRingCertainTimeout(private->ring, private->descriptor, (struct timeval*)value, 0, HandleTimeoutCompletion, private);
  }
}

static void HandleTimeoutFree(AvahiTimeout* timeout)
{
  struct Timeout* private;

  if (private = (struct Timeout*)timeout)
  {
    SetFastRingCertainTimeout(private->ring, private->descriptor, NULL, 0, NULL, NULL);
    free(private);
  }
}

AvahiPoll* CreateFastAvahiPoll(struct FastRing* ring)
{
  AvahiPoll* poll;

  poll = NULL;

  if ((ring != NULL) &&
      (poll  = (AvahiPoll*)calloc(1, sizeof(AvahiPoll))))
  {
    poll->userdata         = ring;
    poll->watch_new        = HandleiWatchNew;
    poll->watch_update     = HandleWatchUpdate;
    poll->watch_get_events = HandleWatchGetEvents;
    poll->watch_free       = HandleWatchFree;
    poll->timeout_new      = HandleTimeoutNew;
    poll->timeout_update   = HandleTimeoutUpdate;
    poll->timeout_free     = HandleTimeoutFree;
  }

  return poll;
}
