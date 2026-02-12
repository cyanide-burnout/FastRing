# FastGLoop API Reference

Header: `Ring/FastGLoop.h`

`FastGLoop` integrates GLib main loop processing with FastRing.

## API

```c
struct FastGLoop* CreateFastGLoop(struct FastRing* ring, int interval);
void ReleaseFastGLoop(struct FastGLoop* loop);
void TouchFastGLoop(struct FastGLoop* loop);
void StopFastGLoop(struct FastGLoop* loop);
int IsInFastGLoop();

void HandleGLogReport(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data);
```

## Notes

- `TouchFastGLoop()` wakes/advances loop integration logic.
- `StopFastGLoop()` requests loop stop before release.
- `HandleGLogReport()` can be used as a GLib log forwarder callback.

