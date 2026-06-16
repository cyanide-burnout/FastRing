#ifndef SAMBAR_H
#define SAMBAR_H

#include <stddef.h>
#include <smb2/libsmb2.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define GetSambarData(context, type, field)  ((type*)GetSambarClosure(context, offsetof(type, field)))

struct SambarOpaque
{
  struct FastRing* ring;
  struct FastRingDescriptor* timeout;
};

void AttachSambarOpaque(struct SambarOpaque* opaque, struct FastRing* ring, struct smb2_context* context);
void DetachSambarOpaque(struct SambarOpaque* opaque);

void* GetSambarClosure(struct smb2_context* context, int offset);


typedef void (*HandleSambarAcceptFunction)(struct smb2_context* context, void* closure);

struct SambarServer
{
  int handle;
  void* closure;
  struct FastRing* ring;
  HandleSambarAcceptFunction function;
};

struct SambarServer* CreateSambarServer(struct FastRing* ring, uint16_t port, HandleSambarAcceptFunction function, void* closure);
void ReleaseSambarServer(struct SambarServer* server);

#ifdef __cplusplus
}
#endif

#endif
