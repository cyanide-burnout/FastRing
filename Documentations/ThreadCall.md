# ThreadCall API Reference

Header: `Ring/ThreadCall.h`

`ThreadCall` schedules execution of a handler in the FastRing thread from other threads.

## API

```c
struct ThreadCall* CreateThreadCall(struct FastRing* ring, HandleThreadCallFunction function, void* closure);
struct ThreadCall* HoldThreadCall(struct ThreadCall* call);
void ReleaseThreadCall(struct ThreadCall* call, int role);
void FreeThreadCall(void* closure);
int MakeVariadicThreadCall(struct ThreadCall* call, va_list arguments);
int MakeThreadCall(struct ThreadCall* call, ...);
int GetThreadCallWeight(struct ThreadCall* call);
```

Roles:
- `TC_ROLE_CALLER`
- `TC_ROLE_HANDLER`

Results:
- `TC_RESULT_PREPARED`
- `TC_RESULT_CALLED`
- `TC_RESULT_CANCELED`

## Notes

- `HoldThreadCall()` increments caller ownership.
- `ReleaseThreadCall(..., TC_ROLE_HANDLER)` stops handler side and drains pending calls.

