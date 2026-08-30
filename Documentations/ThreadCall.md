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

- called **from the ring thread**, the handler is invoked directly on the spot, with
  no queueing and no wakeup;
- called from any other thread, the arguments are published on a lock-free stack, the
  ring thread is woken, and the caller waits on a futex until the handler returns.

Return values:

| Result | Meaning |
| --- | --- |
| `TC_RESULT_CALLED` | The handler ran |
| `TC_RESULT_CANCELED` | The handler side is gone (`ReleaseThreadCall(..., TC_ROLE_HANDLER)` was called, or is in progress); the handler did not run |
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
— but it does mean a handler must never re-enter `MakeThreadCall()` on the same
thread.

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
reference.

`FreeThreadCall()` is a `void (*)(void*)` adapter that releases `TC_ROLE_CALLER`, for
use as a destructor callback in libraries that take one.

### Teardown

`ReleaseThreadCall(call, TC_ROLE_HANDLER)` does the work that makes shutdown safe:

1. the call stack is corked, so no new call can be queued;
2. every already-queued call is completed with `TC_RESULT_CANCELED` and its waiting
   thread is woken — nobody is left blocked;
3. the ring descriptor is cancelled and the eventfd unregistered.

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

- **liburing >= 2.6 with `IORING_OP_FUTEX_WAIT` available** (`TC_FEATURE_RING_FUTEX`) —
  the ring waits directly on a futex embedded in the call stack pointer. No file
  descriptor is used.
- **otherwise** — an `eventfd` is created, registered as a fixed file when possible,
  and read through the ring.

Completion wakeups prefer `IORING_OP_FUTEX_WAKE` submitted through the ring
(`TC_WAKE_LAZY`) and fall back to a synchronous `futex()` syscall when that SQE cannot
be completed — this is what the non-`RING_REASON_COMPLETE` branch of the wakeup
handler is for, see `Documentations/Lifecycle.md`.

## Rules

- The handler runs in the ring thread and must not block: a blocked handler stalls
  both the ring and every waiting caller.
- Never call `MakeThreadCall()` from the ring thread into a handler that would wait on
  that same thread — the direct-call shortcut avoids the deadlock only for the same
  `ThreadCall` object.
- Arguments are passed by `va_list` and are only valid for the duration of the call.
  Pass pointers to caller-owned storage, never to stack objects that the caller may
  unwind past.
