#ifndef SAMBARADAPTER_H
#define SAMBARADAPTER_H

#include "FastRing.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

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

void AttachSambarOpaque(struct SambarOpaque* opaque, struct FastRing* ring, struct smb2_context* context, int force);
void DetachSambarOpaque(struct SambarOpaque* opaque, struct smb2_context* context);
void* GetSambarClosure(struct smb2_context* context, int offset);

struct SambarListener
{
  int handle;
  void* closure;
  smb2_client_connection function;
  struct FastRingDescriptor* accept;
};

struct SambarListener* OpenSambarListener(struct FastRing* ring, uint16_t port, smb2_client_connection function, void* closure);
void CloseSambarListener(struct SambarListener* listener);

#ifdef __cplusplus
}
#endif

#endif
