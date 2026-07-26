#include "CoRing.h"

#include <system_error>

CoRingEvent::CoRingEvent() :
  coring(nullptr), descriptor(nullptr), completion(nullptr), reason(0), result(nullptr)
{

}

CoRingEvent::CoRingEvent(CoRing* coring, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason, bool* result) :
  coring(coring), descriptor(descriptor), completion(completion), reason(reason), result(result)
{
  *result = false;
  coring->submitted.erase(descriptor);
}

void CoRingEvent::keep() const
{
  if (result && !*result)
  {
    *result = true;
    coring->submitted.insert(descriptor);
  }
}

void CoRingEvent::release() const
{
  if (result && *result)
  {
    *result = false;
    coring->submitted.erase(descriptor);
  }
}

CoRing::CoRing(struct FastRing* ring) :
  ring(ring)
{

}

CoRing::~CoRing()
{
  for (auto descriptor : submitted)
  {
    descriptor->function = nullptr;
    descriptor->closure  = nullptr;

    cancel(descriptor);
  }

  for (auto descriptor : allocated)
  {
    // Descriptor is unused, just release
    ReleaseFastRingDescriptor(descriptor);
  }
}

struct FastRingDescriptor* CoRing::allocate()
{
  struct FastRingDescriptor* descriptor;

  descriptor = AllocateFastRingDescriptor(ring, invoke, this);

  if (descriptor == nullptr)
  {
    auto error = std::error_code(errno, std::generic_category());
    throw std::system_error(error, __PRETTY_FUNCTION__);
  }

  allocated.push_back(descriptor);

  return descriptor;
}

void CoRing::submit()
{
  struct FastRingDescriptor* descriptor;

  for (auto descriptor : allocated)
  {
    SubmitFastRingDescriptor(descriptor, 0);
    submitted.insert(descriptor);
  }

  allocated.clear();
}

void CoRing::cancel(struct FastRingDescriptor* other) noexcept
{
  struct FastRingDescriptor* descriptor;

  if (descriptor = AllocateFastRingDescriptor(ring, nullptr, nullptr))
  {
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_cancel64(&descriptor->submission, other->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
}

bool CoRing::update(CoRingEvent& event)
{
  submit();
  return false;
}

int CoRing::invoke(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  CoRing* self(static_cast<CoRing*>(descriptor->closure));
  bool result(false);

  self->wake(CoRingEvent(self, descriptor, completion, reason, &result));

  return result;
}
