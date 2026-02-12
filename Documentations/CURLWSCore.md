# CURLWSCore API Reference

Header: `Ring/CURLWSCore.h`

`CURLWSCore` provides WebSocket transport on top of `Fetch`/`libcurl`.

## States and Reasons

Reasons:
- `CWS_REASON_CLOSED`
- `CWS_REASON_CONNECTED`
- `CWS_REASON_RECEIVED`

States:
- `CWS_STATE_CONNECTING`
- `CWS_STATE_CONNECTED`
- `CWS_STATE_REJECTED`

## API

```c
typedef int (*HandleCWSEventFunction)(
  void* closure,
  struct CWSTransmission* transmission,
  int reason,
  int parameter,
  char* data,
  size_t length);

struct CWSTransmission* MakeExtendedCWSTransmission(
  struct Fetch* fetch,
  CURL* easy,
  HandleCWSEventFunction function,
  void* closure);

struct CWSTransmission* MakeSimpleCWSTransmission(
  struct Fetch* fetch,
  const char* location,
  struct curl_slist* headers,
  const char* token,
  HandleCWSEventFunction function,
  void* closure);

void CloseCWSTransmission(struct CWSTransmission* transmission);

struct CWSMessage* AllocateCWSMessage(struct CWSTransmission* transmission, size_t length, int type);
void TransmitCWSMessage(struct CWSMessage* message);
```

