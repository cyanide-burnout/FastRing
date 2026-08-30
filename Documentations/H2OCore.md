# H2OCore API Reference

Header: `Supplimentary/H2OCore.h`

`H2OCore` integrates H2O HTTP/2 and HTTP/3 server runtime with `libuv`.

Status: within this repository the module is exercised only by `Examples/H2H3Server`
and `Examples/gRPCServer`.

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

## Core Options

The `options` argument of `CreateH2OCore()` and `UpdateH2OCoreSecurity()` packs an
ALPN selector in the low two bits plus independent flags:

- `H2OCORE_OPTION_APLN_BOTH` (1) - advertise HTTP/2 and HTTP/1.1
- `H2OCORE_OPTION_APLN_H2_ONLY` (2) - advertise HTTP/2 only
- `H2OCORE_OPTION_APLN_HTTP1_ONLY` (3) - advertise HTTP/1.1 only
- value `0` in the low bits leaves ALPN registration untouched
- `H2OCORE_OPTION_H3_ENABLE_RETRY` (`1 << 2`) - enable QUIC Retry (`send_retry`) on
  the HTTP/3 listener

ALPN is only registered when a `SSL_CTX*` is supplied.

## Core State

`core->state` reports which listeners failed to come up:

- `H2OCORE_STATE_TCP_FAILED` (`1 << 0`)
- `H2OCORE_STATE_UDP_FAILED` (`1 << 1`)

`CreateH2OCore()` can succeed with one transport down, so check the state before
assuming HTTP/3 is available.

## Routes

```c
struct H2ORoute
{
  const char* path;
  int flags;
  int options;
  int (*function)(h2o_handler_t* handler, h2o_req_t* request);
  void* closure;
};
```

The route table is terminated by an entry with a `NULL` `path`. `flags` is passed to
`h2o_config_register_path()`.

Route options:

- `H2OCORE_ROUTE_OPTION_STREAMING` (`1 << 0`) - sets
  `supports_request_streaming` on the handler, so the request body is delivered
  incrementally instead of being buffered

Inside a handler, recover the route closure with:

```c
#define H2OCORE_ROUTE_CLOSURE(type, handler)  ((type)((struct H2OHandler*)handler)->closure)
```

## Header Helpers

Three operations, each in a `ByIndex` variant taking an H2O interned token
(`H2O_TOKEN_CONTENT_TYPE`, ...) and a `ByName` variant taking a literal string. The
token form is cheaper and is the one to use for standard headers.

```c
const char* GetH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, size_t* size);
const char* GetH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, size_t* size);

int CompareH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const void* sample, size_t size);
int CompareH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const void* sample, size_t size);

int HasInH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const char* needle);
int HasInH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const char* needle);
```

- `Get...` returns a pointer to the header value with its length in `*size`, or `NULL`
  when the header is absent. The value is **not** null-terminated and points into the
  request's memory, so it is valid only while the request is.
- `Compare...` tests the whole value for exact equality with `sample`, returning
  non-zero on match and `0` when the header is absent. This is the right check for a
  fixed value: `CompareH2OHeaderByIndex(&request->headers, H2O_TOKEN_CONTENT_TYPE, H2O_STRLIT("application/grpc"))`.
- `Has...` tests whether the null-terminated `needle` appears anywhere inside the
  value, for list-valued headers such as `grpc-accept-encoding: identity,gzip`.

Only the first occurrence of a header is examined; a header repeated across several
lines is not folded.

```c
void MakeH2OPercentEncodedString(h2o_iovec_t* vector, h2o_mem_pool_t* pool, const char* text, size_t length);
```

Percent-encodes `text` into a vector allocated from the request pool, escaping
control characters, space, `%` and everything from `0x7f` up. The result is owned by
the pool and needs no freeing.

## Required H2O Patch

`Supplimentary/h2o.patch` must be applied to the H2O source tree before building
against `H2OCore`. It is not an adaptation of H2O to FastRing — it fixes a bug in
H2O's own libuv binding that only shows up in the HTTP/3 server path.

`get_sockname_uncached()` in `lib/common/socket/uv-binding.c.h` asserts that the
handle is `UV_TCP` and calls `uv_tcp_getsockname()`. QUIC sockets are driven through
`UV_POLL` handles, so the assertion fires (or the lookup silently fails in a release
build) whenever the HTTP/3 listener needs its local address. The patch accepts
`UV_POLL` handles and resolves the address through `uv_fileno()` + `getsockname()`.

Without the patch, TCP-only operation is unaffected; HTTP/3 is not usable.
