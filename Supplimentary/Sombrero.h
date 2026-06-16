#ifndef SOMBRERO_H
#define SOMBRERO_H

#include "Sambar.h"

#include <time.h>
#include <stdint.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct SombreroCore;

typedef void (*HandleSombreroContextFunction)(struct SombreroCore* core, struct smb2_context* context, void* closure);

struct SombreroCore
{
  void* closure;
  struct FastRing* ring;
  struct smb2_server server;
  struct smb2_server_request_handlers handlers;
  HandleSombreroContextFunction function;
};

void PrepareSombreroCore(struct SombreroCore* core, HandleSombreroContextFunction function, void* closure);
void HandleSombreroAccept(struct smb2_context* context, void* closure);

#ifdef __cplusplus
}
#endif

#endif
