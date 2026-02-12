# FastAvahiPoll API Reference

Header: `Ring/FastAvahiPoll.h`

`FastAvahiPoll` adapts Avahi watch/poll interface to FastRing.

## API

```c
AvahiPoll* CreateFastAvahiPoll(struct FastRing* ring);
```

Release:

```c
#define ReleaseFastAvahiPoll free
```

