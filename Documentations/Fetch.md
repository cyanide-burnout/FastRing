# Fetch API Reference

Header: `Ring/Fetch.h`

`Fetch` is an asynchronous wrapper around `libcurl` multi interface integrated with FastRing.

## API

```c
struct Fetch* CreateFetch(struct FastRing* ring);
void ReleaseFetch(struct Fetch* fetch);
int GetFetchTransmissionCount(struct Fetch* fetch);

struct FetchTransmission* MakeExtendedFetchTransmission(
  struct Fetch* fetch,
  struct FetchTransmission* transmission,
  CURL* easy,
  int option,
  HandleFetchFunction function,
  void* parameter1,
  void* parameter2);

struct FetchTransmission* MakeSimpleFetchTransmission(
  struct Fetch* fetch,
  const char* location,
  struct curl_slist* headers,
  const char* token,
  const char* data,
  size_t length,
  HandleFetchFunction function,
  void* parameter1,
  void* parameter2);

void CancelFetchTransmission(struct FetchTransmission* transmission);
void TouchFetchTransmission(struct FetchTransmission* transmission);

int AppendFetchParameter(CURLU* location, int size, const char* format, ...);
struct curl_slist* AppendFetchList(struct curl_slist* list, int size, const char* format, ...);
struct curl_slist* MakeFetchConnectAddress(const struct sockaddr* address);
```

## Status Codes

- `FETCH_STATUS_INCOMPLETE`
- `FETCH_STATUS_CANCELLED`

