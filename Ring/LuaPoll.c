#include <math.h>
#include <string.h>

#include "LuaPoll.h"
#include "LuaTrace.h"

// Internal Structures

#define STATUS_AWAKE    -EINTR
#define STATUS_TIMEOUT  -ETIME

struct Context
{
  lua_State* state;
  struct FastRing* ring;
  struct FastRingDescriptor* poll;
  struct FastRingDescriptor* event;
  struct FastRingDescriptor* timeout;
};

// FastRing Wrappers

static int HandlePollEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  // Syntax: function(handle, flags, arguments)

  int handler;
  struct Context* context;

  context = (struct Context*)descriptor->closure;

  if ((completion == NULL)  ||
      (completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    // Descriptor is in cancellation state
    return 0;
  }

  lua_getfield(context->state, LUA_REGISTRYINDEX, "LuaPoll");
  lua_getfield(context->state, -1, "HandlerIndex");

  lua_pushlightuserdata(context->state, context);
  lua_gettable(context->state, -3);

  if (lua_type(context->state, -1) != LUA_TTABLE)
  {
    lua_pop(context->state, 3);
    context->poll = NULL;
    return 0;
  }

  lua_getfield(context->state, -1, "function");
  lua_pushinteger(context->state, descriptor->submission.fd);
  lua_pushinteger(context->state, completion->res);
  lua_getfield(context->state, -4, "arguments");

  handler = lua_tointeger(context->state, -5);

  lua_pcall(context->state, 3, 0, handler);
  lua_pop(context->state, 3);

  if (context->poll != NULL)
  {
    SubmitFastRingDescriptor(context->poll, 0);
    return 1;
  }

  return 0;
}

static int HandleRoutineEvent(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  // Syntax: thread(arguments, status)
  // Note:   arguments, status = coroutine.yield()
  //         arguments, status = coroutine.yield(interval)
  //         arguments, status = coroutine.yield(handle, flags, interval)

  int status;
  int handle;
  uint32_t flags;
  double interval;
  lua_State* thread;
  struct Context* context;

  context = (struct Context*)descriptor->closure;

  if ((completion == NULL)  ||
      (completion->res == -ECANCELED) ||
      (completion->user_data & RING_DESC_OPTION_IGNORE))
  {
    // Descriptor is in cancellation state
    return 0;
  }

  if (descriptor == context->timeout)  {  context->timeout = NULL;  status = STATUS_TIMEOUT;   }
  if (descriptor == context->event)    {  context->event   = NULL;  status = STATUS_AWAKE;     }
  if (descriptor == context->poll)     {  context->poll    = NULL;  status = completion->res;  }

  if (descriptor = context->event)
  {
    context->event       = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  if (descriptor = context->poll)
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    context->poll        = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  if (descriptor = context->timeout)
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_timeout_remove(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    context->timeout     = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  lua_getfield(context->state, LUA_REGISTRYINDEX, "LuaPoll");
  lua_pushlightuserdata(context->state, context);
  lua_gettable(context->state, -2);

  if (lua_type(context->state, -1) != LUA_TTABLE)
  {
    lua_pop(context->state, 2);
    return 0;
  }

  lua_getfield(context->state, -1, "thread");
  lua_getfield(context->state, -2, "arguments");

  thread = lua_tothread(context->state, -2);
  lua_xmove(context->state, thread, 1);
  lua_pushinteger(thread, status);
  status = lua_resume(thread, 2);

  lua_pop(context->state, 2);

  if (status == LUA_ERRRUN)
  {
    lua_getfield(context->state, -1, "HandlerFunction");
    lua_xmove(context->state, thread, 1);
    lua_pushvalue(thread, -2);
    lua_call(thread, 1, 0);
    lua_pop(thread, 1);
  }

  if ((status == LUA_YIELD) &&
      (lua_type(thread, 1) == LUA_TNUMBER))
  {
    interval = lua_tonumber(thread, 1);

    if ((lua_type(thread, 2) == LUA_TNUMBER) &&
        (lua_type(thread, 3) == LUA_TNUMBER) &&
        (descriptor = AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
    {
      handle        = lua_tointeger(thread, 1);
      flags         = lua_tointeger(thread, 2);
      interval      = lua_tonumber(thread, 3);
      context->poll = descriptor;
      io_uring_prep_poll_add(&descriptor->submission, handle, flags);
      SubmitFastRingDescriptor(descriptor, 0);
    }

    if ((interval   > 0.0) &&
        (descriptor = AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
    {
      context->timeout                          = descriptor;
      descriptor->data.timeout.interval.tv_nsec = modf(interval, &interval) * 1000000000.0;
      descriptor->data.timeout.interval.tv_sec  = interval;
      io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, 0);
      SubmitFastRingDescriptor(descriptor, 0);
    }

    if ((interval       < 0.0)   &&
        (context->event == NULL) &&
        (descriptor     =  AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
    {
      context->event = descriptor;
      io_uring_prep_nop(&descriptor->submission);
      SubmitFastRingDescriptor(descriptor, 0);
    }
  }

  if ((status           == 0)    &&
      (context->poll    == NULL) &&
      (context->event   == NULL) &&
      (context->timeout == NULL))
  {
    lua_pushlightuserdata(context->state, context);
    lua_pushnil(context->state);
    lua_settable(context->state, -3);
  }

  lua_pop(context->state, 1);
  return 0;
}

// Lua API

static int CreateLuaWorker(lua_State* state)
{
  // Syntax: poll.createWorker(interval, thread, arguments)

  uint64_t value;
  double interval;
  struct Context* context;
  struct FastRingDescriptor* descriptor;

  if ((lua_type(state, 1) != LUA_TNUMBER) ||
      (lua_type(state, 2) != LUA_TTHREAD))
  {
    lua_pushliteral(state, "Invalid syntax of call createWorker()");
    lua_error(state);
    return 1;
  }

  interval = lua_tonumber(state, 1);
  context  = (struct Context*)lua_newuserdata(state, sizeof(struct Context));

  memset(context, 0, sizeof(struct Context));

  lua_getfield(state, LUA_REGISTRYINDEX, "LuaPoll");

  lua_getfield(state, -1, "RoutineContextType");
  lua_setmetatable(state, -3);

  lua_pushlightuserdata(state, context);
  lua_newtable(state);
  lua_pushliteral(state, "thread");     lua_pushvalue(state, 2);  lua_settable(state, -3);
  lua_pushliteral(state, "arguments");  lua_pushvalue(state, 3);  lua_settable(state, -3);
  lua_settable(state, -3);

  lua_getfield(state, -1, "FastRing");

  context->ring       = (struct FastRing*)lua_touserdata(state, -1);
  context->state      = state;

  if ((interval > 0.0) &&
      (descriptor = AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
  {
    context->timeout                          = descriptor;
    descriptor->data.timeout.interval.tv_nsec = modf(interval, &interval) * 1000000000.0;
    descriptor->data.timeout.interval.tv_sec  = interval;
    io_uring_prep_timeout(&descriptor->submission, &descriptor->data.timeout.interval, 0, 0);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  if ((interval < 0.0) &&
      (descriptor = AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
  {
    context->event = descriptor;
    io_uring_prep_nop(&descriptor->submission);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  lua_pop(state, 2);
  return 1;
}

static int CreateLuaHandler(lua_State* state)
{
  // Syntax: poll.createHandler(handle, flags, function, arguments)

  int handle;
  uint32_t flags;
  struct Context* context;
  struct FastRingDescriptor* descriptor;

  if ((lua_type(state, 1) != LUA_TNUMBER) ||
      (lua_type(state, 2) != LUA_TNUMBER) ||
      (lua_type(state, 3) != LUA_TFUNCTION))
  {
    lua_pushliteral(state, "Invalid syntax of call createHandler()");
    lua_error(state);
    return 1;
  }

  context = (struct Context*)lua_newuserdata(state, sizeof(struct Context));

  memset(context, 0, sizeof(struct Context));

  lua_getfield(state, LUA_REGISTRYINDEX, "LuaPoll");

  lua_getfield(state, -1, "GenericContextType");
  lua_setmetatable(state, -3);

  lua_pushlightuserdata(state, context);
  lua_newtable(state);
  lua_pushliteral(state, "function");   lua_pushvalue(state, 3);  lua_settable(state, -3);
  lua_pushliteral(state, "arguments");  lua_pushvalue(state, 4);  lua_settable(state, -3);
  lua_settable(state, -3);

  lua_getfield(state, -1, "FastRing");

  context->ring  = (struct FastRing*)lua_touserdata(state, -1);
  context->state = state;

  if (descriptor = AllocateFastRingDescriptor(context->ring, HandlePollEvent, context))
  {
    context->poll = descriptor;
    handle        = lua_tointeger(state, 1);
    flags         = lua_tointeger(state, 2);
    io_uring_prep_poll_add(&descriptor->submission, handle, flags);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  lua_pop(state, 2);
  return 1;
}

static int ReleaseLuaWorker(lua_State* state)
{
  // Syntax: poll.releaseWorker(object)

  struct Context* context;
  struct FastRingDescriptor* descriptor;

  if (lua_type(state, 1) != LUA_TUSERDATA)
  {
    lua_pushliteral(state, "Invalid syntax of call releaseWorker()");
    lua_error(state);
    return 1;
  }

  context = (struct Context*)lua_touserdata(state, 1);

  if (descriptor = context->event)
  {
    context->event       = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  if (descriptor = context->poll)
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    context->poll        = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  if (descriptor = context->timeout)
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_timeout_remove(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    context->timeout     = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  lua_getfield(state, LUA_REGISTRYINDEX, "LuaPoll");
  lua_pushlightuserdata(state, context);
  lua_pushnil(state);
  lua_settable(state, -3);

  return 0;
}

static int ReleaseLuaHandler(lua_State* state)
{
  // Syntax: poll.releaseHandler(object)

  struct Context* context;
  struct FastRingDescriptor* descriptor;

  if (lua_type(state, 1) != LUA_TUSERDATA)
  {
    lua_pushliteral(state, "Invalid syntax of call releaseHandler()");
    lua_error(state);
    return 1;
  }

  context = (struct Context*)lua_touserdata(state, 1);

  if (descriptor = context->poll)
  {
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_cancel64(&descriptor->submission, descriptor->identifier, 0);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
    context->poll        = NULL;
    descriptor->function = NULL;
    descriptor->closure  = NULL;
  }

  lua_getfield(state, LUA_REGISTRYINDEX, "LuaPoll");
  lua_pushlightuserdata(state, context);
  lua_pushnil(state);
  lua_settable(state, -3);

  return 0;
}

static int WakeLuaWorker(lua_State* state)
{
  // Syntax: poll.wake(worker)

  struct Context* context;
  struct FastRingDescriptor* descriptor;

  if (lua_type(state, 1) != LUA_TUSERDATA)
  {
    lua_pushliteral(state, "Invalid syntax of call wake()");
    lua_error(state);
    return 1;
  }

  context = (struct Context*)lua_touserdata(state, 1);

  if ((context->event == NULL) &&
      (descriptor = AllocateFastRingDescriptor(context->ring, HandleRoutineEvent, context)))
  {
    context->event = descriptor;
    io_uring_prep_nop(&descriptor->submission);
    SubmitFastRingDescriptor(descriptor, 0);
  }

  return 0;
}

// C API :)

void RegisterLuaPoll(struct FastRing* ring, lua_State* state, int handler)
{
  lua_newtable(state);
  lua_pushliteral(state, "FastRing");            lua_pushlightuserdata(state, ring);           lua_settable(state, -3);
  lua_pushliteral(state, "HandlerIndex");        lua_pushinteger(state, handler);              lua_settable(state, -3);
  lua_pushliteral(state, "HandlerFunction");     lua_pushvalue(state, handler);                lua_settable(state, -3);

  lua_pushliteral(state, "GenericContextType");  lua_newtable(state);
  lua_pushliteral(state, "__gc");                lua_pushcfunction(state, ReleaseLuaHandler);  lua_settable(state, -3);
  lua_settable(state, -3);

  lua_pushliteral(state, "RoutineContextType");  lua_newtable(state);
  lua_pushliteral(state, "__gc");                lua_pushcfunction(state, ReleaseLuaWorker);   lua_settable(state, -3);
  lua_settable(state, -3);

  lua_setfield(state, LUA_REGISTRYINDEX, "LuaPoll");

  lua_newtable(state);
  lua_pushliteral(state, "HOLD");            lua_pushinteger(state, 0);                    lua_settable(state, -3);
  lua_pushliteral(state, "IMMEDIATELY");     lua_pushinteger(state, -1);                   lua_settable(state, -3);
  lua_pushliteral(state, "EVENT_READ");      lua_pushinteger(state, POLLIN);               lua_settable(state, -3);
  lua_pushliteral(state, "EVENT_WRITE");     lua_pushinteger(state, POLLOUT);              lua_settable(state, -3);
  lua_pushliteral(state, "EVENT_ERROR");     lua_pushinteger(state, POLLERR);              lua_settable(state, -3);
  lua_pushliteral(state, "EVENT_HANGUP");    lua_pushinteger(state, POLLHUP);              lua_settable(state, -3);
  lua_pushliteral(state, "STATUS_AWAKE");    lua_pushinteger(state, STATUS_AWAKE);         lua_settable(state, -3);
  lua_pushliteral(state, "STATUS_TIMEOUT");  lua_pushinteger(state, STATUS_TIMEOUT);       lua_settable(state, -3);
  lua_pushliteral(state, "wake");            lua_pushcfunction(state, WakeLuaWorker);      lua_settable(state, -3);
  lua_pushliteral(state, "createWorker");    lua_pushcfunction(state, CreateLuaWorker);    lua_settable(state, -3);
  lua_pushliteral(state, "createHandler");   lua_pushcfunction(state, CreateLuaHandler);   lua_settable(state, -3);
  lua_pushliteral(state, "releaseWorker");   lua_pushcfunction(state, ReleaseLuaWorker);   lua_settable(state, -3);
  lua_pushliteral(state, "releaseHandler");  lua_pushcfunction(state, ReleaseLuaHandler);  lua_settable(state, -3);
  lua_setglobal(state, "poll");
}
