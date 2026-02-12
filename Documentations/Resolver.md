# Resolver API Reference

Header: `Ring/Resolver.h`

`Resolver` provides c-ares DNS resolver integration with FastRing.

## API

```c
struct ResolverState* CreateResolver(struct FastRing* ring);
void UpdateResolverTimer(struct ResolverState* state);
void ReleaseResolver(struct ResolverState* state);
```

## Notes

- Call `UpdateResolverTimer()` after resolver activity to refresh next timeout/watch state.

