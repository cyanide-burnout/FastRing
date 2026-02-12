# CoRing API Reference

Header: `Ring/CoRing.h`

`CoRing` is a C++ adapter integrating FastRing descriptors with the `Compromise` coroutine/event abstraction.

## Main Classes

- `CoRing`
- `CoRingEvent`

## CoRing

```cpp
CoRing(struct FastRing* ring);
~CoRing();

struct FastRingDescriptor* allocate();
void submit();
```

## CoRingEvent

```cpp
CoRingEvent();
void keep() const;
void release() const;
```

## Notes

- `allocate()` prepares descriptor tracking and callback wiring.
- `submit()` pushes all currently allocated descriptors.
- `keep()` / `release()` control ownership of completed descriptors.

