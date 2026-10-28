#ifndef TURBORING_H
#define TURBORING_H

#define Uses_TApplication
#define Uses_TEvent
#include <tvision/tv.h>

#include <ucontext.h>

#include <vector>

#include "FastRing.h"

// Turbo Vision application on FastRing: the ring loop stays the main loop, run() goes on a fiber of its own.
// The fiber is resumed when a descriptor Turbo Vision waits for gets readable, when its wait times out or on touch()

class TurboRing : public TApplication
{
  public:

    TurboRing(struct FastRing* ring);
    ~TurboRing();

    void getEvent(TEvent& event) override;

    bool start();             // Begins run() on the fiber and returns when it waits for events, false when the fiber cannot be made
    void touch();             // Lets the application look for events and redraw in this iteration of the ring
    void stop();              // Puts cmQuit, the application decides whether to quit
    bool isRunning() const;   // run() has been started and has not returned yet

    static bool isInside();   // Running on the fiber of the application

  private:

    int timeout;                                    // Milliseconds the fiber waits for, negative without a limit
    bool running;                                   // run() is on the fiber
    ucontext_t fibers[2];                           // The application and the ring
    struct FastRing* ring;                          //
    struct FastRingFlusher* flusher;                // Pending flush handler that resumes the fiber
    struct FastRingDescriptor* timer;               // Timeout of the wait
    std::vector<struct FastRingDescriptor*> polls;  // One-shot polls of the descriptors Turbo Vision waits for

    void jump();
    void arm();
    void cancel();
    void wait(int interval);
    void schedule();
    int getTimeout();

    static void runFiber();
    static void handleFlush(void* closure, int reason);
    static void handleTimeout(struct FastRingDescriptor* descriptor);
    static int handleCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);
};

#endif
