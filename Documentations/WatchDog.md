# WatchDog API Reference

Header: `Ring/WatchDog.h`

`WatchDog` integrates periodic systemd watchdog notifications with FastRing timeouts.

## API

```c
struct WatchDog* CreateWatchDog(struct FastRing* ring);
void ReleaseWatchDog(struct WatchDog* state);
```

## Notes

- `CreateWatchDog()` returns `NULL` when systemd watchdog is disabled or unavailable.

