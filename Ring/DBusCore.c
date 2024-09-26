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

static int HandleWatchEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  int handle;
  uint32_t flags;
  DBusWatch* watch;

  if ((completion != NULL) &&
      (watch = (DBusWatch*)descriptor->closure) &&
      (~completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    descriptor->data.number = 1;
    flags                   =
      ((completion->res > 0) *
       ((((completion->res & POLLIN)  != 0) * DBUS_WATCH_READABLE) |
        (((completion->res & POLLOUT) != 0) * DBUS_WATCH_WRITABLE) |
        (((completion->res & POLLERR) != 0) * DBUS_WATCH_ERROR)    |
        (((completion->res & POLLHUP) != 0) * DBUS_WATCH_HANGUP))) |
      ((completion->res < 0) * DBUS_WATCH_ERROR);

    dbus_watch_handle(watch, flags);

    if (descriptor->data.number != 0)
    {
      // Watch didn't clear resubmission flag
      // Clear flag to indicate an exit from HandleWatchEvent()
      descriptor->data.number = 0;

      if (descriptor->submission.opcode != IORING_OP_POLL_ADD)
      {
        // Last SQE was about io_uring_prep_poll_update()
        io_uring_prep_rw(IORING_OP_POLL_ADD, &descriptor->submission, -1, NULL, 0, 0);
        descriptor->submission.fd         = dbus_watch_get_unix_fd(watch);
        descriptor->submission.user_data &= ~RING_DESC_OPTION_IGNORE;
      }

      if (descriptor->state == RING_DESC_STATE_SUBMITTED)
      {
        // Since D-BUS doesn't support edge triggering
        // We have to submit each time after completion
        SubmitFastRingDescriptor(descriptor, 0);
        return 1;
      }
    }
  }

  return 0;
}

static dbus_bool_t AddWatch(DBusWatch* watch, void* data)
{
  struct FastRingDescriptor* descriptor;
  struct Context* context;
  uint32_t flags;
  int handle;

  if (!dbus_watch_get_enabled(watch))
  {
    // Watch is created in disabled state
    return TRUE;
  }

  context = (struct Context*)data;
  handle  = dbus_watch_get_unix_fd(watch);
  flags   = dbus_watch_get_flags(watch);
  flags   =
    (((flags & DBUS_WATCH_READABLE) != 0) * POLLIN)  |
    (((flags & DBUS_WATCH_WRITABLE) != 0) * POLLOUT) |
    (((flags & DBUS_WATCH_ERROR)    != 0) * POLLERR) |
    (((flags & DBUS_WATCH_HANGUP)   != 0) * POLLHUP);

  if (descriptor = (struct FastRingDescriptor*)dbus_watch_get_data(watch))
  {
    if ((descriptor->data.number != 0) ||                                            // We are inside a call to HandleWatchEvent()
        (descriptor->state == RING_DESC_STATE_PENDING) ||                            // There is a pending io_uring_prep_poll_add() or io_uring_prep_poll_update()
        (descriptor->submission.poll32_events == __io_uring_prep_poll_mask(flags)))  // Poll mask has no changes
    {
      // Existing io_uring_prep_poll_add() or io_uring_prep_poll_update() is not yet submitted to kernel
      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(flags);
      return TRUE;
    }

    // Update any incomplete io_uring_prep_poll_add()
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_poll_update(&descriptor->submission, descriptor->identifier, descriptor->identifier, flags, IORING_POLL_UPDATE_USER_DATA | IORING_POLL_UPDATE_EVENTS);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    return TRUE;
  }

  if (descriptor = AllocateFastRingDescriptor(context->ring, HandleWatchEvent, watch))
  {
    dbus_watch_set_data(watch, descriptor, NULL);
    io_uring_prep_poll_add(&descriptor->submission, handle, flags);
    SubmitFastRingDescriptor(descriptor, 0);
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

    descriptor->function = NULL;
    descriptor->closure  = NULL;

    if ((descriptor->data.number != 0) ||                       // We are inside a call to HandleWatchEvent()
        (descriptor->state == RING_DESC_STATE_PENDING) &&       //
        (descriptor->submission.opcode == IORING_OP_POLL_ADD))  // io_uring_prep_poll_add() is not yet submitted to kernel
    {
      io_uring_prep_nop(&descriptor->submission);
      descriptor->data.number = 0;
      return;
    }

    // Cancel any incomplete io_uring_prep_poll_add()
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
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
