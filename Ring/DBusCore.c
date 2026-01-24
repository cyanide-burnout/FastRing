// http://lists.freedesktop.org/archives/dbus/2005-June/002764.html
// http://illumination.svn.sourceforge.net/viewvc/illumination/trunk/Illumination/src/Lum/OS/X11/Display.cpp?revision=808&view=markup

#include "DBusCore.h"

#include <malloc.h>

struct DBusCore
{
  struct FastRing* ring;
  struct FastRingFlusher* flusher;
  DBusConnection* connection;
};

static void HandleWatchEvent(struct FastRingDescriptor* descriptor, int result)
{
  int handle;
  uint32_t flags;
  DBusWatch* watch;

  if (watch = (DBusWatch*)descriptor->closure)
  {
    flags =
      ((result > 0) *
       ((((result & POLLIN)  != 0) * DBUS_WATCH_READABLE) |
        (((result & POLLOUT) != 0) * DBUS_WATCH_WRITABLE) |
        (((result & POLLERR) != 0) * DBUS_WATCH_ERROR)    |
        (((result & POLLHUP) != 0) * DBUS_WATCH_HANGUP))) |
      ((result < 0) * DBUS_WATCH_ERROR);

    dbus_watch_handle(watch, flags);
  }
}

static dbus_bool_t AddWatch(DBusWatch* watch, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct DBusCore* core;
  uint32_t flags;
  int handle;

  if (!dbus_watch_get_enabled(watch))
  {
    // Watch is created in disabled state
    return TRUE;
  }

  core   = (struct DBusCore*)data;
  handle = dbus_watch_get_unix_fd(watch);
  flags  = dbus_watch_get_flags(watch);
  flags  =
    (((flags & DBUS_WATCH_READABLE) != 0) * POLLIN)  |
    (((flags & DBUS_WATCH_WRITABLE) != 0) * POLLOUT) |
    (((flags & DBUS_WATCH_ERROR)    != 0) * POLLERR) |
    (((flags & DBUS_WATCH_HANGUP)   != 0) * POLLHUP);

  if (descriptor = AddFastRingWatch(core->ring, handle, flags, HandleWatchEvent, watch))
  {
    dbus_watch_set_data(watch, descriptor, NULL);
    return TRUE;
  }

  return FALSE;
}

static void RemoveWatch(DBusWatch* watch, void* data)
{
  struct FastRingDescriptor* descriptor;

  if (descriptor = (struct FastRingDescriptor*)dbus_watch_get_data(watch))
  {
    dbus_watch_set_data(watch, NULL, NULL);
    RemoveFastRingWatch(descriptor);
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

  dbus_timeout_handle(timeout);
}

static dbus_bool_t AddTimeout(DBusTimeout* timeout, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct DBusCore* core;
  int interval;

  if (!dbus_timeout_get_enabled(timeout))
  {
    // Timeout is created in disabled state
    return TRUE;
  }

  core     = (struct DBusCore*)data;
  interval = dbus_timeout_get_interval(timeout);

  if (descriptor = SetFastRingTimeout(core->ring, NULL, interval, TIMEOUT_FLAG_REPEAT, HandleTimoutEvent, timeout))
  {
    dbus_timeout_set_data(timeout, descriptor, NULL);
    return TRUE;
  }

  return FALSE;
}

static void RemoveTimeout(DBusTimeout* timeout, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct DBusCore* core;

  if (descriptor = (struct FastRingDescriptor*)dbus_timeout_get_data(timeout))
  {
    core = (struct DBusCore*)data;
    SetFastRingTimeout(core->ring, descriptor, -1, 0, NULL, NULL);
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

static void HandleDispatchEvent(void* closure, int reason)
{
  struct DBusCore* core;

  if (reason == RING_REASON_COMPLETE)
  {
    core          = (struct DBusCore*)closure;
    core->flusher = NULL;

    if ((dbus_connection_dispatch(core->connection) != DBUS_DISPATCH_COMPLETE) &&
        (core->flusher == NULL))
    {
      // Schedule one-time call of dbus_connection_dispatch
      core->flusher = SetFastRingFlushHandler(core->ring, HandleDispatchEvent, core);
    }
  }
}

static void HandleDispatchStatus(DBusConnection* connection, DBusDispatchStatus status, void* data)
{
  struct DBusCore* core;

  core = (struct DBusCore*)data;

  if ((status != DBUS_DISPATCH_COMPLETE) &&
      (core->flusher == NULL))
  {
    // Schedule one-time call of dbus_connection_dispatch
    core->flusher = SetFastRingFlushHandler(core->ring, HandleDispatchEvent, core);
  }
}

struct DBusCore* CreateDBusCore(DBusConnection* connection, struct FastRing* ring)
{
  struct DBusCore* core;

  core = NULL;

  if ((connection != NULL) &&
      (core = (struct DBusCore*)calloc(1, sizeof(struct DBusCore))))
  {
    core->ring       = ring;
    core->connection = connection;
    core->flusher    = SetFastRingFlushHandler(core->ring, HandleDispatchEvent, core);

    dbus_connection_ref(connection);
    dbus_connection_set_dispatch_status_function(connection, HandleDispatchStatus, core, NULL);
    dbus_connection_set_watch_functions(connection, AddWatch, RemoveWatch, ToggleWatch, core, NULL);
    dbus_connection_set_timeout_functions(connection, AddTimeout, RemoveTimeout, ToggleTimeout, core, NULL);
  }

  return core;
}

void ReleaseDBusCore(struct DBusCore* core)
{
  if (core != NULL)
  {
    RemoveFastRingFlushHandler(core->ring, core->flusher);

    dbus_connection_set_timeout_functions(core->connection, NULL, NULL, NULL, NULL, NULL);
    dbus_connection_set_watch_functions(core->connection, NULL, NULL, NULL, NULL, NULL);
    dbus_connection_set_dispatch_status_function(core->connection, NULL, NULL, NULL);
    dbus_connection_unref(core->connection);

    free(core);
  }
}
