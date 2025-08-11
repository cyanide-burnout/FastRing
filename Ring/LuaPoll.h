#ifndef LUAPOLL_H
#define LUAPOLL_H

#include "FastRing.h"

#ifdef __cplusplus
#include <lua.hpp>
#else
#include <lua.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

void RegisterLuaPoll(struct FastRing* ring, lua_State* state, int handler);

#ifdef __cplusplus
}
#endif


#endif