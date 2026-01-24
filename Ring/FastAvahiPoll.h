#ifndef FASTAVAHIPOLL_H
#define FASTAVAHIPOLL_H

#include <avahi-common/watch.h> 

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ReleaseFastAvahiPoll  free

AvahiPoll* CreateFastAvahiPoll(struct FastRing* ring);

#ifdef __cplusplus
}
#endif

#endif
