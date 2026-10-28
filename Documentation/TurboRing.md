# TurboRing API Reference

Header: `Ring/TurboRing.h`

`TurboRing` runs a [Turbo Vision](https://github.com/magiblot/tvision) application on FastRing.
The ring loop stays the main loop of the thread; Turbo Vision becomes one more party served by it.

## How the Integration Works

Turbo Vision insists on owning the wait: `TProgram::run()` loops over `getEvent()`, which blocks in
`select()` on the descriptors of its event sources until one gets readable or a timeout expires.
`TurboRing` derives from `TApplication` and takes the blocking part away from it:

- `run()` goes on a fiber with a stack of its own, `RLIMIT_STACK` bytes allocated with `malloc()`
  (with an unlimited stack limit `start()` fails), the way [`FastGLoop`](FastGLoop.md) runs
  `GMainLoop`.
- `getEvent()` lets Turbo Vision look for events with a zero timeout. When there is none, it
  switches back to the ring and leaves what Turbo Vision would have waited for behind:
  - a one-shot `IORING_OP_POLL_ADD` for reading on every descriptor of Turbo Vision's
    `EventWaiter`: the terminal, the pipe of its `SIGWINCH` handler, the pipe of
    `TEventQueue::wakeUp()`, the GPM mouse, and any source an application has added itself;
  - a ring timeout of the interval Turbo Vision would have waited for: `TProgram::eventTimeoutMs`
    and the timers of `setTimer()`, or less when a frame is held back by its frame rate limit.
- A completion of any of them, or `touch()`, arms a flush handler that switches into the fiber.
  `getEvent()` looks for events again and returns the first one, or waits once more.
- Which sources are readable is told by the polls, not by `select()` of Turbo Vision: a completed
  poll marks its source ready in the `EventWaiter` the way `select()` would, and a readable source
  with nothing to read leaves it, as there. While Turbo Vision looks for events on the fiber, the
  handles its `select()` would look at are replaced by `FD_SETSIZE`, which it skips, so no `select()`
  is made at all and sources on descriptors past `FD_SETSIZE` work as the others.

Both sides run on the ring thread and no locking is needed between them. Turbo Vision keeps its
event sources, their poll states, its display buffer and its wait interval private; `TurboRing` reads them through
explicit template instantiations, which the rules of access do not apply to. It needs the internal
headers of Turbo Vision, so `TurboRing.cpp` is compiled with `-I<tvision>/include/tvision` in
addition to `-I<tvision>/include`, and must be built against the very Turbo Vision it links with.

Modal views work as usual: `execView()` loops over `getEvent()` on the fiber, and the ring keeps
running underneath.

## API

```cpp
class TurboRing : public TApplication
{
  public:

    TurboRing(struct FastRing* ring);
    ~TurboRing();

    void getEvent(TEvent& event) override;

    bool start();
    void touch();
    void stop();
    bool isRunning() const;

    static bool isInside();
};
```

| Member | Purpose |
| --- | --- |
| `TurboRing(ring)` | Constructs the application on the ring thread; the console of Turbo Vision is set up here, as by `TApplication` |
| `start()` | Begins `run()` on the fiber and returns as soon as it waits for events. `false` when it has been started before or the fiber stack cannot be allocated |
| `touch()` | Arms a flush handler that resumes the fiber in the current iteration of the ring. For changes made outside the fiber: views drawn from a ring callback reach the screen only when the fiber runs |
| `stop()` | Puts `cmQuit` and touches the fiber. A modal view that is open decides what to do with it |
| `isRunning()` | `true` from `start()` until `run()` returns |
| `isInside()` | `true` on the fiber of the application |
| `getEvent()` | Overrides `TProgram::getEvent()`; an application overriding it in turn calls `TurboRing::getEvent()` |

A derived application initializes the virtual base `TProgInit` itself, as with `TApplication`:

```cpp
class Application : public TurboRing
{
  public:

    Application(struct FastRing* ring) :
      TProgInit(&Application::initStatusLine, &Application::initMenuBar, &TApplication::initDeskTop),
      TurboRing(ring)
    {
    }
};
```

## Usage

```cpp
ring        = CreateFastRing(0);
application = new Application(ring);

if (application->start())
{
  while (application->isRunning())
  {
    result = WaitForFastRing(ring, 1000, nullptr);
    ...
  }
}

TObject::destroy(application);
ReleaseFastRing(ring);
```

## Notes

- `TProgram::eventTimeoutMs` keeps its meaning: it is the interval of the ring timeout while
  nothing happens, `20` by default, so `idle()` runs as often as it does without the ring. A
  negative value leaves only the descriptors, the timers of `setTimer()` and `touch()` to resume
  the fiber. Inside `TurboRing::getEvent()` it reads `0`; a value `idle()` sets there is kept, a
  value of `0` set there is not.
- Ring callbacks run outside the fiber. They may change views and call `drawView()`, and then
  `touch()`; they must not open modal views. A `getEvent()` outside the fiber falls back to the
  wait of Turbo Vision itself and stalls the ring until it returns; there `select()` sees the real
  handles again, and those past `FD_SETSIZE` are not seen.
- Other threads reach the application with `TEventQueue::wakeUp()` as before, since its pipe is
  one of the polled descriptors, or with [`ThreadCall`](ThreadCall.md) and `touch()`.
- Turbo Vision code on the fiber may submit descriptors and arm timeouts, but must not call
  `WaitForFastRing()`.
- The polls are made anew on every wait, so a source that is replaced by another on the same
  descriptor number between two waits is polled as the new file.

## Lifecycle

The constructor sets up Turbo Vision and nothing on the ring. `start()` allocates the fiber stack,
switches into the fiber and, once `run()` waits for the first time, submits the polls and the
timeout of that wait. Every later wait discards the polls left from the previous one with
`DiscardFastRingDescriptor()` and makes new ones; the timeout descriptor is re-armed with the new
interval or removed when there is none.

When `run()` returns, the fiber removes a pending flush handler and switches back for the last
time; the polls are discarded and the timeout removed, and `isRunning()` turns `false`. Destroy
the application with `TObject::destroy()` after that and before `ReleaseFastRing()`. Destroying it
while `run()` has not returned drops the fiber with whatever is on its stack.
