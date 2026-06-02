#ifndef LATCH_H
#define LATCH_H

#include <stdint.h>
#include <endian.h>

#ifndef ATOMIC
#ifndef __cplusplus
#include <stdatomic.h>
#define ATOMIC(type)  type _Atomic
#else
#include <atomic>
#define ATOMIC(type)  std::atomic<type>
#endif
#endif

#ifndef LATCH
#if (__BYTE_ORDER == __LITTLE_ENDIAN) || (UINTPTR_MAX != UINT64_MAX)
#define LATCH(address)  ((uint32_t*)(address) + 0)
#else
#define LATCH(address)  ((uint32_t*)(address) + 1)
#endif
#endif

#define LATCH_STATE_LOCK_REQUESTED  0b01ULL
#define LATCH_STATE_LOCK_GRANTED    0b10ULL
#define LATCH_STATE_MASK            0b11ULL
#define LATCH_ID_SHIFT              8

struct Latch
{
  ATOMIC(uint64_t) value;
};

#endif
