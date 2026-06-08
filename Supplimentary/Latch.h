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
#if (__BYTE_ORDER == __LITTLE_ENDIAN)
#define LATCH(address)  ((uint32_t*)(address) + 0)
#else
#define LATCH(address)  ((uint32_t*)(address) + 1)
#endif
#endif

#define LATCH_STATE_LOCK_REQUESTED  0b01ULL
#define LATCH_STATE_LOCK_GRANTED    0b10ULL
#define LATCH_STATE_MASK            0b11ULL
#define LATCH_TID_SHIFT             8
#define LATCH_PID_SHIFT             32
#define LATCH_TID_MASK              0x00ffffffULL
#define LATCH_PID_MASK              0xffffffffULL

struct Latch
{
  ATOMIC(uint64_t) value;
};

#endif
