#define GRPCSERVER_INTERNAL
#define _GNU_SOURCE

#include <malloc.h>
#include <string.h>
#include <endian.h>
#include <errno.h>
#include <zlib.h>

#include "gRPCServer.h"

static const h2o_iovec_t codes[] =
{
  { H2O_STRLIT("0")  }, { H2O_STRLIT("1")  }, { H2O_STRLIT("2")  }, { H2O_STRLIT("3")  },
  { H2O_STRLIT("4")  }, { H2O_STRLIT("5")  }, { H2O_STRLIT("6")  }, { H2O_STRLIT("7")  },
  { H2O_STRLIT("8")  }, { H2O_STRLIT("9")  }, { H2O_STRLIT("10") }, { H2O_STRLIT("11") },
  { H2O_STRLIT("12") }, { H2O_STRLIT("13") }, { H2O_STRLIT("14") }, { H2O_STRLIT("15") },
  { H2O_STRLIT("16") }
};

#define GRPC_STATUS_CODE(index)      codes[index].base, codes[index].len
#define GRPC_ALLOCATION_GRANULARITY  (1 << 12)
#define GRPC_FRAME_SIZE_LIMIT        (1 << 24)

// Helpers

static void* ExpandBuffer(struct GRPCBuffer* buffer, size_t size)
{
  uint8_t* pointer;

  size    = (size + GRPC_ALLOCATION_GRANULARITY - 1) & ~(GRPC_ALLOCATION_GRANULARITY - 1);
  pointer = (uint8_t*)realloc(buffer->buffer, size);

  if (pointer != NULL)
  {
    buffer->buffer = pointer;
    buffer->size   = size;
  }

  return pointer;
}

static struct GRPCReply* AllocateReply(struct GRPCInvocation* invocation, size_t length)
{
  struct GRPCReply* reply;
  size_t size;

  size = sizeof(struct GRPCReply) + length;
  size = (size + GRPC_ALLOCATION_GRANULARITY - 1) & ~(GRPC_ALLOCATION_GRANULARITY - 1);

  if (invocation->pool != NULL)
  {
    reply            = invocation->pool;
    invocation->pool = reply->next;

    if (size <= reply->size)
    {
      reply->next        = NULL;
      reply->state       = H2O_SEND_STATE_IN_PROGRESS;
      reply->vector.base = reply->data;
      reply->vector.len  = length;
      return reply;
    }

    free(reply);
  }

  if (reply = (struct GRPCReply*)malloc(size))
  {
    reply->size        = size;
    reply->next        = NULL;
    reply->state       = H2O_SEND_STATE_IN_PROGRESS;
    reply->vector.base = reply->data;
    reply->vector.len  = length;
  }

  return reply;
}

static void PushReply(struct GRPCInvocation* invocation, struct GRPCReply* reply)
{
  if (invocation->head == NULL)
  {
    invocation->head = reply;
    invocation->tail = reply;

    if (invocation->flight == NULL)
    {
      // No arming needed while something is still in flight
      h2o_proceed_response(invocation->request);
    }
  }
  else
  {
    invocation->tail->next = reply;
    invocation->tail       = reply;
  }
}

// Inbound

static void HandleError(struct GRPCInvocation* invocation, int status)
{
  struct GRPCDispatch* dispatch;
  struct GRPCReply* reply;

  dispatch            = invocation->dispatch;
  invocation->flags  |= GRPC_IV_FLAG_IGNORING;
  invocation->status  = status;

  if (reply = AllocateReply(invocation, 0))
  {
    reply->vector.len = 0;
    reply->state      = H2O_SEND_STATE_FINAL;
    PushReply(invocation, reply);
  }

  dispatch->handle(invocation, GRPC_IV_REASON_FAILED, NULL, 0);
}

static void HandleFrame(struct GRPCInvocation* invocation, uint8_t flags, uint8_t* data, size_t length)
{
  struct GRPCDispatch* dispatch;
  z_stream stream;
  int result;

  if (flags & GRPC_FLAG_COMPRESSED)
  {
    if (~invocation->flags & GRPC_IV_FLAG_INPUT_GZIP)
    {
      HandleError(invocation, GRPC_STATUS_FAILED_PRECONDITION);
      return;
    }

    if ((length > invocation->scratch.size) &&
        (ExpandBuffer(&invocation->scratch, length) == NULL))
    {
      HandleError(invocation, GRPC_STATUS_INTERNAL);
      return;
    }

    memset(&stream, 0, sizeof(z_stream));

    result = inflateInit2(&stream, 16 + MAX_WBITS);

    stream.next_in   = data;
    stream.avail_in  = length;
    stream.next_out  = invocation->scratch.buffer;
    stream.avail_out = invocation->scratch.size;

    while ((result == Z_OK) &&
           (invocation->scratch.size < GRPC_FRAME_SIZE_LIMIT))
    {
      result                     = inflate(&stream, Z_NO_FLUSH);
      invocation->scratch.length = (uint8_t*)stream.next_out - invocation->scratch.buffer;

      if ((stream.avail_out == 0) &&
          (ExpandBuffer(&invocation->scratch, invocation->scratch.size << 1) == NULL))
      {
        inflateEnd(&stream);
        HandleError(invocation, GRPC_STATUS_INTERNAL);
        return;
      }

      stream.next_out  = invocation->scratch.buffer + invocation->scratch.length;
      stream.avail_out = invocation->scratch.size   - invocation->scratch.length;
    }

    inflateEnd(&stream);

    if (result != Z_STREAM_END)
    {
      HandleError(invocation, GRPC_STATUS_RESOURCE_EXHAUSTED);
      return;
    }

    data   = invocation->scratch.buffer;
    length = invocation->scratch.length;
  }

  dispatch = invocation->dispatch;
  dispatch->handle(invocation, GRPC_IV_REASON_RECEIVED, data, length);
}

static void HandleStreamChunk(struct GRPCInvocation* invocation, uint8_t* data, size_t length, int final)
{
  struct GRPCDispatch* dispatch;
  struct gRPC* frame;
  size_t size;

  if (invocation->flags & GRPC_IV_FLAG_IGNORING)
  {
    // There is no need to proceed following frames
    return;
  }

  // Fast path: complete data in a single chunk

  if ((final != 0) &&
      (invocation->inbound.buffer == NULL))
  {
    while (length >= sizeof(struct gRPC))
    {
      frame = (struct gRPC*)data;
      size  = be32toh(frame->length) + sizeof(struct gRPC);

      if (size > length)
      {
        // Last frame is incomplete
        break;
      }

      HandleFrame(invocation, frame->flags, frame->data, size - sizeof(struct gRPC));

      if (invocation->flags & GRPC_IV_FLAG_IGNORING)
      {
        // There is no need to proceed following frames
        return;
      }

      data   += size;
      length -= size;
    }

    if (length != 0)
    {
      HandleError(invocation, GRPC_STATUS_DATA_LOSS);
      return;
    }

    dispatch = invocation->dispatch;
    dispatch->handle(invocation, GRPC_IV_REASON_FINISHED, NULL, 0);
    return;
  }

  // Slow path: accumulate data in buffer

  if (length != 0)
  {
    size = invocation->inbound.length + length;

    if ((invocation->inbound.size < size) &&
        (ExpandBuffer(&invocation->inbound, size) == NULL))
    {
      HandleError(invocation, GRPC_STATUS_INTERNAL);
      return;
    }

    memcpy(invocation->inbound.buffer + invocation->inbound.length, data, length);
    invocation->inbound.length += length;

    data   = invocation->inbound.buffer;
    length = invocation->inbound.length;

    while (length >= sizeof(struct gRPC))
    {
      frame = (struct gRPC*)data;
      size  = be32toh(frame->length) + sizeof(struct gRPC);

      if (size > length)
      {
        // Wait for the next chunk
        break;
      }

      HandleFrame(invocation, frame->flags, frame->data, size - sizeof(struct gRPC));

      if (invocation->flags & GRPC_IV_FLAG_IGNORING)
      {
        // There is no need to proceed following frames
        return;
      }

      data   += size;
      length -= size;
    }

    if (data != invocation->inbound.buffer)
    {
      memmove(invocation->inbound.buffer, data, length);
      invocation->inbound.length = length;
    }

    if (length >= sizeof(struct gRPC))
    {
      frame = (struct gRPC*)invocation->inbound.buffer;
      size  = be32toh(frame->length) + sizeof(struct gRPC);

      if (size > GRPC_FRAME_SIZE_LIMIT)
      {
        HandleError(invocation, GRPC_STATUS_RESOURCE_EXHAUSTED);
        return;
      }

      if ((invocation->inbound.size < size) &&
          (ExpandBuffer(&invocation->inbound, size) == NULL))
      {
        HandleError(invocation, GRPC_STATUS_INTERNAL);
        return;
      }
    }
  }

  if (final != 0)
  {
    if (invocation->inbound.length != 0)
    {
      HandleError(invocation, GRPC_STATUS_DATA_LOSS);
      return;
    }

    dispatch = invocation->dispatch;
    dispatch->handle(invocation, GRPC_IV_REASON_FINISHED, NULL, 0);
  }
}

// H2O bindings

static int HandleStreamWrite(void* context, int final)
{
  struct GRPCInvocation* invocation;
  h2o_req_t* request;

  invocation = (struct GRPCInvocation*)context;
  request    = invocation->request;

  HandleStreamChunk(invocation, request->entity.base, request->entity.len, final);

  if (final == 0)
  {
    //
    request->proceed_req(request, NULL);
  }

  return 0;
}

static void HandleGeneratorProceed(h2o_generator_t* generator, h2o_req_t* request)
{
  struct GRPCInvocation* invocation;
  struct GRPCDispatch* dispatch;
  struct GRPCReply* reply;
  h2o_headers_t* storage;

  invocation = (H2O_STRUCT_FROM_MEMBER(struct GRPCInvocation, generator, generator));
  dispatch   = invocation->dispatch;

  if (reply = invocation->flight)
  {
    reply->next        = invocation->pool;
    invocation->pool   = reply;
    invocation->flight = NULL;
  }

  if (reply = invocation->head)
  {
    invocation->head    = reply->next;
    invocation->flight  = reply;
    invocation->flags  |= GRPC_IV_FLAG_SENDING * (reply->vector.len != 0);

    if (reply->state != H2O_SEND_STATE_IN_PROGRESS)
    {
      if (invocation->flags & GRPC_IV_FLAG_SENDING)  storage = &request->res.trailers;
      else                                           storage = &request->res.headers;

      h2o_add_header_by_str(&request->pool, storage, H2O_STRLIT("grpc-status"), 0, NULL, GRPC_STATUS_CODE(invocation->status));
      h2o_add_header_by_str(&request->pool, storage, H2O_STRLIT("grpc-message"), 0, NULL, invocation->message.base, invocation->message.len);
    }

    h2o_send(request, &reply->vector, 1, reply->state);
  }
}

static void HandleGeneratorStop(h2o_generator_t* generator, h2o_req_t* request)
{
  struct GRPCInvocation* invocation;
  struct GRPCDispatch* dispatch;
  struct GRPCReply* reply;

  invocation = (H2O_STRUCT_FROM_MEMBER(struct GRPCInvocation, generator, generator));
  dispatch   = invocation->dispatch;

  dispatch->handle(invocation, GRPC_IV_REASON_FAILED, NULL, 0);
}

static void HandleRequestDispose(void* pointer)
{
  struct GRPCInvocation* invocation;
  struct GRPCDispatch* dispatch;

  invocation         = *(struct GRPCInvocation**)pointer;
  dispatch           = invocation->dispatch;
  invocation->flags &= ~GRPC_IV_FLAG_ACTIVE;

  dispatch->handle(invocation, GRPC_IV_REASON_RELEASED, NULL, 0);
  ReleaseGRPCInvocation(invocation);
}

int HandleGRPCDispatchRequest(h2o_handler_t* handler, h2o_req_t* request)
{
  struct GRPCDispatch* dispatch;
  struct GRPCInvocation* invocation;

  int result;
  char* parts[2];
  const char* delimiter;
  struct GRPCInvocation** pointer;
  const ProtobufCMethodDescriptor* descriptor;

  dispatch = H2OCORE_ROUTE_CLOSURE(struct GRPCDispatch*, handler);

  if ((request->path.len > 1) &&
      h2o_memis(request->method.base, request->method.len, H2O_STRLIT("POST")) &&
      CompareH2OHeaderByIndex(&request->headers, H2O_TOKEN_CONTENT_TYPE, H2O_STRLIT("application/grpc")) &&
      CompareH2OHeaderByName(&request->headers, H2O_STRLIT("te"), H2O_STRLIT("trailers")))
  {
    // Initial gRPC response parts

    request->res.status = 200;
    request->res.reason = "OK";

    h2o_add_header(&request->pool, &request->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/grpc"));
    h2o_add_header_by_str(&request->pool, &request->res.headers, H2O_STRLIT("te"), 0, NULL, H2O_STRLIT("trailers"));

    // Extract full service and method names

    descriptor = NULL;

    if ((delimiter = memrchr(request->path.base, '/', request->path.len)) &&
        (delimiter > request->path.base))
    {
      parts[0] = strndupa(request->path.base + 1, delimiter - request->path.base - 1);
      parts[1] = strndupa(delimiter + 1, request->path.base + request->path.len - delimiter - 1);

      if (strcmp(parts[0], dispatch->descriptor->name) == 0)
      {
        // Short path: protobuf-c's optimized search
        descriptor = protobuf_c_service_descriptor_get_method_by_name(dispatch->descriptor, parts[1]);
      }
    }

    // Initial condition checks

    if (descriptor == NULL)
    {
      h2o_add_header_by_str(&request->pool, &request->res.trailers, H2O_STRLIT("grpc-status"), 0, NULL, GRPC_STATUS_CODE(GRPC_STATUS_UNIMPLEMENTED));
      h2o_send_inline(request, "", 0);
      return 0;
    }

    if ((dispatch->authorize != NULL) &&
        (result = dispatch->authorize(dispatch, descriptor, request)))
    {
      h2o_add_header_by_str(&request->pool, &request->res.trailers, H2O_STRLIT("grpc-status"), 0, NULL, GRPC_STATUS_CODE(result));
      h2o_send_inline(request, "", 0);
      return 0;
    }

    // Prepare invocation

    invocation = (struct GRPCInvocation*)calloc(1, sizeof(struct GRPCInvocation));

    if (invocation == NULL)
    {
      h2o_add_header_by_str(&request->pool, &request->res.trailers, H2O_STRLIT("grpc-status"), 0, NULL, GRPC_STATUS_CODE(GRPC_STATUS_INTERNAL));
      h2o_send_inline(request, "", 0);
      return 0;
    }

    // A hacky way to handle request disposal
    pointer  = (struct GRPCInvocation**)h2o_mem_alloc_shared(&request->pool, sizeof(struct GRPCInvocation*), HandleRequestDispose);
    *pointer = invocation;

    invocation->generator.proceed = HandleGeneratorProceed;
    invocation->generator.stop    = HandleGeneratorStop;
    invocation->dispatch          = dispatch;
    invocation->request           = request;
    invocation->descriptor        = descriptor;
    invocation->message           = h2o_iovec_init(H2O_STRLIT(""));
    invocation->status            = GRPC_STATUS_OK;
    invocation->count             = 1;

    h2o_start_response(request, &invocation->generator);

    invocation->flags =
      GRPC_IV_FLAG_ACTIVE |
      GRPC_IV_FLAG_INPUT_GZIP  * !!CompareH2OHeaderByName(&request->headers, H2O_STRLIT("grpc-encoding"), H2O_STRLIT("gzip")) |
      GRPC_IV_FLAG_OUTPUT_GZIP * !!HasInH2OHeaderByName(&request->headers, H2O_STRLIT("grpc-accept-encoding"), "gzip");

    if (invocation->flags & GRPC_IV_FLAG_OUTPUT_GZIP)  h2o_add_header_by_str(&request->pool, &request->res.headers, H2O_STRLIT("grpc-encoding"), 0, NULL, H2O_STRLIT("gzip"));
    else                                               h2o_add_header_by_str(&request->pool, &request->res.headers, H2O_STRLIT("grpc-encoding"), 0, NULL, H2O_STRLIT("identity"));

    dispatch->handle(invocation, GRPC_IV_REASON_CREATED, NULL, 0);

    // Proceed invocation

    if (request->proceed_req != NULL)
    {
      // See h2o_handler_t::supports_request_streaming for details

      request->write_req.ctx  = invocation;
      request->write_req.cb   = HandleStreamWrite;

      HandleStreamChunk(invocation, request->entity.base, request->entity.len, 0);

      request->proceed_req(request, NULL);
    }
    else
    {
      // Complete request has been received
      HandleStreamChunk(invocation, request->entity.base, request->entity.len, 1);
    }

    return 0;
  }

  return -1;
}

// Interace

void HoldGRPCInvocation(struct GRPCInvocation* invocation)
{
  if (invocation != NULL)
  {
    // Increment reference count
    invocation->count ++;
  }
}

void ReleaseGRPCInvocation(struct GRPCInvocation* invocation)
{
  struct GRPCReply* reply;

  if ((invocation != NULL) &&
      !(-- invocation->count))
  {
    while (reply = invocation->head)
    {
      invocation->head = reply->next;
      free(reply);
    }

    while (reply = invocation->pool)
    {
      invocation->pool = reply->next;
      free(reply);
    }

    free(invocation->inbound.buffer);
    free(invocation->scratch.buffer);
    free(invocation->flight);
    free(invocation);
  }
}

int TransmitGRPCPing(struct GRPCInvocation* invocation)
{
  struct GRPCReply* reply;

  if ((invocation != NULL) &&
      (invocation->flags & GRPC_IV_FLAG_ACTIVE))
  {
    if (reply = AllocateReply(invocation, sizeof(struct gRPC)))
    {
      reply->state = H2O_SEND_STATE_IN_PROGRESS;
      memset(reply->data, 0, sizeof(struct gRPC));
      PushReply(invocation, reply);
      return 0;
    }

    return -ENOMEM;
  }

  return -EINVAL;
}

int TransmitGRPCReply(struct GRPCInvocation* invocation, const ProtobufCMessage* message, uint8_t flags)
{
  struct GRPCReply* reply;
  struct gRPC* frame;
  z_stream stream;
  size_t length;
  size_t size;
  int result;

  if ((message    != NULL) &&
      (invocation != NULL) &&
      (invocation->flags & GRPC_IV_FLAG_ACTIVE))
  {
    length  = protobuf_c_message_get_packed_size(message);
    flags  &= ~(GRPC_FLAG_COMPRESSED * !(invocation->flags & GRPC_IV_FLAG_OUTPUT_GZIP));

    if (flags & GRPC_FLAG_COMPRESSED)
    {
      if ((invocation->scratch.size < length) &&
          (ExpandBuffer(&invocation->scratch, length) == NULL))
      {
        //
        return -ENOMEM;
      }

      // Expected size and extra space for gzip's header and trailer
      size = compressBound(length) + 24;

      if (reply = AllocateReply(invocation, size + sizeof(struct gRPC)))
      {
        memset(&stream, 0, sizeof(z_stream));

        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
          free(reply);
          return -EINVAL;
        }

        reply->state = H2O_SEND_STATE_IN_PROGRESS;
        frame        = (struct gRPC*)reply->data;
        frame->flags = flags;

        stream.next_in   = invocation->scratch.buffer;
        stream.avail_in  = protobuf_c_message_pack(message, stream.next_in);
        stream.next_out  = frame->data;
        stream.avail_out = size;

        result = deflate(&stream, Z_FINISH);

        frame->length     = htobe32(stream.total_out);
        reply->vector.len = stream.total_out + sizeof(struct gRPC);

        deflateEnd(&stream);

        if (result != Z_STREAM_END)
        {
          free(reply);
          return -EIO;
        }

        PushReply(invocation, reply);
        return 0;
      }
    }

    flags &= ~GRPC_FLAG_COMPRESSED;

    if (reply = AllocateReply(invocation, length + sizeof(struct gRPC)))
    {
      reply->state  = H2O_SEND_STATE_IN_PROGRESS;
      frame         = (struct gRPC*)reply->data;
      frame->flags  = flags;
      frame->length = htobe32(length);
      protobuf_c_message_pack(message, frame->data);
      PushReply(invocation, reply);
      return 0;
    }

    return -ENOMEM;
  }

  return -EINVAL;
}

int TransmitGRPCStatus(struct GRPCInvocation* invocation, int status, const char* message)
{
  struct GRPCReply* reply;
  size_t length;

  if ((invocation != NULL) &&
      (status >= GRPC_STATUS_OK) &&
      (status <= GRPC_STATUS_UNAUTHENTICATED) &&
      (invocation->flags & GRPC_IV_FLAG_ACTIVE))
  {
    invocation->status = status;

    if ((message    != NULL) &&
        (message[0] != '\0'))
    {
      length = strlen(message);
      MakeH2OPercentEncodedString(&invocation->message, &invocation->request->pool, message, length);
    }

    if (reply = AllocateReply(invocation, 0))
    {
      reply->vector.len = 0;
      reply->state      = H2O_SEND_STATE_FINAL;
      PushReply(invocation, reply);
      return 0;
    }

    return -ENOMEM;
  }

  return -EINVAL;
}
