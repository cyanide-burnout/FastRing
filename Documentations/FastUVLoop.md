# FastUVLoop API Reference

Header: `Ring/FastUVLoop.h`

`FastUVLoop` integrates `libuv` with FastRing.

## API

```c
typedef int (*CheckUVLoopDepletion)(void* closure);

struct FastUVLoop* CreateFastUVLoop(struct FastRing* ring, int interval);
void ReleaseFastUVLoop(struct FastUVLoop* loop);
void TouchFastUVLoop(struct FastUVLoop* loop);
void DepleteFastUVLoop(struct FastUVLoop* loop, int timeout, uint64_t kick, CheckUVLoopDepletion function, void* closure);
```

Kick flags:
- `UVLOOP_KICK_UNREF(type)`
- `UVLOOP_KICK_POKE_TIMER`

