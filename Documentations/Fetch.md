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

## Transmission Options

The `option` argument of `MakeExtendedFetchTransmission()`:

- `FETCH_OPTION_HANDLE_CONTENT` - `Fetch` installs its own `CURLOPT_WRITEFUNCTION`
  and accumulates the response body, delivering it to the callback as `data`/`length`.
- `FETCH_OPTION_SET_HANDLER_DATA` - `CURLOPT_READDATA`, `CURLOPT_WRITEDATA` and
  `CURLOPT_HEADERDATA` are pointed at the `FetchTransmission`, leaving the callbacks
  themselves to the caller.

`MakeSimpleFetchTransmission()` always uses `FETCH_OPTION_HANDLE_CONTENT`.

`FETCH_STORAGE_SIZE` (128) is the initial capacity of the accumulation buffer.

## Completion Codes

`HandleFetchFunction` is called exactly once per transmission. The meaning of `code`
and of `data`/`length` depends on how the transmission ended:

| Outcome | `code` | `data` / `length` |
| --- | --- | --- |
| libcurl finished with `CURLE_OK` | HTTP response code (`CURLINFO_RESPONSE_CODE`), a non-negative value | Accumulated body, but only with `FETCH_OPTION_HANDLE_CONTENT`; otherwise `NULL` / `0` |
| libcurl failed | `-CURLcode`, a negative value | `curl_easy_strerror()` text, `length` is `0` |
| `CancelFetchTransmission()` | `FETCH_STATUS_CANCELLED` (-1001) | `NULL` / `0` |
| `ReleaseFetch()` with the transmission still in flight | `FETCH_STATUS_INCOMPLETE` (-1000) | `NULL` / `0` |

So a non-negative `code` is an HTTP status, not a `CURLcode` — a transport failure is
always negative. `FETCH_STATUS_INCOMPLETE` is also the internal state of a running
transmission and is available under the legacy name `TRANSMISSION_INCOMPLETE`.

The transmission object is freed right after the callback returns; do not keep the
pointer.

