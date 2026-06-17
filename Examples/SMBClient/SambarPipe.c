#include "SambarPipe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

static int ArmRead(struct SambarPipe* pipe);
static void ArmShutdown(struct SambarPipe* pipe);

static int GetPipeError(void)
{
  return (errno != 0) ? -errno : -EIO;
}

static void RetainPipe(struct SambarPipe* pipe)
{
  if (pipe != NULL)  pipe->count ++;
}

static void ReleasePipe(struct SambarPipe* pipe)
{
  if ((pipe != NULL) &&
      (--pipe->count == 0))
  {
    free(pipe->buffer);
    free(pipe->path);
    free(pipe);
  }
}

static void CallFunction(struct SambarPipe* pipe, int event, int status, void* data, uint32_t length)
{
  if ((pipe != NULL) && (pipe->function != NULL))
  {
    pipe->function(pipe->closure, event, status, data, length);
  }
}

static void FinishPipe(struct SambarPipe* pipe)
{
  if (pipe != NULL)
  {
    if (pipe->context != NULL)
    {
      DetachSambarOpaque(&pipe->super, pipe->context);
      smb2_destroy_context(pipe->context);
    }

    pipe->context = NULL;
    pipe->handle  = NULL;

    if (( pipe->flags & SAMBAR_PIPE_FLAG_TERMINAL) &&
        (~pipe->flags & SAMBAR_PIPE_FLAG_NOTIFIED))
    {
      pipe->flags |= SAMBAR_PIPE_FLAG_NOTIFIED;
      CallFunction(pipe, pipe->event, pipe->status, NULL, 0);
    }

    ReleasePipe(pipe);
  }
}

static void HandleFinish(void* closure, int reason)
{
  struct SambarPipe* pipe;

  if ((reason == RING_REASON_COMPLETE) &&
      (pipe = (struct SambarPipe*)closure))
  {
    pipe->flags &= ~SAMBAR_PIPE_FLAG_FINISH;
    FinishPipe(pipe);
    ReleasePipe(pipe);
  }
}

static void HandleDisconnect(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipe* pipe;

  if (pipe = (struct SambarPipe*)closure)
  {
    pipe->flags &= ~SAMBAR_PIPE_FLAG_DISCONNECT;
    ArmShutdown(pipe);
    ReleasePipe(pipe);
  }
}

static void HandleClose(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipe* pipe;

  if (pipe = (struct SambarPipe*)closure)
  {
    pipe->flags  &= ~(SAMBAR_PIPE_FLAG_HANDLE_CLOSE | SAMBAR_PIPE_FLAG_OPEN);
    pipe->handle  = NULL;
    ArmShutdown(pipe);
    ReleasePipe(pipe);
  }
}

static void ArmShutdown(struct SambarPipe* pipe)
{
  if (pipe == NULL)
  {
    return;
  }

  if (pipe->flags & (SAMBAR_PIPE_FLAG_HANDLE_CLOSE | SAMBAR_PIPE_FLAG_DISCONNECT | SAMBAR_PIPE_FLAG_FINISH))
  {
    return;
  }

  if (pipe->context == NULL)
  {
    if ((pipe->super.ring != NULL) &&
        SetFastRingFlushHandler(pipe->super.ring, HandleFinish, pipe))
    {
      RetainPipe(pipe);
      pipe->flags |= SAMBAR_PIPE_FLAG_FINISH;
      return;
    }

    FinishPipe(pipe);
    return;
  }

  if ((pipe->handle != NULL) &&
      (~pipe->flags & SAMBAR_PIPE_FLAG_HANDLE_CLOSE) &&
      smb2_context_active(pipe->context))
  {
    pipe->flags |= SAMBAR_PIPE_FLAG_HANDLE_CLOSE;

    if (smb2_close_async(pipe->context, pipe->handle, HandleClose, pipe) == 0)
    {
      RetainPipe(pipe);
      return;
    }

    pipe->flags  &= ~(SAMBAR_PIPE_FLAG_HANDLE_CLOSE | SAMBAR_PIPE_FLAG_OPEN);
    pipe->handle  = NULL;
  }

  if (( pipe->flags & SAMBAR_PIPE_FLAG_CONNECTED)  &&
      (~pipe->flags & SAMBAR_PIPE_FLAG_DISCONNECT) &&
      smb2_context_active(pipe->context))
  {
    pipe->flags |= SAMBAR_PIPE_FLAG_DISCONNECT;

    if (smb2_disconnect_share_async(pipe->context, HandleDisconnect, pipe) == 0)
    {
      RetainPipe(pipe);
      return;
    }

    pipe->flags &= ~SAMBAR_PIPE_FLAG_DISCONNECT;
  }

  if ((pipe->super.ring != NULL) &&
      SetFastRingFlushHandler(pipe->super.ring, HandleFinish, pipe))
  {
    RetainPipe(pipe);
    pipe->flags |= SAMBAR_PIPE_FLAG_FINISH;
    return;
  }

  FinishPipe(pipe);
}

static void CloseInternal(struct SambarPipe* pipe, int event, int status)
{
  if ((pipe != NULL) &&
      (~pipe->flags & SAMBAR_PIPE_FLAG_TERMINAL))
  {
    pipe->flags |= SAMBAR_PIPE_FLAG_TERMINAL | SAMBAR_PIPE_FLAG_CLOSING;
    pipe->event  = event;
    pipe->status = status;
    ArmShutdown(pipe);
  }
}

static void HandleRead(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipe* pipe;

  if (pipe = (struct SambarPipe*)closure)
  {
    pipe->flags &= ~SAMBAR_PIPE_FLAG_READ_ARMED;

    if (status > 0)
    {
      CallFunction(pipe, SAMBAR_PIPE_READ, 0, pipe->buffer, (uint32_t)status);
      status = ArmRead(pipe);
    }

    if ((status == 0) && (~pipe->flags & SAMBAR_PIPE_FLAG_CLOSING))
    {
      CloseInternal(pipe, SAMBAR_PIPE_CLOSE, 0);
    }

    if ((status < 0) && (~pipe->flags & SAMBAR_PIPE_FLAG_CLOSING))
    {
      CloseInternal(pipe, SAMBAR_PIPE_ERROR, status);
    }

    ReleasePipe(pipe);
  }
}

static int ArmRead(struct SambarPipe* pipe)
{
  if ((pipe == NULL) || (pipe->handle == NULL) || (pipe->flags & (SAMBAR_PIPE_FLAG_CLOSING | SAMBAR_PIPE_FLAG_TERMINAL)))  return 0;
  if (pipe->flags & SAMBAR_PIPE_FLAG_READ_ARMED)                                                                              return 1;

  if (pipe->buffer == NULL)
  {
    pipe->length = smb2_get_max_read_size(pipe->context);
    pipe->length = (pipe->length == 0) ? 4096U : pipe->length;

    if (!(pipe->buffer = (uint8_t*)malloc(pipe->length)))
    {
      // Fatal error
      return -ENOMEM;
    }
  }

  pipe->flags |= SAMBAR_PIPE_FLAG_READ_ARMED;

  if (smb2_read_async(pipe->context, pipe->handle, pipe->buffer, pipe->length, HandleRead, pipe) < 0)
  {
    pipe->flags &= ~SAMBAR_PIPE_FLAG_READ_ARMED;
    return GetPipeError();
  }

  RetainPipe(pipe);
  return 1;
}

static void HandleWrite(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipeWrite* write;
  struct SambarPipe* pipe;
  uint32_t length;

  if (write = (struct SambarPipeWrite*)closure)
  {
    pipe   = write->pipe;
    length = (status > 0) ? status : 0U;

    free(write);
    CallFunction(pipe, SAMBAR_PIPE_WRITE, status, NULL, length);

    if ((status < 0) && (~pipe->flags & SAMBAR_PIPE_FLAG_CLOSING))
    {
      CloseInternal(pipe, SAMBAR_PIPE_ERROR, status);
    }

    ReleasePipe(pipe);
  }
}

static void HandleOpen(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipe* pipe;

  if (pipe = (struct SambarPipe*)closure)
  {
    if (status >= 0)
    {
      pipe->handle  = (struct smb2fh*)data;
      pipe->flags  |= SAMBAR_PIPE_FLAG_CONNECTED | SAMBAR_PIPE_FLAG_OPEN;

      CallFunction(pipe, SAMBAR_PIPE_CONNECT, 0, NULL, 0);

      if ((pipe->flags & (SAMBAR_PIPE_FLAG_CLOSING | SAMBAR_PIPE_FLAG_TERMINAL)) == 0)
      {
        status = ArmRead(pipe);
      }
    }

    if (status < 0)
    {
      CloseInternal(pipe, SAMBAR_PIPE_ERROR, status);
    }

    ReleasePipe(pipe);
  }
}

static void HandleConnect(struct smb2_context* context, int status, void* data, void* closure)
{
  struct SambarPipe* pipe;

  if (pipe = (struct SambarPipe*)closure)
  {
    if (status >= 0)
    {
      pipe->flags |= SAMBAR_PIPE_FLAG_CONNECTED;

      if (smb2_open_async(pipe->context, pipe->path, O_RDWR, HandleOpen, pipe) == 0)
      {
        RetainPipe(pipe);
        status = 0;
      }
      else
      {
        status = GetPipeError();
      }
    }

    if (status < 0)
    {
      CloseInternal(pipe, SAMBAR_PIPE_ERROR, status);
    }

    ReleasePipe(pipe);
  }
}

struct SambarPipe* OpenSambarPipe(struct FastRing* ring, const char* address, const char* user, const char* password, const char* path, HandleSambarPipeFunction function, void* closure)
{
  struct SambarPipe* pipe;

  if ((pipe          = (struct SambarPipe*)calloc(1, sizeof(struct SambarPipe))) &&
      (pipe->context = smb2_init_context()))
  {
    pipe->count    = 1;
    pipe->path     = strdup(path);
    pipe->closure  = closure;
    pipe->function = function;

    smb2_set_user(pipe->context, user);
    smb2_set_password(pipe->context, password);
    AttachSambarOpaque(&pipe->super, ring, pipe->context, 0);

    if ((pipe->path != NULL) &&
        (smb2_connect_share_async(pipe->context, address, "IPC$", user, HandleConnect, pipe) == 0))
    {
      pipe->count ++;
      return pipe;
    }

    DetachSambarOpaque(&pipe->super, pipe->context);
    smb2_destroy_context(pipe->context);
    free(pipe->path);
  }

  free(pipe);

  return NULL;
}

int WriteSambarPipe(struct SambarPipe* pipe, const void* data, uint32_t length)
{
  struct SambarPipeWrite* write;
  int result;

  if ((pipe == NULL) || (pipe->context == NULL) || (pipe->handle == NULL) || (length > 0) && (data == NULL))  return -EINVAL;
  if (pipe->flags & SAMBAR_PIPE_FLAG_CLOSING)                                                                 return -EPIPE;
  if (!(write = (struct SambarPipeWrite*)malloc(sizeof(struct SambarPipeWrite) + length)))                    return -ENOMEM;

  write->pipe   = pipe;
  write->length = length;

  memcpy(write->data, data, length);

  result       = smb2_write_async(pipe->context, pipe->handle, write->data, length, HandleWrite, write);
  pipe->count += (result >= 0);

  if (result < 0)
  {
    result = GetPipeError();
    free(write);
  }

  return result;
}

void CloseSambarPipe(struct SambarPipe* pipe)
{
  if ((pipe != NULL) &&
      (~pipe->flags & SAMBAR_PIPE_FLAG_TERMINAL))
  {
    //
    CloseInternal(pipe, SAMBAR_PIPE_CLOSE, 0);
  }
}
