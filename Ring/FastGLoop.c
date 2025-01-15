#define FASTGLOOP_INTERNAL
#include "FastGLoop.h"

#include <errno.h>
#include <alloca.h>
#include <malloc.h>
#include <syslog.h>
#include <sys/resource.h>

#define FIBER_LOOP     0
#define FIBER_MAIN     1

#define LIST_INCREASE  1024

static void JumpToLoop(struct FastGLoop* loop);
static void HandleRequest(struct FastGLoop* loop);
static int HandleResponse(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);
static void HandleFlush(void* closure, int reason);

// Supplementary

static int ExpandPollData(struct FastGLoop* loop, int handle)
{
  struct FastGLoopPoolData* data;
  uint32_t length;

  handle ++;
  length  = handle + LIST_INCREASE - handle % LIST_INCREASE;

  if ((handle > 0) &&
      (length > loop->length) &&
      (data = (struct FastGLoopPoolData*)realloc(loop->files, length * sizeof(struct FastGLoopPoolData))))
  {
    memset(data + loop->length, 0, (length - loop->length) * sizeof(struct FastGLoopPoolData));
    loop->files  = data;
    loop->length = length;
    return 0;
  }

  return -ENOMEM;
}

// FastRing

static void HandleRequest(struct FastGLoop* loop)
{
  struct FastRingDescriptor* descriptor;
  struct FastRingDescriptor* other;
  struct FastGLoopPoolData* data;
  GPollFD* entry;
  GPollFD* limit;
  int handle;
  int* stack;
  int* top;

  // Build list of unique polls

  loop->cycle ++;

  stack = (int*)alloca(loop->count * sizeof(int));
  top   = stack;

  entry = loop->entries;
  limit = loop->entries + loop->count;

  while (entry < limit)
  {
    if ((entry->fd < loop->length) ||
        (ExpandPollData(loop, entry->fd) == 0))
    {
      data = loop->files + entry->fd;

      if ((data->cycle != loop->cycle) &&
          (entry->events != 0))
      {
        data->result  = 0;
        data->current = 0;
        data->cycle   = loop->cycle;
        *(top ++)     = entry->fd; 
      }

      data->current |= entry->events;
    }

    entry ++;
  }

  // Submit unique polls

  while (top > stack)
  {
    handle = *(-- top);
    data   = loop->files + handle;

    if ((data->previous != data->current)    &&
        (other           = data->descriptor) &&
        (descriptor      = AllocateFastRingDescriptor(loop->ring, HandleResponse, loop)))
    {
      io_uring_prep_poll_remove(&descriptor->submission, other->identifier);
      descriptor->submission.flags |= IOSQE_IO_HARDLINK;
      descriptor->linked            = 2;
      data->descriptor              = NULL;
      SubmitFastRingDescriptor(descriptor, 0);
    }

    if ((data->descriptor == NULL) &&
        (descriptor = AllocateFastRingDescriptor(loop->ring, HandleResponse, loop)))
    {
      io_uring_prep_poll_add(&descriptor->submission, handle, data->current);
      SubmitFastRingDescriptor(descriptor, 0);
      data->descriptor = descriptor;
      data->previous   = data->current;
    }
  }

  // Submit timeout

  if ((other      = loop->descriptor) &&
      (descriptor = AllocateFastRingDescriptor(loop->ring, HandleResponse, loop)))
  {
    io_uring_prep_timeout_update(&descriptor->submission, &loop->timeout, other->identifier, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  if ((loop->descriptor == NULL) &&
      (descriptor        = AllocateFastRingDescriptor(loop->ring, HandleResponse, loop)))
  {
    io_uring_prep_timeout(&descriptor->submission, &loop->timeout, 0, 0);
    SubmitFastRingDescriptor(descriptor, 0);
    loop->descriptor = descriptor;
  }
}

static int HandleResponse(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FastGLoopPoolData* data;
  struct FastGLoop* loop;
  int condition;
  int handle;

  if ((completion      != NULL) &&
      (completion->res != -ECANCELED))
  {
    loop      = (struct FastGLoop*)descriptor->closure;
    condition = FALSE;

    switch (descriptor->submission.opcode)
    {
      case IORING_OP_POLL_ADD:
        condition        = TRUE;
        handle           = descriptor->submission.fd;
        data             = loop->files + handle;
        data->result     = completion->res;
        data->descriptor = NULL;
        break;

      case IORING_OP_TIMEOUT_REMOVE:
        if (completion->res == 0)
        {
          // Timeout successfully updated
          break;
        }

      case IORING_OP_TIMEOUT:
        condition        = TRUE;
        loop->descriptor = NULL;
        break;
    }

    if (condition && !loop->condition)
    {
      loop->condition = TRUE;
      SetFastRingFlushHandler(loop->ring, HandleFlush, loop);
    }
  }
  
  return 0;  
}

static void HandleFlush(void* closure, int reason)
{
  struct FastGLoop* loop;
  struct FastGLoopPoolData* data;

  GPollFD* entry;
  GPollFD* limit;

  if (reason == RING_REASON_COMPLETE)
  {
    loop            = (struct FastGLoop*)closure;
    loop->condition = FALSE;

    entry = loop->entries;
    limit = loop->entries + loop->count;

    for ( ; entry < limit; entry ++)
    {
      data            = loop->files + entry->fd;
      entry->revents  = (data->result > 0) * data->result;
      entry->revents &= entry->events;
      loop->result   += entry->revents > 0;
    }

    JumpToLoop(loop);
    HandleRequest(loop);
  }
}

// GMainLoop

static __thread struct FastGLoop* state = NULL;

static gboolean HandleTimeout(gpointer data)
{
  // Dummy event source, required to refresh an emply poll()
  return TRUE;
}

static gint HandlePoll(GPollFD* entries, guint count, gint timeout)
{
  gint number;

  state->entries = entries;
  state->count   = count;
  state->result  = 0;

  // Due to g_timeout_source_new() the timeout should never be less 0
  state->timeout.tv_sec  =  timeout / 1000;
  state->timeout.tv_nsec = (timeout % 1000) * 1000000;

  swapcontext(state->fibers + FIBER_LOOP, state->fibers + FIBER_MAIN);

  errno = state->error;
  return state->result;
}

static void RunLoop()
{
  g_main_loop_run(state->loop);

  state->entries = NULL;
  state->count   = 0;

  setcontext(state->fibers + FIBER_MAIN);
}

static void JumpToLoop(struct FastGLoop* loop)
{
  state = loop;
  swapcontext(state->fibers + FIBER_MAIN, state->fibers + FIBER_LOOP);
  state = NULL;
}

// FastGLoop

struct FastGLoop* CreateFastGLoop(struct FastRing* ring, int interval)
{
  struct FastGLoop* loop;
  struct rlimit limit;
  GSource* source;

  loop = (struct FastGLoop*)calloc(1, sizeof(struct FastGLoop));

  getrlimit(RLIMIT_STACK, &limit);
  getcontext(loop->fibers + FIBER_MAIN);
  getcontext(loop->fibers + FIBER_LOOP);
  loop->fibers[FIBER_LOOP].uc_stack.ss_sp   = malloc(limit.rlim_cur);
  loop->fibers[FIBER_LOOP].uc_stack.ss_size = limit.rlim_cur;
  makecontext(loop->fibers + FIBER_LOOP, RunLoop, 0);

  loop->ring    = ring;
  loop->context = g_main_context_new();
  loop->loop    = g_main_loop_new(loop->context, TRUE);
  g_main_context_set_poll_func(loop->context, HandlePoll);

  source = g_timeout_source_new(interval);
  g_source_set_callback(source, HandleTimeout, NULL, NULL);
  g_source_attach(source, loop->context);

  JumpToLoop(loop);
  HandleRequest(loop);

  return loop;
}

void ReleaseFastGLoop(struct FastGLoop* loop)
{
  if (loop != NULL)
  {
    if (loop->entries != NULL)
    {
      g_main_loop_quit(loop->loop);
      JumpToLoop(loop);
    }

    g_main_loop_unref(loop->loop);
    g_main_context_unref(loop->context);
    free(loop->fibers[FIBER_LOOP].uc_stack.ss_sp);
    free(loop->files);
    free(loop);
  }
}

void TouchFastGLoop(struct FastGLoop* loop)
{
  struct FastRingDescriptor* descriptor;

  if ((loop  != NULL) &&
      (state == NULL) &&
      (!loop->condition))
  {
    loop->condition = TRUE;
    SetFastRingFlushHandler(loop->ring, HandleFlush, loop);
  }
}

void StopFastGLoop(struct FastGLoop* loop)
{
  if ((loop != NULL) &&
      (loop->entries != NULL))
  {
    g_main_loop_quit(loop->loop);
    JumpToLoop(loop);
  }
}

int IsInFastGLoop()
{
  return state != NULL;
}

// GLogFunc

typedef void (*HandleReportFunction)(int priority, const char* format, ...);

void HandleGLogReport(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data)
{
  HandleReportFunction function;

  function = (HandleReportFunction)data;

  switch (level & G_LOG_LEVEL_MASK)
  {
    case G_LOG_LEVEL_ERROR:     function(LOG_ERR,     "GLib: %s %s", domain, message);  break;
    case G_LOG_LEVEL_CRITICAL:  function(LOG_CRIT,    "GLib: %s %s", domain, message);  break;
    case G_LOG_LEVEL_WARNING:   function(LOG_WARNING, "GLib: %s %s", domain, message);  break;
    case G_LOG_LEVEL_MESSAGE:   function(LOG_NOTICE,  "GLib: %s %s", domain, message);  break;
    case G_LOG_LEVEL_INFO:      function(LOG_INFO,    "GLib: %s %s", domain, message);  break;
    case G_LOG_LEVEL_DEBUG:     function(LOG_DEBUG,   "GLib: %s %s", domain, message);  break;
  }
}
