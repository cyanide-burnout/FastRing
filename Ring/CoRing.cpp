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
  struct FastRingDescriptor* descriptor;

  for (auto descriptor : submitted)
  {
    // Unbind callback to prevent segmentation fault
    descriptor->function = nullptr;
    descriptor->closure  = nullptr;
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
