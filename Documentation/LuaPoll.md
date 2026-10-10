# LuaPoll API Reference

Header: `Ring/LuaPoll.h`

`LuaPoll` registers FastRing polling bindings in a Lua 5.1 or LuaJIT state (it uses the
Lua 5.1 `lua_resume()` signature). Lua coroutines
become cooperative workers driven by the ring: a coroutine yields an interval or a
descriptor event, and the ring resumes it when that condition is met.

## C API

```c
void RegisterLuaPoll(struct FastRing* ring, lua_State* state, int handler);
```

- `ring` is the ring that will drive the workers.
- `state` is the Lua state to populate.
- `handler` is the absolute stack index of a Lua function used as the **message handler**
  for errors. The index is passed as `errfunc` to `lua_pcall()` when a `createHandler`
  callback runs, so it has to stay valid on that stack for as long as handlers exist
  (keep the function at the bottom of the main state's stack); a copy of the function is
  called with the error message when a worker coroutine raises an error. Both are stored
  in the registry together with the ring under the key `LuaPoll`.

The call creates the global table `poll` described below; it creates nothing on the ring.

`Ring/LuaPoll.c` includes `LuaTrace.h`, which is not part of this repository; provide it,
or remove the include, which nothing in the file depends on.

## Lua Table `poll`

### Constants

| Name | Value | Meaning |
| --- | --- | --- |
| `poll.HOLD` | `0` | Create the worker suspended; it runs only after `poll.wake()` |
| `poll.IMMEDIATELY` | `-1` | Run the worker on the next loop iteration |
| `poll.EVENT_READ` | `POLLIN` | Descriptor is readable |
| `poll.EVENT_WRITE` | `POLLOUT` | Descriptor is writable |
| `poll.EVENT_ERROR` | `POLLERR` | Descriptor error |
| `poll.EVENT_HANGUP` | `POLLHUP` | Peer hung up |
| `poll.STATUS_AWAKE` | `-EINTR` | Resumed by `poll.wake()` or by `poll.IMMEDIATELY` |
| `poll.STATUS_TIMEOUT` | `-ETIME` | Resumed because the interval expired |

### Functions

```lua
worker  = poll.createWorker(timeout, thread, arguments)
handler = poll.createHandler(handle, flags, function, arguments)
poll.wake(worker)
poll.releaseWorker(worker)
poll.releaseHandler(handler)
```

- `createWorker(timeout, thread, arguments)` registers a coroutine created with
  `coroutine.create()`. `timeout` is an interval in seconds, `poll.IMMEDIATELY`, or
  `poll.HOLD`. `arguments` is passed on every resume, together with `status`: as the
  parameters of the function the first time, as the results of `coroutine.yield()`
  afterwards. A worker whose coroutine returns is unregistered; one that raises an error
  stays registered until it is released or collected.
- `createHandler(handle, flags, function, arguments)` registers a callback
  `function(handle, result, arguments)` for a file descriptor, where `result` is the poll
  result: the returned events, or a negative errno. `flags` is a sum of `poll.EVENT_*`.
  The one-shot poll is armed again after every callback, an error result included, until
  the handler is released.
- `wake(worker)` resumes a worker waiting on anything — an interval, a descriptor or
  `poll.HOLD`. It does nothing when a wake is already pending. Only for workers: passing a
  handler crashes.
- `releaseWorker()` / `releaseHandler()` unregister explicitly. Both are also wired as
  the `__gc` metamethod, so a collected worker or handler is unregistered
  automatically. The registry entry does not keep the object alive: keep a Lua reference
  to every worker and handler for as long as it should run.

## Yielding

```lua
local arguments, status = coroutine.yield()                         -- wait until wake() is called
local arguments, status = coroutine.yield(interval)                 -- wait for the interval
local arguments, status = coroutine.yield(handle, flags, interval)  -- wait for a descriptor event or the interval
```

`status` is:

- the poll result when a descriptor event resumed the coroutine: a set of `poll.EVENT_*`
  flags, or a negative errno,
- `poll.STATUS_AWAKE` when `poll.wake()` resumed it or the interval was
  `poll.IMMEDIATELY`,
- `poll.STATUS_TIMEOUT` when the interval expired.

An `interval` of `0`, or none, arms no timeout: the worker waits for `wake()` or the
descriptor. A negative interval resumes it on the next iteration with `poll.STATUS_AWAKE`.

## Example

```lua
local function doTestThread(arguments, status)
  local inspect = require('inspect')
  while true do
    core.report('*** Multithreading example: %s', inspect(arguments))
    coroutine.yield(10)
  end
end

local routine1 = poll.createWorker(1, coroutine.create(doTestThread), { 1, 2, 3 })
local routine2 = poll.createWorker(1, coroutine.create(doTestThread), { 4, 5, 6 })
local routine3 = poll.createWorker(poll.IMMEDIATELY, coroutine.create(doTestThread))
local routine4 = poll.createWorker(poll.HOLD,        coroutine.create(doTestThread))

poll.wake(routine4)
```

The example keeps the workers in chunk-local variables; they run until the collector
releases them once those variables are gone.

## Lifecycle

A handler owns one descriptor, a one-shot `IORING_OP_POLL_ADD` that its completion re-submits.
A waiting worker owns up to three one-shot descriptors: a poll, a timeout and a wake NOP. The
first to complete resumes the coroutine, and the others are abandoned before the resume
(`DiscardFastRingDescriptor()` for the poll and the timeout). Every worker descriptor is
released by its own completion. Releasing, explicitly or by the collector, abandons the armed
descriptors and removes the registry entry; releasing from inside the object's own callback is
safe.

Callbacks run on the `lua_State` that created the object: create workers and handlers from the
main state, not from inside a coroutine. Everything runs on the ring thread. `lua_close()` runs
the `__gc` releases and must come before `ReleaseFastRing()`: if the ring is released first, it
frees the descriptors without calling Lua, and the collector's release touches freed memory.
