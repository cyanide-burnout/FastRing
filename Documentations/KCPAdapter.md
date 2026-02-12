# KCPAdapter API Reference

Header: `Supplimentary/KCPAdapter.h`

`KCPAdapter` connects `KCPService` with FastRing/FastSocket transport.

## API

```c
typedef const struct KCPFormat* (*ValidateKCPPacket)(
  struct KCPAdapter* adapter,
  struct sockaddr* address,
  uint8_t* data,
  int length);

struct KCPAdapter* CreateKCPAdapter(
  struct FastRing* ring,
  struct KCPService* service,
  const struct KCPFormat* format,
  ValidateKCPPacket validate,
  void* closure,
  int port);

void ReleaseKCPAdapter(struct KCPAdapter* adapter);
```

