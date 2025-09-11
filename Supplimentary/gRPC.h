#ifndef GRPC_H
#define GRPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define GRPC_HEADER_CONTENT_TYPE     "content-type: application/grpc"       // Mandatory
#define GRPC_HEADER_TRAILERS         "te: trailers"                         // Mandatory
#define GRPC_HEADER_TIMEOUT          "grpc-timeout: %d%c"                   // Optional (H, M, S, m for milliseconds, u for microseconds, n for nanoseconds)
#define GRPC_HEADER_ENCODING         "grpc-encoding: gzip"                  // Optional
#define GRPC_HEADER_ACCEPT_ENCODING  "grpc-accept-encoding: identity,gzip"  // Optional
#define GRPC_HEADER_AUTHORIZATION    "authorization: Bearer %s"             // Optional
#define GRPC_TRAILER_STATUS          "grpc-status: "                        // Mandatory, for example: grpc-status: 0 = GRPC_STATUS_OK
#define GRPC_TRAILER_MESSAGE         "grpc-message: "                       // Optional, for example: grpc-message: permission denied
#define GRPC_TRAILER_STATUS_DETAILS  "grpc-status-details-bin"              // Optional (google/rpc/status.proto), for example: grpc-status-details-bin: <base64-protobuf>

#define GRPC_STATUS_OK                   0
#define GRPC_STATUS_CANCELLED            1
#define GRPC_STATUS_UNKNOWN              2
#define GRPC_STATUS_INVALID_ARGUMENT     3
#define GRPC_STATUS_DEADLINE_EXCEEDED    4
#define GRPC_STATUS_NOT_FOUND            5
#define GRPC_STATUS_ALREADY_EXISTS       6
#define GRPC_STATUS_PERMISSION_DENIED    7
#define GRPC_STATUS_RESOURCE_EXHAUSTED   8
#define GRPC_STATUS_FAILED_PRECONDITION  9
#define GRPC_STATUS_ABORTED              10
#define GRPC_STATUS_OUT_OF_RANGE         11
#define GRPC_STATUS_UNIMPLEMENTED        12
#define GRPC_STATUS_INTERNAL             13
#define GRPC_STATUS_UNAVAILABLE          14
#define GRPC_STATUS_DATA_LOSS            15
#define GRPC_STATUS_UNAUTHENTICATED      16

#define GRPC_FLAG_COMPRESSED  (1 << 0)

struct gRPC
{
  uint8_t flags;    // GRPC_FLAG_*
  uint32_t length;  // Length of data (big endian)
  uint8_t data[0];  // Data of ProtoBuf message

} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif
