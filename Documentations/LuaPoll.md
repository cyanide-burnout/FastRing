# LuaPoll API Reference

Header: `Ring/LuaPoll.h`

`LuaPoll` registers FastRing polling bindings in a Lua/LuaJIT state. Lua coroutines
become cooperative workers driven by the ring: a coroutine yields an interval or a
descriptor event, and the ring resumes it when that condition is met.

## C API

```c
void RegisterLuaPoll(struct FastRing* ring, lua_State* state, int handler);
```

- `ring` is the ring that will drive the workers.
- `state` is the Lua state to populate.
- `handler` is a stack index of a Lua function used as the default event handler for
  descriptor handlers created from C-side registrations. It is stored in the registry
  together with the ring under the key `LuaPoll`.

The call creates the global table `poll` described below.

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
  `poll.HOLD`. `arguments` is passed to the coroutine on its first resume.
- `createHandler(handle, flags, function, arguments)` registers a callback for a file
  descriptor. `flags` is a sum of `poll.EVENT_*`.
- `wake(worker)` resumes a worker that is waiting on an interval or on `poll.HOLD`.
- `releaseWorker()` / `releaseHandler()` unregister explicitly. Both are also wired as
  the `__gc` metamethod, so a collected worker or handler is unregistered
  automatically.

## Yielding

```lua
local arguments, status = coroutine.yield()                         -- wait until wake() is called
local arguments, status = coroutine.yield(interval)                 -- wait for the interval
local arguments, status = coroutine.yield(handle, flags, interval)  -- wait for a descriptor event or the interval
```

`status` is:

- a set of `poll.EVENT_*` flags when a descriptor event resumed the coroutine,
- `poll.STATUS_AWAKE` when `poll.wake()` resumed it or the interval was
  `poll.IMMEDIATELY`,
- `poll.STATUS_TIMEOUT` when the interval expired.

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
