# Examples

Each example builds independently from its own directory. There is no top-level build
target.

```bash
cd Examples/CURLWS
make
./curlwstest
```

Dependencies are resolved through `pkg-config` in each local `Makefile`. Every example
compiles the core objects it needs directly out of `Ring/` and `Supplimentary/`, so no
library has to be installed first.

## Index

| Directory | Target | Demonstrates | Extra dependencies |
| --- | --- | --- | --- |
| `AAA` | - | `FastSocket` usage in a RADIUS client | - |
| `Avahi` | `avahitest` | `FastAvahiPoll` service discovery | `avahi-client` |
| `CURLWS` | `curlwstest` | `Fetch` + `CURLWSCore` WebSocket client | `libcurl` |
| `Fetch` | `fetchtest` | `Fetch` HTTP client, every completion path | `libcurl` |
| `H2H3Server` | `h2h3test` | `H2OCore` + `FastUVLoop` + `PicoBundle`, HTTP/2 and HTTP/3 | `libuv`, `openssl`, `zlib`, `brotli`, patched H2O |
| `SMBClient` | `smbpipeecho` | `SambarAdapter` named pipe client | `libsmb2`, `krb5` |
| `SMBServer` | `smbserver` | `SambarAdapter` SMB2 server | `libsmb2`, `krb5` |
| `SSL` | `ssltest` | `FastBIO` + `SSLSocket` + `Resolver` | `openssl`, `c-ares` |
| `gRPCClient` | `grpctest` | `gRPCClient` + `ProtoBuf` over `Fetch` | `libcurl`, `protobuf-c` |
| `gRPCServer` | `grpctest` | `gRPCServer` + `ProtoBuf` over `H2OCore` | `libuv`, `openssl`, `protobuf-c`, patched H2O |

Most examples also link `jemalloc`.

`Examples/SMBClientPipe` is currently empty.

## Notes per Example

### Fetch

`FetchTest.c` issues five requests against `www.google.com` in one ring and shows all
four ways a transmission can end — HTTP status, negated `CURLcode`,
`FETCH_STATUS_CANCELLED` and `FETCH_STATUS_INCOMPLETE`. It needs outbound HTTPS and DNS;
the stalled request is aimed at a local listening socket, so it does not depend on the
network. See [Documentations/Fetch.md](../Documentations/Fetch.md).

### AAA

An extract from a production system showing how to drive `FastSocket`. Start from
`AAAClient.c`; `RADIUSTools.c` is protocol support code. There is no `Makefile` — the
sources are meant to be read, or dropped into an existing build.

### H2H3Server and gRPCServer

Both need an H2O source tree with `Supplimentary/h2o.patch` applied:

```bash
git clone https://github.com/h2o/h2o.git
cd h2o
git submodule update --init --recursive
git apply /path/to/FastRing/Supplimentary/h2o.patch
```

The patch fixes H2O's own libuv binding for the HTTP/3 path; see
[Documentations/H2OCore.md](../Documentations/H2OCore.md). Without it, HTTP/3 does not
work.

`H2H3Server` expects a certificate bundle (`bundle.pem`) next to the binary.

### SMBServer

`test-cifs.sh` and `test-echo.sh` drive the server; both take an optional host and
port, defaulting to `127.0.0.1` and `4450`.

### gRPCClient and gRPCServer

`gRPCTest.proto` is shared. Generate the protobuf-c sources before building:

```bash
protoc-c --c_out=. gRPCTest.proto
```

The server can be exercised with `grpcurl`:

```bash
grpcurl -vv -plaintext -proto gRPCTest.proto -import-path . -d '{"text":"one"}' -rpc-header "grpc-encoding: gzip" -rpc-header "grpc-accept-encoding: gzip" -rpc-header "authorization: Bearer token" <host>:8080 demo.Echoer/UnaryEcho
```

`gRPCClient` additionally ships `gRPCTest.py` as a reference implementation of the
same calls against the Python gRPC stack.
