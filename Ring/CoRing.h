#ifndef CORING_H
#define CORING_H

#include "FastRing.h"
#include "Compromise.h"

#include <set>
#include <vector>

class CoRing;

class CoRingEvent
{
  public:

    struct FastRingDescriptor* descriptor;
    struct io_uring_cqe* completion;
    int reason;

    CoRingEvent();
    CoRingEvent(CoRing* coring, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason, bool* result);

    void keep() const;     // Keep completed descriptor
    void release() const;  // Release completed descriptor

  private:

    CoRing* coring;
    bool* result;
};

class CoRing : public Compromise::Emitter<CoRingEvent>
{
  public:

    friend class CoRingEvent;

    CoRing(struct FastRing* ring);
    ~CoRing();

    struct FastRingDescriptor* allocate();  // Allocate FastRing descriptor (it also sets callbacks and adds to the tracking list)
    void submit();                          // Submit all allocated descriptor to the queue (FastRing's pending queue or URing's SQ)

  private:

    struct FastRing* ring;
    std::set<struct FastRingDescriptor*> submitted;
    std::vector<struct FastRingDescriptor*> allocated;

    bool update(CoRingEvent& event) final;
    static int invoke(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);
};

#endif
