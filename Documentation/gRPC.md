# gRPC Wire Format Constants

Header: `Supplimentary/gRPC.h`

This header defines shared constants and the wire frame struct used by gRPC client/server modules.

## Important Macros

- standard headers:
  - `GRPC_HEADER_CONTENT_TYPE`
  - `GRPC_HEADER_TRAILERS`
  - `GRPC_HEADER_TIMEOUT`
  - `GRPC_HEADER_ENCODING`
  - `GRPC_HEADER_ACCEPT_ENCODING`
  - `GRPC_HEADER_AUTHORIZATION`
- trailer helpers:
  - `GRPC_TRAILER_STATUS`
  - `GRPC_TRAILER_MESSAGE`
  - `GRPC_TRAILER_STATUS_DETAILS`
- status codes:
  - `GRPC_STATUS_OK` .. `GRPC_STATUS_UNAUTHENTICATED`

## Wire Frame

```c
struct gRPC
{
  uint8_t flags;
  uint32_t length;  // big endian
  uint8_t data[0];
} __attribute__((packed));
```

Frame flags:

- `GRPC_FLAG_COMPRESSED` (`1 << 0`) - the payload is gzip-compressed. When clear, the
  payload is the raw protobuf message. `gRPCClient` accepts compressed frames and never
  sends them; `gRPCServer` accepts them only when the request declared
  `grpc-encoding: gzip`, and compresses a reply only when asked to and the client listed
  `gzip` in `grpc-accept-encoding`.

`length` excludes the five-byte header. `GRPC_HEADER_TIMEOUT` is a `printf` format,
`grpc-timeout: %d%c`, the letter being the unit (`H`, `M`, `S`, `m`, `u`, `n`).
`GRPC_HEADER_ENCODING` is not used by either module.

The header creates nothing and owns no memory.

