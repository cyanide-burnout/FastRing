# H2OCore API Reference

Header: `Supplimentary/H2OCore.h`

`H2OCore` integrates H2O HTTP/2 and HTTP/3 server runtime with `libuv`.

## API

```c
struct H2OCore* CreateH2OCore(
  uv_loop_t* loop,
  const struct sockaddr* address,
  SSL_CTX* context1,
  ptls_context_t* context2,
  struct H2ORoute* route,
  int options);

void StopH2OCore(struct H2OCore* core);
void ReleaseH2OCore(struct H2OCore* core);
void UpdateH2OCoreSecurity(struct H2OCore* core, SSL_CTX* context1, ptls_context_t* context2, int options);
size_t GetH2OCoreConnectionCount(struct H2OCore* core);
```

Header helpers:
- `GetH2OHeaderByIndex`
- `GetH2OHeaderByName`
- `CompareH2OHeaderByIndex`
- `CompareH2OHeaderByName`
- `HasInH2OHeaderByIndex`
- `HasInH2OHeaderByName`
- `MakeH2OPercentEncodedString`

