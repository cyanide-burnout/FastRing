#ifndef RADIUSTOOLS_H
#define RADIUSTOOLS_H

#include "RADIUS.h"

#ifdef __cplusplus
extern "C"
{
#endif

int PackRADIUSIntegerAttribute(struct RADIUSAttribute* attribute, uint8_t type, uint32_t value);

void MakeRADIUSAuthenticator(struct RADIUSDataUnit* unit, uint8_t* authenticator, const char* secret);
int CheckRADIUSAuthenticator(struct RADIUSDataUnit* unit, uint8_t* authenticator, const char* secret);

#ifdef __cplusplus
}
#endif

#endif
