# FastBIO API Reference

Header: `Ring/FastBIO.h`

`FastBIO` connects OpenSSL BIO operations to asynchronous transport over `FastRing`.

## API

```c
BIO* CreateFastBIO(
  struct FastRing* ring,
  struct FastRingBufferProvider* provider,
  struct FastBufferPool* inbound,
  struct FastBufferPool* outbound,
  int handle,
  uint32_t granularity,
  uint32_t limit,
  HandleFastBIOEvent function,
  void* closure);
```

Event callback:

```c
typedef void (*HandleFastBIOEvent)(struct FastBIO* engine, int event, int parameter);
```

## Notes

- Returns OpenSSL `BIO*` object configured for async operation.
- Uses inbound/outbound `FastBufferPool` and ring buffer provider.
- `FASTBIO_CTRL_TOUCH` is provided for module-specific BIO control integration.

