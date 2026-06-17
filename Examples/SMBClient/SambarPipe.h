#ifndef SAMBARPIPE_H
#define SAMBARPIPE_H

#include "SambarAdapter.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SAMBAR_PIPE_FLAG_CONNECTED     (1 << 0)
#define SAMBAR_PIPE_FLAG_OPEN          (1 << 1)
#define SAMBAR_PIPE_FLAG_CLOSING       (1 << 2)
#define SAMBAR_PIPE_FLAG_TERMINAL      (1 << 3)
#define SAMBAR_PIPE_FLAG_RELEASE       (1 << 4)
#define SAMBAR_PIPE_FLAG_READ_ARMED    (1 << 5)
#define SAMBAR_PIPE_FLAG_HANDLE_CLOSE  (1 << 6)
#define SAMBAR_PIPE_FLAG_DISCONNECT    (1 << 7)
#define SAMBAR_PIPE_FLAG_NOTIFIED      (1 << 8)
#define SAMBAR_PIPE_FLAG_FINISH        (1 << 9)

#define SAMBAR_PIPE_CONNECT  0
#define SAMBAR_PIPE_READ     1
#define SAMBAR_PIPE_WRITE    2
#define SAMBAR_PIPE_CLOSE    3
#define SAMBAR_PIPE_ERROR    4

typedef void (*HandleSambarPipeFunction)(void* closure, int event, int status, void* data, uint32_t length);

struct SambarPipe
{
  struct SambarOpaque super;
  struct smb2_context* context;
  struct smb2fh* handle;

  HandleSambarPipeFunction function;
  void* closure;

  uint32_t length;
  uint8_t* buffer;
  char* path;
  int count;
  int flags;
  int event;
  int status;
};

struct SambarPipeWrite
{
  struct SambarPipe* pipe;
  uint32_t length;
  uint8_t data[0];
};

struct SambarPipe* OpenSambarPipe(struct FastRing* ring, const char* address, const char* user, const char* password, const char* path, HandleSambarPipeFunction function, void* closure);
int WriteSambarPipe(struct SambarPipe* pipe, const void* data, uint32_t length);
void CloseSambarPipe(struct SambarPipe* pipe);

#ifdef __cplusplus
}
#endif

#endif
