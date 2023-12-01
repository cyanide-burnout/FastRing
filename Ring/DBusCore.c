// http://lists.freedesktop.org/archives/dbus/2005-June/002764.html
// http://illumination.svn.sourceforge.net/viewvc/illumination/trunk/Illumination/src/Lum/OS/X11/Display.cpp?revision=808&view=markup

#define DBusCore  struct Context

#include "DBusCore.h"

#include <malloc.h>

struct Context
{
  struct FastRing* ring;
  DBusConnection* connection;
  struct FastRingDescriptor* descriptor;
};

static void HandleWatchEvent(int handle, uint32_t flags, void* data, uint64_t options)
{
  /*
    FastPoll supports only one handler per file descriptor,
    but D-BUS mostly uses separated watches for READ and WRITE operations.
    https://gitlab.freedesktop.org/dbus/dbus/-/blob/master/dbus/dbus-transport-socket.c#L1291
  */

  uint32_t mask;
  uint32_t value;
  DBusWatch** list;

  list  = (DBusWatch**)data;
  value =
    (((flags & RING_EVENT_READ)   > 0) * DBUS_WATCH_READABLE) |
    (((flags & RING_EVENT_WRITE)  > 0) * DBUS_WATCH_WRITABLE) |
    (((flags & RING_EVENT_ERROR)  > 0) * DBUS_WATCH_ERROR)    |
    (((flags & RING_EVENT_HANGUP) > 0) * DBUS_WATCH_HANGUP);

  if (list[0] != NULL)
  {
    mask  = dbus_watch_get_flags(list[0]);
    mask |= DBUS_WATCH_ERROR | DBUS_WATCH_HANGUP;
    if (value & mask)
    {
      // Call watch handler only in suitable condition
      dbus_watch_handle(list[0], value & mask);
    }
  }

  if (list[1] != NULL)
  {
    mask  = dbus_watch_get_flags(list[1]);
    mask |= DBUS_WATCH_ERROR | DBUS_WATCH_HANGUP;
    if (value & mask)
    {
      // Call watch handler only in suitable condition
      dbus_watch_handle(list[1], value & mask);
    }
  }
}

static void UpdateWatchHandler(struct Context* context, int handle, DBusWatch** list)
{
  uint32_t flags;
  uint64_t mask;

  mask = 0;

  if (list[0] != NULL)
  {
    flags  = dbus_watch_get_flags(list[0]);
    mask  |=
      RING_EVENT_ERROR | RING_EVENT_HANGUP |
      (((flags & DBUS_WATCH_READABLE) > 0) * RING_EVENT_READ) |
      (((flags & DBUS_WATCH_WRITABLE) > 0) * RING_EVENT_WRITE);
  }

  if (list[1] != NULL)
  {
    flags  = dbus_watch_get_flags(list[1]);
    mask  |=
      RING_EVENT_ERROR | RING_EVENT_HANGUP |
      (((flags & DBUS_WATCH_READABLE) > 0) * RING_EVENT_READ) |
      (((flags & DBUS_WATCH_WRITABLE) > 0) * RING_EVENT_WRITE);
  }

  ManageFastRingEventHandler(context->ring, handle, mask, HandleWatchEvent, list);
}

static dbus_bool_t AddWatch(DBusWatch* watch, void* data)
{
  struct Context* context;
  DBusWatch** list;
  int handle;

  if (dbus_watch_get_enabled(watch))
  {
    context = (struct Context*)data;
    handle  = dbus_watch_get_unix_fd(watch);
    list    = (DBusWatch**)GetFastRingEventHandlerData(context->ring, handle);

    if (list == NULL)
    {
      // Create a new list when not exists
      list = (DBusWatch**)calloc(2, sizeof(DBusWatch*));
    }

    // DBusCore supports up to two watches per handle
    list[list[0] != NULL] = watch;

    dbus_watch_set_data(watch, list, NULL);
    UpdateWatchHandler(context, handle, list);
  }

  return TRUE;
}

static void RemoveWatch(DBusWatch* watch, void* data)
{
  struct Context* context;
  DBusWatch** list;
  int handle;

  if (list = (DBusWatch**)dbus_watch_get_data(watch))
  {
    context = (struct Context*)data;
    handle  = dbus_watch_get_unix_fd(watch);

    // DBusCore supports up to two watches per handle
    list[list[0] != watch] = NULL;

    dbus_watch_set_data(watch, NULL, NULL);
    UpdateWatchHandler(context, handle, list);

    if ((list[0] == NULL) &&
        (list[1] == NULL))
    {
      // Release the list when all watches removed
      free(list);
    }
  }
}

static void ToggleWatch(DBusWatch* watch, void* data)
{
  if (dbus_watch_get_enabled(watch))
  {
    AddWatch(watch, data);
    return;
  }

  RemoveWatch(watch, data);
}

static void HandleTimoutEvent(struct FastRingDescriptor* descriptor)
{
  DBusTimeout* timeout;

  timeout = (DBusTimeout*)descriptor->closure;

  dbus_timeout_set_data(timeout, NULL, NULL);
  dbus_timeout_handle(timeout);
}

static dbus_bool_t AddTimeout(DBusTimeout* timeout, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct Context* context;
  int interval;

  if (dbus_timeout_get_enabled(timeout))
  {
    context    = (struct Context*)data;
    interval   = dbus_timeout_get_interval(timeout);
    descriptor = SetFastRingTimeout(context->ring, NULL, interval, 0, HandleTimoutEvent, timeout);
    dbus_timeout_set_data(timeout, descriptor, NULL);
  }

  return TRUE;
}

static void RemoveTimeout(DBusTimeout* timeout, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct Context* context;

  if (descriptor = (struct FastRingDescriptor*)dbus_timeout_get_data(timeout))
  {
    context = (struct Context*)data;
    SetFastRingTimeout(context->ring, descriptor, -1, 0, NULL, NULL);
    dbus_timeout_set_data(timeout, NULL, NULL);
  }
}

static void ToggleTimeout(DBusTimeout* timeout, void* data)
{
  if (dbus_timeout_get_enabled(timeout))
  {
    AddTimeout(timeout, data);
    return;
  }

  RemoveTimeout(timeout, data);
}

static void HandleDispatchEvent(struct FastRingDescriptor* descriptor)
{
  struct Context* context;
  uint64_t value;

  context             = (struct Context*)descriptor->closure;
  context->descriptor = NULL;

  if ((dbus_connection_dispatch(context->connection) != DBUS_DISPATCH_COMPLETE) &&
      (context->descriptor == NULL))
  {
    // Schedule one-time call of dbus_connection_dispatch
    context->descriptor = SetFastRingTimeout(context->ring, NULL, 0, 0, HandleDispatchEvent, context);
  }
}

static void HandleDispatchStatus(DBusConnection* connection, DBusDispatchStatus status, void* data)
{
  struct Context* context;
  uint64_t value;

  context = (struct Context*)data;

  if ((status != DBUS_DISPATCH_COMPLETE) &&
      (context->descriptor == NULL))
  {
    // Schedule one-time call of dbus_connection_dispatch
    context->descriptor = SetFastRingTimeout(context->ring, NULL, 0, 0, HandleDispatchEvent, context);
  }
}

struct Context* CreateDBusCore(DBusConnection* connection, struct FastRing* ring)
{
  struct Context* context;

  context = (struct Context*)calloc(1, sizeof(struct Context));

  context->ring       = ring;
  context->connection = connection;
  context->descriptor = SetFastRingTimeout(ring, NULL, 0, 0, HandleDispatchEvent, context);

  dbus_connection_ref(connection);
  dbus_connection_set_dispatch_status_function(connection, HandleDispatchStatus, context, NULL);
  dbus_connection_set_watch_functions(connection, AddWatch, RemoveWatch, ToggleWatch, context, NULL);
  dbus_connection_set_timeout_functions(connection, AddTimeout, RemoveTimeout, ToggleTimeout, context, NULL);

  return context;
}

void ReleaseDBusCore(struct Context* context)
{
  SetFastRingTimeout(context->ring, context->descriptor, -1, 0, NULL, NULL);

  dbus_connection_set_timeout_functions(context->connection, NULL, NULL, NULL, NULL, NULL);
  dbus_connection_set_watch_functions(context->connection, NULL, NULL, NULL, NULL, NULL);
  dbus_connection_set_dispatch_status_function(context->connection, NULL, NULL, NULL);
  dbus_connection_unref(context->connection);

  free(context);
}
