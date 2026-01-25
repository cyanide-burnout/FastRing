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

static void HandleWatchEvent(struct FastRingDescriptor* descriptor, int result)
{
  struct Watch* private;
  AvahiWatchEvent events;

  if (private = (struct Watch*)descriptor->closure)
  {
    events =
      ((result > 0) *
       ((((result & POLLIN)  != 0) * AVAHI_WATCH_IN)    |
        (((result & POLLOUT) != 0) * AVAHI_WATCH_OUT)   |
        (((result & POLLERR) != 0) * AVAHI_WATCH_ERR)   |
        (((result & POLLHUP) != 0) * AVAHI_WATCH_HUP))) |
      ((result < 0) * AVAHI_WATCH_ERR);

    private->function((AvahiWatch*)private, private->handle, events, private->closure);
  }
}

static AvahiWatch* HandleiWatchNew(const AvahiPoll* poll, int handle, AvahiWatchEvent events, AvahiWatchCallback function, void* closure)
{
  uint32_t mask;
  struct Watch* private;
  struct FastRingDescriptor* descriptor;

  if (private = (struct Watch*)calloc(1, sizeof(struct Watch)))
  {
    mask =
      (((events & AVAHI_WATCH_IN)  != 0) * POLLIN)  |
      (((events & AVAHI_WATCH_OUT) != 0) * POLLOUT) |
      (((events & AVAHI_WATCH_ERR) != 0) * POLLERR) |
      (((events & AVAHI_WATCH_HUP) != 0) * POLLHUP);

    private->ring       = (struct FastRing*)poll->userdata;
    private->handle     = handle;
    private->events     = events;
    private->closure    = closure;
    private->function   = function;
    private->descriptor = SetFastRingWatch(private->ring, NULL, handle, mask, 0, HandleWatchEvent, private);

    if (private->descriptor == NULL)
    {
      free(private);
      return NULL;
    }
  }

  return (AvahiWatch*)private;
}

static void HandleWatchUpdate(AvahiWatch* watch, AvahiWatchEvent events)
{
  uint32_t mask;
  struct Watch* private;
  struct FastRingDescriptor* descriptor;

  if (private = (struct Watch*)watch)
  {
    mask =
      (((events & AVAHI_WATCH_IN)  != 0) * POLLIN)  |
      (((events & AVAHI_WATCH_OUT) != 0) * POLLOUT) |
      (((events & AVAHI_WATCH_ERR) != 0) * POLLERR) |
      (((events & AVAHI_WATCH_HUP) != 0) * POLLHUP);

    private->events     = events;
    private->descriptor = SetFastRingWatch(private->ring, private->descriptor, private->handle, mask, 0, HandleWatchEvent, private);
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

  if (private = (struct Watch*)watch)
  {
    RemoveFastRingWatch(private->descriptor);
    free(private);
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
