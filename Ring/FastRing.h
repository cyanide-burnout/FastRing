#ifndef FASTRING_H
#define FASTRING_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <liburing.h>
#include <sys/types.h>

#ifndef __cplusplus

#include <stdatomic.h>

#define ATOMIC(type)  type _Atomic

#else

#include <atomic>
#include <memory>

#define ATOMIC(type)  std::atomic<type>

#endif

#ifdef __cplusplus
extern "C"
{
#endif

struct FastRing;
struct FastRingEntry;
struct FastRingDescriptor;
struct FastRingBufferProvider;

#define ADD_ABA_TAG(address, tag, addenum, alignment)  ((void*)(((uintptr_t)(address)) | ((((uintptr_t)(tag)) + ((uintptr_t)(addenum))) & (((uintptr_t)(alignment)) - 1ULL))))
#define REMOVE_ABA_TAG(type, address, alignment)       ((type*)(((uintptr_t)(address)) & (~(((uintptr_t)(alignment)) - 1ULL))))

// FastRing

#define RING_DESC_STATE_FREE       0
#define RING_DESC_STATE_ALLOCATED  1
#define RING_DESC_STATE_PENDING    2
#define RING_DESC_STATE_SUBMITTED  3

#define RING_DESC_ALIGNMENT        512
#define RING_DESC_INTEGRITY_MASK   0x3f

// Fortunately due to alignment the lower bits of SQE/CQE user_data can be used to pass CRC6 and an option

#define RING_DESC_OPTION_MASK      (((uint64_t)RING_DESC_ALIGNMENT - 1ULL) ^ (uint64_t)RING_DESC_INTEGRITY_MASK)
#define RING_DESC_OPTION_IGNORE    (RING_DESC_ALIGNMENT >> 1)
#define RING_DESC_OPTION_USER1     (RING_DESC_ALIGNMENT >> 2)
#define RING_DESC_OPTION_USER2     (RING_DESC_ALIGNMENT >> 3)

#define RING_DATA_UNDEFINED        (LIBURING_UDATA_TIMEOUT - 1ULL)
#define RING_DATA_ADDRESS_MASK     (UINT64_MAX ^ ((uint64_t)RING_DESC_ALIGNMENT - 1ULL))

#define RING_REASON_COMPLETE       0
#define RING_REASON_INCOMPLETE     1
#define RING_REASON_RELEASED       2

#define RING_TRACE_ACTION_HANDLE   0
#define RING_TRACE_ACTION_RELEASE  1

typedef int (*HandleFastRingEventFunction)(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);
typedef void (*HandleFlushFunction)(struct FastRing* ring, void* closure);
typedef void (*TraceFastRingFunction)(int action, struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason, void* closure);
typedef void (*HandlePollEventFunction)(int handle, uint32_t flags, void* closure, uint64_t options);
typedef void (*HandleTimeoutFunction)(struct FastRingDescriptor* descriptor);

struct FastRingPollData
{
  int handle;
  uint64_t flags;
  HandlePollEventFunction function;
};

struct FastRingTimeoutData
{
  uint64_t flags;
  struct __kernel_timespec interval;
  HandleTimeoutFunction function;
};

struct FastRingSocketData
{
  uint64_t number;
  socklen_t length;
  struct iovec vector;
  struct msghdr message;
  struct sockaddr_storage address;
};

union FastRingData
{
  void* pointer;
  uint64_t number;
  struct FastRingPollData poll;
  struct FastRingSocketData socket;
  struct FastRingTimeoutData timeout;
  uint8_t data[256];
};

struct FastRingDescriptor
{
  struct FastRing* ring;                         // (  8) Related ring

  uint32_t state;                                // ( 12) RING_DESC_STATE_*
  uint32_t length;                               // ( 16) Length of submission
  uint32_t linked;                               // ( 20) Count of following linked descriptors in chain (when check is required)
  ATOMIC(uint32_t) tag;                          // ( 24) Lock-free stack tag (see FastRing's available)
  ATOMIC(uint32_t) references;                   // ( 28) Count of references (SQEs, files, etc.)
  struct FastRingDescriptor* previous;           // ( 36) Previous linked descriptor (useful for chains with IOSQE_CQE_SKIP_SUCCESS)
  ATOMIC(struct FastRingDescriptor*) next;       // ( 44) Next descriptor in the queue (available, pending, IOSQE_CQE_SKIP_SUCCESS)

  void* closure;                                 // ( 52) User's closure
  HandleFastRingEventFunction function;          // ( 60) Handler function
  ATOMIC(struct FastRingDescriptor*) heap;       // ( 68) Next allocated descriptor (see FastRing's heap)
  uint64_t identifier;                           // ( 76) Prepared user_data identifier for *_update() and *_remove() SQEs
  uint32_t integrity;                            // ( 80) CRC6(function + closure + tag)

  struct io_uring_sqe submission;                // (152) Copy of actual SQE
  uint64_t reserved[8];                          // (216) Reserved for IORING_SETUP_SQE128
  union FastRingData data;                       // (472) User's specified data
};                                               // ~ 512 bytes block including malloc header (usualy 24 bytes)

struct FastRingTrace
{
  void* closure;
  TraceFastRingFunction function;
};

struct FastRingFileEntry
{
  uint32_t index;                                // | Index in registered file table
  uint32_t references;                           // | Count of references to registered file table
  struct FastRingDescriptor* descriptor;         // Descriptor for FastPoll API
};

struct FastRingFlushEntry
{
  void* closure;
  HandleFlushFunction function;
};

struct FastRingFileList
{
  pthread_mutex_t lock;                          //
  uint32_t length;                               // Length of data in elements
  uint32_t* filters;                             // Bitmap of registered files (Regitered File API)
  struct FastRingFileEntry* entries;             //
};

struct FastRingBufferList
{
  pthread_mutex_t lock;                          //
  uint32_t count;                                // Count of registered buffers
  uint32_t length;                               // List length
  uint32_t position;                             // Current scanning position
  struct iovec* vectors;                         // List of vectors
};

struct FastRingFlushList
{
  pthread_mutex_t lock;                          //
  uint32_t count;                                // Count of available elements
  uint32_t length;                               // Length of data in elements
  struct FastRingFlushEntry* entries;            //
};

struct FastRing
{
  struct io_uring ring;                          //
  struct io_uring_probe* probe;                  //
  struct io_uring_params parameters;             //
  struct FastRingTrace trace;                    //
  pid_t thread;                                  // TID of processing thread

  ATOMIC(struct FastRingDescriptor*) heap;       // Last allocated descriptor (required for release)
  ATOMIC(struct FastRingDescriptor*) available;  // Last available (free) descriptor
  ATOMIC(struct FastRingDescriptor*) pending;    // Last pending descriptor prepared for submission
  struct FastRingDescriptor* submitting;         // Next descriptor to submit

  uint32_t limit;                                // Limit for registered files
  ATOMIC(uint16_t) groups;                       // Count of buffer rings (GetFastRingBufferGroup)
  struct FastRingFileList files;                 // List of watching file descriptors (Poll API)
  struct FastRingFlushList flushers;             // List of flush handlers (SetFastRingFlushHandler)
  struct FastRingBufferList buffers;             // List of registered buffers (Registered Buffer API)
};

int WaitForFastRing(struct FastRing* ring, uint32_t interval, sigset_t* mask);

void PrepareFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptor(struct FastRingDescriptor* descriptor, int option);
void SubmitFastRingDescriptorRange(struct FastRingDescriptor* first, struct FastRingDescriptor* last);
struct FastRingDescriptor* AllocateFastRingDescriptor(struct FastRing* ring, HandleFastRingEventFunction function, void* closure);

// Note: ReleaseFastRingDescriptor has to be used only in special cases, normally release will be done automatically by result of HandleFastRingEventFunction
void ReleaseFastRingDescriptor(struct FastRingDescriptor* descriptor);

int SetFastRingFlushHandler(struct FastRing* ring, HandleFlushFunction function, void* closure);
void RemoveFastRingFlushHandler(struct FastRing* ring, int number);

uint16_t GetFastRingBufferGroup(struct FastRing* ring);
int IsFastRingThread(struct FastRing* ring);

struct FastRing* CreateFastRing(uint32_t length);
void ReleaseFastRing(struct FastRing* ring);

// Poll

#define RING_POLL_FLAGS_SHIFT  32

// There is bug in io_uring (at least, Kernel 6.1 and less): IORING_POLL_ADD_LEVEL is not supported!
// https://github.com/axboe/liburing/issues/829
// #define USE_RING_LEVEL_TRIGGERING

#ifndef USE_RING_LEVEL_TRIGGERING
#define RING_POLL_EDGE    ((uint64_t)IORING_POLL_ADD_LEVEL << RING_POLL_FLAGS_SHIFT)
#else
#define RING_POLL_EDGE    0ULL
#endif

#define RING_POLL_SHOT    ((uint64_t)IORING_POLL_ADD_MULTI << RING_POLL_FLAGS_SHIFT)
#define RING_POLL_READ    (uint64_t)POLLIN
#define RING_POLL_WRITE   (uint64_t)POLLOUT
#define RING_POLL_ERROR   (uint64_t)POLLERR
#define RING_POLL_HANGUP  (uint64_t)POLLHUP

int AddFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandlePollEventFunction function, void* closure);
int ModifyFastRingPoll(struct FastRing* ring, int handle, uint64_t flags);
int RemoveFastRingPoll(struct FastRing* ring, int handle);
void DestroyFastRingPoll(struct FastRing* ring, HandlePollEventFunction function, void* closure);

int ManageFastRingPoll(struct FastRing* ring, int handle, uint64_t flags, HandlePollEventFunction function, void* closure);
void* GetFastRingPollData(struct FastRing* ring, int handle);

// Timeout

#ifndef IORING_TIMEOUT_MULTISHOT
#define TIMEOUT_FLAG_REPEAT  (1ULL << 32)
#else
#define TIMEOUT_FLAG_REPEAT  IORING_TIMEOUT_MULTISHOT
#endif

struct FastRingDescriptor* SetFastRingTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, int64_t interval, uint64_t flags, HandleTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingCertainTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timeval* interval, uint64_t flags, HandleTimeoutFunction function, void* closure);
struct FastRingDescriptor* SetFastRingPreciseTimeout(struct FastRing* ring, struct FastRingDescriptor* descriptor, struct timespec* interval, uint64_t flags, HandleTimeoutFunction function, void* closure);

// Buffer Provider

typedef void* (*CreateRingBufferFunction)(size_t length, void* closure);
typedef void (*ReleaseRingBufferFunction)(void* buffer);

struct FastRingBufferProvider
{
  struct FastRing* ring;
  struct io_uring_buf_reg registration;
  struct io_uring_buf_ring* data;
  uint32_t length;
  uintptr_t map[0];
};

struct FastRingBufferProvider* CreateFastRingBufferProvider(struct FastRing* ring, uint16_t group, uint16_t count, uint32_t length, CreateRingBufferFunction function, void* closure);
void ReleaseFastRingBufferProvider(struct FastRingBufferProvider* provider, ReleaseRingBufferFunction function);

void PrepareFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_sqe* submission);
uint8_t* GetFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion);
void AdvanceFastRingBuffer(struct FastRingBufferProvider* provider, struct io_uring_cqe* completion, CreateRingBufferFunction function, void* closure);

// Registered File

int AddFastRingRegisteredFile(struct FastRing* ring, int handle);
void RemoveFastRingRegisteredFile(struct FastRing* ring, int handle);

// Registered Buffer

int AddFastRingRegisteredBuffer(struct FastRing* ring, void* address, size_t length);
int UpdateFastRingRegisteredBuffer(struct FastRing* ring, int index, void* address, size_t length);

#ifdef __cplusplus
}

inline std::shared_ptr<struct FastRing> CreateSharedFastRing(uint32_t length = 0)
{
  return std::shared_ptr<struct FastRing>(CreateFastRing(length), ReleaseFastRing);
}

#endif

#endif
