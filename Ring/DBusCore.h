#ifndef DBUSCORE_H
#define DBUSCORE_H

#include <dbus/dbus.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef DBusCore
typedef void DBusCore;
#endif

DBusCore* CreateDBusCore(DBusConnection* connection, struct FastRing* ring);
void ReleaseDBusCore(DBusCore* context);

#ifdef __cplusplus
}
#endif

#endif
