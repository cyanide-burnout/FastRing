# DBusCore API Reference

Header: `Ring/DBusCore.h`

`DBusCore` bridges a D-Bus connection with FastRing event processing.

## API

```c
struct DBusCore* CreateDBusCore(DBusConnection* connection, struct FastRing* ring);
void ReleaseDBusCore(struct DBusCore* core);
```

