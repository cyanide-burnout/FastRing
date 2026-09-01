# Fetch API Reference

Header: `Ring/Fetch.h`

`Fetch` drives the libcurl **multi** interface from a FastRing loop. One `struct Fetch`
owns a `CURLM` multi handle, a `CURLSH` share handle and the ring descriptors libcurl
asks for; each HTTP request is a `struct FetchTransmission` wrapping one `CURL` easy
handle. There is no thread and no blocking call anywhere — sockets are polled with
`IORING_OP_POLL_ADD` and libcurl's timer becomes a FastRing timeout.

That makes it useful for plain requests, but plain requests are not the point of the
module. `Fetch` is a **multiplexer for protocols built on libcurl**: a place where an
arbitrary number of sessions of different kinds share one multi handle, one set of ring
descriptors and one connection pool. Two such protocols ship with the project —
[CURLWSCore.md](CURLWSCore.md) for WebSocket and [gRPCClient.md](gRPCClient.md) for gRPC
— and the list is extended by external modules **without changing this API**.

A complete, runnable program covering every completion path is in
[Examples/Fetch](../Examples/Fetch/FetchTest.c).

## Layering

```text
FastRing  ->  Fetch (libcurl multi)  ->  application
                                    ->  CURLWSCore  ->  application
                                    ->  gRPCClient  ->  application
```

`CURLMOPT_SOCKETFUNCTION` allocates one FastRing descriptor per libcurl socket and
resubmits its poll after every event; `CURLMOPT_TIMERFUNCTION` maps onto
`SetFastRingTimeout()`. Finished transfers are not reaped inside those callbacks —
`curl_multi_info_read()` is drained from a flush handler armed at most once per loop
iteration, so completion handlers run in the ring thread after CQ processing, like any
other FastRing callback.

Everything in this module must be called from the ring thread. Use
[ThreadCall.md](ThreadCall.md) to get there from a foreign thread.

## Lifecycle

```c
struct Fetch* CreateFetch(struct FastRing* ring);
void ReleaseFetch(struct Fetch* fetch);
int GetFetchTransmissionCount(struct Fetch* fetch);
```

`CreateFetch()` does not initialize libcurl globally. Call `curl_global_init()` once
before it, as with any libcurl program.

The multi handle is created with `CURLMOPT_PIPELINING` set to
`CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX`, so HTTP/2 requests to the same origin share a
connection. The share handle carries `CURL_LOCK_DATA_COOKIE` and `CURL_LOCK_DATA_HSTS`,
which provides common storage for that state — but **neither engine is switched on by
`Fetch`**: libcurl keeps cookies off until `CURLOPT_COOKIEFILE` is set, and HSTS off
until `CURLOPT_HSTS` or `CURLOPT_HSTS_CTRL` is. **Switching them on stays a property of
each easy handle**, which an application does per transmission; the share only makes the
storage common to those handles that did switch them on. A jar is therefore per `Fetch`
instance rather than per request, and separate jars mean separate instances.

`ReleaseFetch()` disarms the timer and the flush handler, then walks every easy handle
still registered and completes it with `FETCH_STATUS_INCOMPLETE` before destroying the
multi and share handles. Handlers therefore run during shutdown — see
[Shutdown](#shutdown).

`GetFetchTransmissionCount()` returns the number of transmissions currently registered
with the multi handle, which is the natural "is anything in flight" guard:

```c
if (GetFetchTransmissionCount(transport->fetch) == 0)
{
  // Previous poll finished, start the next one
}
```

## Starting a Transmission

```c
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

struct FetchTransmission* MakeExtendedFetchTransmission(
  struct Fetch* fetch,
  struct FetchTransmission* transmission,
  CURL* easy,
  int option,
  HandleFetchFunction function,
  void* parameter1,
  void* parameter2);
```

Both return `NULL` on failure, having already released the easy handle, and both start
the transfer immediately — `curl_multi_socket_action()` is called before they return.
`parameter1` and `parameter2` are opaque and are handed back to the completion handler.

### MakeSimpleFetchTransmission

Builds the easy handle itself:

| Argument | Effect |
| --- | --- |
| `location` | `CURLOPT_URL`; `CURLOPT_FOLLOWLOCATION` is always enabled |
| `headers` | `CURLOPT_HTTPHEADER`, ignored when `NULL` |
| `token` | `CURLAUTH_BEARER` with `CURLOPT_XOAUTH2_BEARER`, ignored when `NULL` |
| `data` / `length` | When `data` is not `NULL`: `CURLOPT_POST` with `CURLOPT_COPYPOSTFIELDS` |

The body is copied, the header list is not. `FETCH_OPTION_HANDLE_CONTENT` is always
used, so the response body is accumulated for the handler.

```c
MakeSimpleFetchTransmission(fetch, "https://www.google.com/", NULL, NULL, NULL, 0, HandleResponseEvent, "simple", NULL);
```

### MakeExtendedFetchTransmission

Takes an easy handle the caller has prepared, which is the form to use whenever a
libcurl option beyond the four above is needed:

```c
if (easy = curl_easy_init())
{
  curl_easy_setopt(easy, CURLOPT_CURLU,             location);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER,        headers);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING,   "");
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,    1);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,        4000L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 2000L);

  MakeExtendedFetchTransmission(fetch, NULL, easy, FETCH_OPTION_HANDLE_CONTENT, HandleResponseEvent, closure, NULL);
}
```

**The easy handle is owned by `Fetch` from the call on.** It is passed to
`curl_easy_cleanup()` when the call fails and when the transmission completes; an
application must never clean it up itself, and must not keep using it after the
completion handler returns.

`FOLLOWLOCATION` is *not* set here — unlike `MakeSimpleFetchTransmission()`, an
extended transmission gets exactly the options the caller set, plus the ones below.

Pass `transmission` as `NULL` to have the object allocated. A non-`NULL` value lets the
object be embedded in a larger structure — that is how `CURLWSCore` puts
`struct FetchTransmission` first in `struct CWSTransmission`. This is the deliberate
division of labour: a module layered on `Fetch` allocates one block for its own
structure with the transmission inside it, rather than the transmission carrying
storage on the module's behalf. Such an object must come
**from the `malloc()` family**, since `Fetch` releases it with `free()`. Its
`FetchTransmission` part does not have to be pre-cleared — the constructor initializes
every field, the accumulation buffer included.

### Options reserved by Fetch

These are set on every easy handle and must not be overwritten:

| Option | Value |
| --- | --- |
| `CURLOPT_PRIVATE` | The `FetchTransmission` — this is how completions are matched, so it is unavailable to applications |
| `CURLOPT_SHARE` | The `Fetch` share handle |
| `CURLOPT_DNS_CACHE_TIMEOUT` | `600` |
| `CURLOPT_ALTSVC` | `""`, enabling in-memory Alt-Svc |

`option` adds to that:

- `FETCH_OPTION_HANDLE_CONTENT` — `Fetch` installs its own `CURLOPT_WRITEFUNCTION` and
  `CURLOPT_WRITEDATA` and accumulates the response body, delivering it to the handler
  as `data` / `length`.
- `FETCH_OPTION_SET_HANDLER_DATA` — `CURLOPT_READDATA`, `CURLOPT_WRITEDATA` and
  `CURLOPT_HEADERDATA` are pointed at the `FetchTransmission`, leaving the callback
  functions themselves to the caller. This is what a module layered on `Fetch` uses.

The two are independent bits, but combining them is pointless: `HANDLE_CONTENT` owns
the write path.

### Argument lifetimes

libcurl does not copy a header list nor a `CURLU` handle, so anything passed through
`CURLOPT_HTTPHEADER`, `CURLOPT_CURLU` or `CURLOPT_CONNECT_TO` must stay alive until the
transmission completes, and is the application's to free afterwards. Owning them for
the lifetime of the enclosing object and freeing them next to `ReleaseFetch()` is the
usual arrangement:

```c
curl_slist_free_all(transport->list);
curl_url_cleanup(transport->location);
ReleaseFetch(transport->fetch);
```

## Hosting a Protocol Module

A protocol on top of libcurl needs the same things every time: a session object with its
own state, its own libcurl callbacks, and a completion path. `Fetch` provides the last of
those and stays out of the way of the first two, which is what makes it a host rather
than an HTTP helper. Adding a protocol requires **no change to this module** — both
`CURLWSCore` and `gRPCClient` are written entirely against the API above.

The two mechanisms that make it work were introduced for exactly this purpose:

- **A caller-supplied `transmission`.** A module declares
  `struct FetchTransmission super;` as the first member of its own session structure,
  allocates one block, and hands the inner transmission to
  `MakeExtendedFetchTransmission()`. One allocation, one lifetime, and a cast in either
  direction. `CURLWSCore` does this with `struct CWSTransmission`, `gRPCClient` with
  `struct GRPCTransmission` — and `gRPCClient` nests once more, putting
  `GRPCTransmission` first in `struct GRPCCall`.
- **`FETCH_OPTION_SET_HANDLER_DATA`.** `CURLOPT_READDATA`, `CURLOPT_WRITEDATA` and
  `CURLOPT_HEADERDATA` are pointed at that object, so the module installs its own
  `CURLOPT_WRITEFUNCTION`, `CURLOPT_HEADERFUNCTION` and `CURLOPT_READFUNCTION` and
  receives its session pointer directly. `Fetch` touches neither the protocol framing
  nor the payload.

What a module gets for free: one multi handle and one connection pool shared with every
other session, HTTP/2 multiplexing, a share handle that pools cookie and HSTS state once
an application enables those engines, socket polling and timers on the ring, and
cancellation and shutdown semantics that are the same for every protocol. A transfer that
ends on its own is reaped from the flush handler, so its completion runs in the ring
thread after CQ processing; `CancelFetchTransmission()` and `ReleaseFetch()` call the
handler in place instead.

What a module owes in return:

- allocate its session object from the `malloc()` family, since `Fetch` frees it;
- leave the [reserved options](#options-reserved-by-fetch) alone, `CURLOPT_PRIVATE`
  above all;
- do its teardown in the completion handler, which is the last call it will receive;
- **never destroy the easy handle from inside one of its own libcurl callbacks.** A
  handler that cancels its own session while libcurl is calling it has to defer that
  cancellation. `CURLWSCore` delivers received frames from a ring flush handler and can
  cancel in place; `gRPCClient` dispatches frames from the write callback and therefore
  marks the request, aborts the transfer, and lets the completion path release
  everything. A new module has to pick one of those two shapes deliberately.

## Completion Handler

```c
typedef void (*HandleFetchFunction)(
  struct FetchTransmission* transmission,
  CURL* easy,
  int code,
  char* data,
  size_t length,
  void* parameter1,
  void* parameter2);
```

Called **exactly once** per transmission, always from the ring thread. `easy` is still
valid inside the call, so `curl_easy_getinfo()` works:

```c
curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type);
curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME_T, &time);
```

**The transmission and the easy handle are destroyed as soon as the handler returns,
and `data` is freed with them.** Copy anything that must outlive the call and clear
every stored pointer to the transmission.

### Completion codes

| Outcome | `code` | `data` / `length` |
| --- | --- | --- |
| libcurl finished with `CURLE_OK` | `CURLINFO_RESPONSE_CODE`, a non-negative value | Accumulated body with `FETCH_OPTION_HANDLE_CONTENT`, otherwise `NULL` / `0` |
| libcurl failed | `-CURLcode`, a negative value | `curl_easy_strerror()` text, `length` is `0` |
| `CancelFetchTransmission()` | `FETCH_STATUS_CANCELLED` (-1001) | `NULL` / `0` |
| `ReleaseFetch()` with the transfer still running | `FETCH_STATUS_INCOMPLETE` (-1000) | `NULL` / `0` |

A non-negative `code` is an HTTP status, never a `CURLcode` — every transport failure
is negative. `code` is `0` for a protocol with no response code (a `file://` URL, for
instance), so a successful HTTP request is `code > 0`.

In the failure case `data` is static text from libcurl and `length` is `0`: treat it as
a C string, not as a buffer.

`FETCH_STATUS_INCOMPLETE` doubles as the internal state of a running transmission and
is also available under the legacy name `TRANSMISSION_INCOMPLETE`.

The usual shape of a handler:

```c
static void HandleResponseEvent(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  switch (code)
  {
    case 200:
      // data / length is the response body
      break;

    case TRANSMISSION_INCOMPLETE:
      // Fetch is being released, do not start anything new
      break;

    default:
      report(LOG_WARNING, "Request failed: %s (%d)\n", data, code);
      break;
  }
}
```

### Accumulated body

With `FETCH_OPTION_HANDLE_CONTENT` the body is collected into a single buffer:

- When the response declares `Content-Length`, the buffer is allocated once at that
  size plus one byte.
- Otherwise it grows in 16 KiB steps, so a chunked response costs a `realloc()` per
  16 KiB.
- **The buffer always carries a terminating zero byte** past `length`, so text bodies
  can be handed straight to `strcspn()`, `strstr()` or `sscanf()`.
- An allocation failure aborts the transfer, which surfaces as `-CURLE_WRITE_ERROR`.

There is no incremental or streaming delivery: the handler sees the whole body at once,
or nothing. For a response too large to hold in memory, use
`FETCH_OPTION_SET_HANDLER_DATA` and install `CURLOPT_WRITEFUNCTION` yourself.

## Cancelling and Nudging

```c
void CancelFetchTransmission(struct FetchTransmission* transmission);
void TouchFetchTransmission(struct FetchTransmission* transmission);
```

`CancelFetchTransmission()` **invokes the handler synchronously**, before it returns,
with `FETCH_STATUS_CANCELLED`, and then destroys the transmission. It tolerates `NULL`.
Since the object is gone afterwards, the pointer must not be used again — the tidy
arrangement is to have the handler clear the application's pointer, so a cancel and a
natural completion leave the same state behind:

```c
static void HandleResponseEvent(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  if (parameter2 != NULL)
  {
    *(struct FetchTransmission**)parameter2 = NULL;
  }
  ...
}

transmission = MakeSimpleFetchTransmission(fetch, location, NULL, NULL, NULL, 0, HandleResponseEvent, "poll", &transmission);
```

A handler may cancel its own transmission: the nested release is detected and ignored,
so the object is freed once.

`TouchFetchTransmission()` calls `curl_multi_socket_action()` and arms the completion
drain. It is needed after something outside libcurl's knowledge changed the state of a
transfer — most commonly after `curl_easy_pause()` has un-paused a handle. It uses only
the transmission's `fetch`, so any live transmission of that instance will do.

## Shutdown

`ReleaseFetch()` calls the handler of every in-flight transmission with
`FETCH_STATUS_INCOMPLETE` while the multi handle is being torn down. A handler must
therefore be safe to run during shutdown and **must not start new transmissions on that
`Fetch`**. Handling the code explicitly, as in the example above, is enough.

Cancelling everything first is not necessary; letting `ReleaseFetch()` reap the
remainder is the intended pattern:

```c
ReleaseFetch(fetch);
ReleaseFastRing(ring);
```

## Helpers

```c
int AppendFetchParameter(CURLU* location, int size, const char* format, ...);
struct curl_slist* AppendFetchList(struct curl_slist* list, int size, const char* format, ...);
struct curl_slist* MakeFetchConnectAddress(const struct sockaddr* address);
```

`AppendFetchParameter()` formats a `name=value` pair and appends it to the query of a
`CURLU` with `CURLU_APPENDQUERY | CURLU_URLENCODE`, returning a `CURLUcode`.

`AppendFetchList()` formats a header line and appends it to a `curl_slist`, returning
the new head — so calls chain, and the result must be checked for `NULL` before use as
with any `curl_slist_append()`.

In both, `size` is the size of a scratch buffer taken from the stack with `alloca()`,
that is the maximum formatted length including the terminator. Overlong output is
**silently truncated**, so size the buffer for the data, and keep `size` modest since
it is a stack allocation.

```c
headers = AppendFetchList(NULL, 64, "Accept-Language: %s", "en-US");
headers = AppendFetchList(headers, 64, "X-Request-Number: %d", 1);

AppendFetchParameter(location, 64, "q=%s", "FastRing io_uring");
```

`MakeFetchConnectAddress()` turns a `sockaddr` into a one-element list for
`CURLOPT_CONNECT_TO`, in the `::host:port` form — empty host and empty port, meaning
*every* host and port of the request is redirected to that address. `AF_INET` and
`AF_INET6` are supported, anything else returns `NULL`. Useful for talking to a
specific node while keeping the URL, and therefore the TLS SNI and the `Host` header,
intact:

```c
if (addresses = MakeFetchConnectAddress((const struct sockaddr*)&address))
{
  curl_easy_setopt(easy, CURLOPT_URL,        "https://service.example.net/");
  curl_easy_setopt(easy, CURLOPT_CONNECT_TO, addresses);
}
```

The list is the caller's to free, after the transmission has completed.

## Tracing

`Fetch` also provides a ready-made `CURLOPT_DEBUGFUNCTION`:

```c
int HandleFetchDebug(CURL* easy, curl_infotype type, char* data, size_t size, void* closure);
```

`closure` — passed through `CURLOPT_DEBUGDATA` — is a
`void (*)(int priority, const char* format, ...)` function, invoked with `LOG_DEBUG`;
`syslog()` itself fits, as does any printf-like logger:

```c
curl_easy_setopt(easy, CURLOPT_VERBOSE,       1L);
curl_easy_setopt(easy, CURLOPT_DEBUGDATA,     report);
curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, HandleFetchDebug);
```

Headers are logged in full, payloads only as byte counts.

## Data Structures

`struct Fetch` and `struct FetchTransmission` are exposed in the header because
`CURLWSCore` embeds the latter. Applications read `transmission->easy` at most;
everything else is internal.

| `struct FetchTransmission` | Role |
| --- | --- |
| `fetch`, `easy` | Owning instance and the libcurl easy handle |
| `state` | `FETCH_STATUS_INCOMPLETE` while running, otherwise the `CURLcode` or `FETCH_STATUS_CANCELLED` |
| `condition` | Re-entrancy guard for the release path |
| `function`, `parameter1`, `parameter2`, `option` | As passed to the constructor |
| `buffer`, `length`, `capacity` | Accumulation buffer of `FETCH_OPTION_HANDLE_CONTENT` |
