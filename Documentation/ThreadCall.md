# ThreadCall API Reference

Header: `Ring/ThreadCall.h`

`ThreadCall` executes a handler in the FastRing thread on behalf of another thread.
The call is **synchronous**: the caller blocks until the handler has run, which makes
it the standard way to let foreign threads (libraries with their own thread pools,
blocking APIs) touch ring-owned state without locking it.

## API

```c
typedef void (*HandleThreadCallFunction)(void* closure, va_list arguments);

struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure);
struct ThreadCall* HoldThreadCall(struct ThreadCall* call);
void ReleaseThreadCall(struct ThreadCall* call, int role);
void FreeThreadCall(void* closure);

int MakeThreadCall(struct ThreadCall* call, ...);
int MakeVariadicThreadCall(struct ThreadCall* call, va_list arguments);
int GetThreadCallWeight(struct ThreadCall* call);
```

## Making a Call

`MakeThreadCall()` passes its variadic arguments to the handler as a `va_list` and
returns once the handler is done:

- called **from the ring thread** of the call's ring, the handler is invoked directly on
  the spot, with no queueing and no wakeup;
- called from any other thread, the arguments are published on a lock-free stack, the
  ring thread is woken if the stack was empty, and the caller waits on a futex until the
  handler returns. Queued calls are served last in, first out.

Both paths run only while the handler role is alive **and at least one caller reference
exists**: `CreateThreadCall()` starts with the handler role alone, so a call made before any
`HoldThreadCall()` returns `TC_RESULT_CANCELED`, on the ring thread as well.

Return values:

| Result | Meaning |
| --- | --- |
| `TC_RESULT_CALLED` | The handler ran |
| `TC_RESULT_CANCELED` | The handler did not run: the handler side is gone or going (`ReleaseThreadCall(..., TC_ROLE_HANDLER)`), no caller reference is held, or `call` is `NULL` |
| `TC_RESULT_PREPARED` | Internal in-flight marker, not observable as a return value |

Always check for `TC_RESULT_CANCELED` — after teardown every call returns it silently
instead of failing.

### Passing arguments

Arguments are forwarded verbatim as a `va_list`, so the natural shape is a
discriminated dispatch: a leading opcode, then whatever that opcode needs.

```c
#define CODE_SESSION_CREATE   0
#define CODE_SESSION_RELEASE  1

// from a foreign thread
MakeThreadCall(worker->call, CODE_SESSION_CREATE, session, address, host, path);
...
MakeThreadCall(worker->call, CODE_SESSION_RELEASE, session);
```

```c
static void HandleThreadCall(void* closure, va_list arguments)
{
  struct Worker* worker   = (struct Worker*)closure;
  int code                = va_arg(arguments, int);
  struct Session* session = va_arg(arguments, struct Session*);

  switch (code)
  {
    case CODE_SESSION_CREATE:
      {
        struct sockaddr* address = va_arg(arguments, struct sockaddr*);
        const char* host         = va_arg(arguments, const char*);
        const char* path         = va_arg(arguments, const char*);

        CreateSession(worker, session, address, host, path);
      }
      break;

    case CODE_SESSION_RELEASE:
      ReleaseSession(worker, session);
      break;
  }
}
```

The number of arguments may differ per opcode — read the common ones first, then the
rest inside the branch. Two rules follow from `va_arg` having no type information:

- **Caller and handler must agree exactly** on the order and the types. There is no
  checking; a mismatch is undefined behaviour, not an error code.
- Default argument promotions apply. `char`, `short` and `bool` arrive as `int`, and
  `float` as `double` — read them with the promoted type.

Pointers may safely refer to caller-owned objects, including stack objects, because the
call is synchronous: the caller is blocked inside `MakeThreadCall()` for the whole time
the handler runs.

### Passing results back

Since the caller waits, the handler can answer through the same memory. Where a call
has a result, the established shape is one pointer to a caller-owned structure that
the handler fills in:

```c
struct WebKeeperData parameters;

memset(&parameters, 0, sizeof(struct WebKeeperData));
parameters.type   = KEEPER_TYPE_REQUEST_STREAM_DATA;
parameters.data   = buffer;
parameters.length = size;

MakeThreadCall(stream->call, &parameters);

switch (parameters.type)     // written by the handler
{
  ...
}
```

```c
static void HandleThreadCall(void* closure, va_list arguments)
{
  struct Object* self        = (struct Object*)closure;
  struct WebKeeperData* data = va_arg(arguments, struct WebKeeperData*);
  ...
}
```

The argument state lives in a thread-local buffer, so a thread has at most one call
outstanding at a time. That is not a restriction in practice — the call blocks anyway
— but it does mean a handler must never re-enter `MakeThreadCall()`: on the ring thread the
nested direct call overwrites the `va_list` the outer handler is still reading.

## Ownership: Two Roles

`ThreadCall` is reference counted with two distinct roles, so the handler side and the
caller side can be torn down independently:

- `TC_ROLE_HANDLER` — the object that owns the handler function, established by
  `CreateThreadCall()`. There is exactly one.
- `TC_ROLE_CALLER` — a caller's reference, taken with `HoldThreadCall()`. There can be
  many.

`ReleaseThreadCall(call, role)` drops the matching role, and the object is freed when
both sides are gone. `GetThreadCallWeight()` returns the raw counter: a value above
`TC_ROLE_HANDLER` means the handler is still alive and at least one caller holds a
reference, which is exactly the condition `MakeThreadCall()` checks. Releasing the handler
role twice is not guarded.

`FreeThreadCall()` is a `void (*)(void*)` adapter that releases `TC_ROLE_CALLER`, for
use as a destructor callback in libraries that take one.

### Teardown

`ReleaseThreadCall(call, TC_ROLE_HANDLER)` does the work that makes shutdown safe:

1. the call stack is corked, so no new call can be queued;
2. every already-queued call is completed with `TC_RESULT_CANCELED` and its waiting
   thread is woken — nobody is left blocked;
3. the ring descriptor is abandoned with `DiscardFastRingDescriptor()`, and in eventfd mode
   the registered file is removed. The `eventfd` itself is closed when the object is freed.

Callers that still hold references keep the object allocated but every further
`MakeThreadCall()` returns `TC_RESULT_CANCELED` immediately.

The typical shape, taken from a C++ owner:

```cpp
Object::Object(struct FastRing* ring)
{
  call = CreateThreadCall(ring, handleThreadCall, this);
  ...
}

Object::~Object()
{
  ReleaseThreadCall(call, TC_ROLE_HANDLER);   // first, so no call can reach a half-destroyed object
  ...
}
```

Release the handler role **before** tearing down what the handler touches.

## Wakeup Mechanism

The way the ring thread is woken is chosen at construction:

- **liburing >= 2.6 at build time and `IORING_OP_FUTEX_WAIT` supported by the kernel**
  (Linux 6.7, probed through `ring->probe`; `TC_FEATURE_RING_FUTEX`) — the ring waits
  directly on a futex, the low 32 bits of the call stack pointer, for the value 0. No file
  descriptor is used.
- **otherwise** — an `eventfd` is created, registered as a fixed file when possible (not
  when `CreateThreadCall()` runs off the ring thread: the kernel refuses the registration),
  and read through the ring.

The caller is woken with `IORING_OP_FUTEX_WAKE` submitted through the ring (`TC_WAKE_LAZY`)
in futex mode when a descriptor can be allocated, and with a synchronous `futex()` syscall
otherwise. The lazy wake leaves with the ring's next submission; one that completes with an
error is resubmitted while the caller is still waiting. The synchronous fallback also runs
when the wake descriptor is released without having completed (`RING_REASON_RELEASED`) —
this is what the non-`RING_REASON_COMPLETE` branch of the wakeup handler is for, see
[LIFECYCLE.md](LIFECYCLE.md#arguments).

## Rules

- The handler runs in the ring thread and must not block: a blocked handler stalls
  both the ring and every waiting caller.
- The direct-call shortcut applies to every `ThreadCall` of the caller's own ring. A ring
  thread calling into a `ThreadCall` of another ring blocks like any foreign thread, and
  deadlocks if that ring's thread is itself blocked in a call back.
- Arguments are passed by `va_list` and are only valid for the duration of the handler.
  Pointers to the caller's stack are fine, because the caller is blocked meanwhile; the
  handler must not keep them.
- Do not release the handler role from inside the handler. The handler re-arms its
  descriptor after the queued calls have run, and the descriptor would then be queued a
  second time by the abandonment in `ReleaseThreadCall()`, which stops the ring from
  submitting (see [LIFECYCLE.md](LIFECYCLE.md#the-submission-queue)). Defer it to a flusher.

## Lifecycle

`CreateThreadCall()` allocates the object and one descriptor that stays armed for the life
of the handler role: an `IORING_OP_FUTEX_WAIT` in futex mode, or a read of the `eventfd`.
The descriptor keeps its single reference: its handler runs every queued call, re-submits
the descriptor and returns non-zero. A lazy wake is a transient descriptor without a
reference to the object, released by its own completion.

The object lives while either role is held. The handler role is released on the ring
thread (it removes the registered file), and **before `ReleaseFastRing()`**. If the ring is
released first, its teardown calls the descriptor handler with `RING_REASON_RELEASED`, which
runs every call still queued and wakes the callers with `futex()`; the object stays
allocated, and a later `ReleaseThreadCall(call, TC_ROLE_HANDLER)` touches freed ring memory.

`HoldThreadCall()`, `MakeThreadCall()` and the release of a caller role may be used from any
thread.
