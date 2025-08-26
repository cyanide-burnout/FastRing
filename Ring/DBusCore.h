#ifndef DBUSCORE_H
#define DBUSCORE_H

#include <dbus/dbus.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct DBusCore;

struct DBusCore* CreateDBusCore(DBusConnection* connection, struct FastRing* ring);
void ReleaseDBusCore(struct DBusCore* core);

#ifdef __cplusplus
}
#endif

#endif
