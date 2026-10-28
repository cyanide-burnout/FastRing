#include "TurboRing.h"

#include <internal/platform.h>

#include <poll.h>
#include <errno.h>
#include <malloc.h>
#include <sys/resource.h>

#include <algorithm>

#define FIBER_APPLICATION  0
#define FIBER_RING         1

namespace
{
  // Turbo Vision keeps what its wait is made of to itself, the rules of access are not checked in an explicit instantiation

  template <typename Tag, typename Tag::Type Value> struct Expose
  {
    friend typename Tag::Type Fetch(Tag)
    {
      return Value;
    }
  };

  struct WaiterMember    { typedef tvision::EventWaiter tvision::Platform::*Type;                  friend Type Fetch(WaiterMember);    };
  struct DisplayMember   { typedef tvision::DisplayBuffer tvision::Platform::*Type;                friend Type Fetch(DisplayMember);   };
  struct SourcesMember   { typedef std::vector<tvision::EventSource*> tvision::EventWaiter::*Type; friend Type Fetch(SourcesMember);   };
  struct TimeoutFunction { typedef int (*Type)();                                                  friend Type Fetch(TimeoutFunction); };

  template struct Expose<WaiterMember,    &tvision::Platform::waiter>;
  template struct Expose<DisplayMember,   &tvision::Platform::displayBuf>;
  template struct Expose<SourcesMember,   &tvision::EventWaiter::sources>;
  template struct Expose<TimeoutFunction, &TProgram::eventWaitTimeout>;
}

static thread_local TurboRing* active = nullptr;

// TurboRing

TurboRing::TurboRing(struct FastRing* ring) :
  TProgInit(&TApplication::initStatusLine, &TApplication::initMenuBar, &TApplication::initDeskTop),
  timeout(-1), running(false), ring(ring), flusher(nullptr), timer(nullptr)
{
  fibers[FIBER_APPLICATION].uc_stack.ss_sp = nullptr;
}

TurboRing::~TurboRing()
{
  // A fiber still waiting is dropped with whatever is on its stack, stop() and the ring loop let run() return first
  cancel();
  wait(-1);
  RemoveFastRingFlushHandler(ring, flusher);
  free(fibers[FIBER_APPLICATION].uc_stack.ss_sp);
}

void TurboRing::getEvent(TEvent& event)
{
  if (active != this)
  {
    // Outside the fiber, a modal view opened from a ring callback for instance: Turbo Vision waits itself and the ring stalls
    TApplication::getEvent(event);
    return;
  }

  for ( ; ; )
  {
    // Turbo Vision only looks for events, waiting is the business of the ring; idle() may set another timeout meanwhile
    int interval = eventTimeoutMs;

    eventTimeoutMs = 0;
    TApplication::getEvent(event);
    eventTimeoutMs = (eventTimeoutMs != 0) ? eventTimeoutMs : interval;

    if ((event.what != evNothing) ||
        ((timeout = getTimeout()) == 0))
    {
      // An event, or nothing to wait for
      return;
    }

    swapcontext(fibers + FIBER_APPLICATION, fibers + FIBER_RING);
  }
}

bool TurboRing::start()
{
  struct rlimit limit;

  if ((running) ||
      (fibers[FIBER_APPLICATION].uc_stack.ss_sp != nullptr))
  {
    // Once only
    return false;
  }

  getrlimit(RLIMIT_STACK, &limit);
  getcontext(fibers + FIBER_RING);
  getcontext(fibers + FIBER_APPLICATION);

  if ((limit.rlim_cur == RLIM_INFINITY) ||
      ((fibers[FIBER_APPLICATION].uc_stack.ss_sp = malloc(limit.rlim_cur)) == nullptr))
  {
    // No size to take or no memory for it
    return false;
  }

  fibers[FIBER_APPLICATION].uc_stack.ss_size = limit.rlim_cur;
  fibers[FIBER_APPLICATION].uc_link          = nullptr;
  makecontext(fibers + FIBER_APPLICATION, runFiber, 0);

  running = true;
  jump();
  return true;
}

void TurboRing::touch()
{
  if ((active == nullptr) &&
      (running))
  {
    // From the fiber it is no use, it looks for events anyway before it waits
    schedule();
  }
}

void TurboRing::stop()
{
  TEvent event;

  if (running)
  {
    event.what            = evCommand;
    event.message.command = cmQuit;
    event.message.infoPtr = nullptr;
    putEvent(event);
    touch();
  }
}

bool TurboRing::isRunning() const
{
  return running;
}

bool TurboRing::isInside()
{
  return active != nullptr;
}

void TurboRing::jump()
{
  active = this;
  swapcontext(fibers + FIBER_RING, fibers + FIBER_APPLICATION);
  active = nullptr;

  // The fiber waits again or has finished
  arm();
}

void TurboRing::arm()
{
  tvision::Platform& platform = tvision::Platform::getInstance();
  tvision::EventWaiter& waiter = platform.*Fetch(WaiterMember());

  // Sources come and go, a descriptor number may be taken by another file meanwhile: the polls of the last wait go,
  // the new ones are made for the files there are now, a file readable already completes its poll at once
  cancel();

  if (!running)
  {
    // Nothing waits anymore
    wait(-1);
    return;
  }

  for (tvision::EventSource* source : waiter.*Fetch(SourcesMember()))
  {
    struct FastRingDescriptor* descriptor;

    if (descriptor = AllocateFastRingDescriptor(ring, handleCompletion, this))
    {
      // Turbo Vision selects for reading only
      io_uring_prep_poll_add(&descriptor->submission, source->handle, POLLIN);
      SubmitFastRingDescriptor(descriptor, 0);
      polls.push_back(descriptor);
    }
  }

  wait(timeout);
}

void TurboRing::wait(int interval)
{
  if (interval >= 0)
  {
    // Made, or armed anew with the interval
    timer = SetFastRingTimeout(ring, timer, interval, 0, handleTimeout, this);
    return;
  }

  if (timer != nullptr)
  {
    // A negative interval removes it, without one there is nothing to make
    timer = SetFastRingTimeout(ring, timer, -1, 0, nullptr, nullptr);
  }
}

void TurboRing::cancel()
{
  for (struct FastRingDescriptor* descriptor : polls)
  {
    // Completed polls are not in the list anymore
    DiscardFastRingDescriptor(descriptor, nullptr);
  }

  polls.clear();
}

void TurboRing::schedule()
{
  if (flusher == nullptr)
  {
    // The fiber goes on after the completions of this iteration, a handler that cannot be made is tried again by the next one
    flusher = SetFastRingFlushHandler(ring, handleFlush, this);
  }
}

int TurboRing::getTimeout()
{
  tvision::Platform& platform = tvision::Platform::getInstance();

  // The wait of Turbo Vision: eventTimeoutMs and the timers of setTimer(), then a frame held back by the limit of frame rate
  int interval = Fetch(TimeoutFunction())();
  int display  = (platform.*Fetch(DisplayMember())).timeUntilPendingFlushMs();

  if (interval < 0) return display;
  if (display  < 0) return interval;

  return std::min(interval, display);
}

void TurboRing::runFiber()
{
  TurboRing* self = active;

  self->run();

  // The fiber ends here and nothing may switch into it again
  RemoveFastRingFlushHandler(self->ring, self->flusher);

  self->flusher = nullptr;
  self->running = false;

  setcontext(self->fibers + FIBER_RING);
}

void TurboRing::handleFlush(void* closure, int reason)
{
  TurboRing* self;

  if (reason == RING_REASON_COMPLETE)
  {
    self          = static_cast<TurboRing*>(closure);
    self->flusher = nullptr;

    if (self->running)
    {
      // Back to getEvent() of the application
      self->jump();
    }
  }
}

void TurboRing::handleTimeout(struct FastRingDescriptor* descriptor)
{
  TurboRing* self;

  // Released after the callback
  self        = static_cast<TurboRing*>(descriptor->closure);
  self->timer = nullptr;
  self->schedule();
}

int TurboRing::handleCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  TurboRing* self;

  if ((completion      != nullptr) &&
      (completion->res != -ECANCELED) &&
      (self             = static_cast<TurboRing*>(descriptor->closure)))
  {
    // Readable, the poll is done and its descriptor goes with the return
    self->polls.erase(std::remove(self->polls.begin(), self->polls.end(), descriptor), self->polls.end());
    self->schedule();
  }

  return 0;
}
