# FastSemaphore API Reference

Header: `Ring/FastSemaphore.h`

`FastSemaphore` provides asynchronous waiting/posting semantics for `sem_t`.

## Compatibility

- Requires glibc >= 2.34
- Requires liburing >= 2.6

## API

```c
typedef int (*FastSemaphoreFunction)(sem_t* semaphore, void* closure);

struct FastRingDescriptor* RegisterFastSemaphore(
  struct FastRing* ring,
  sem_t* semaphore,
  FastSemaphoreFunction function,
  void* closure,
  int limit);

void CancelFastSemaphore(struct FastRingDescriptor* descriptor);
int PostFastSemaphore(struct FastRing* ring, sem_t* semaphore);
```

## Notes

- Callback returns `1` to continue waiting, `0` to unregister.
- `limit` caps tokens handled per callback invocation.

