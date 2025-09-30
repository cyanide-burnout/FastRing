#include "gRPCClient.h"

#include <stdlib.h>
#include <endian.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

// Transport

#define GRPC_TRANSMISSION_LENGTH     sizeof(struct GRPCCall)
#define GRPC_STRING_BUFFER_LENGTH    2048
#define GRPC_ALLOCATION_GRANULARITY  (1 << 12)
#define GRPC_FRAME_SIZE_LIMIT        (1 << 24)

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

static int HandleFrame(struct GRPCTransmission* transmission, uint8_t flags, uint8_t* data, size_t length)
{
  z_stream stream;
  int result;

  if (flags & GRPC_FLAG_COMPRESSED)
  {
    if ((length > transmission->scratch.size) &&
        (ExpandBuffer(&transmission->scratch, length) == NULL))
    {
      // Allocation error
      return -1;
    }

    memset(&stream, 0, sizeof(z_stream));

    result = inflateInit2(&stream, 16 + MAX_WBITS);

    stream.next_in   = data;
    stream.avail_in  = length;
    stream.next_out  = transmission->scratch.buffer;
    stream.avail_out = transmission->scratch.size;

    while ((result == Z_OK) &&
           (transmission->scratch.size < GRPC_FRAME_SIZE_LIMIT))
    {
      result                       = inflate(&stream, Z_NO_FLUSH);
      transmission->scratch.length = (uint8_t*)stream.next_out - transmission->scratch.buffer;

      if ((stream.avail_out == 0) &&
          (ExpandBuffer(&transmission->scratch, transmission->scratch.size << 1) == NULL))
      {
        inflateEnd(&stream);
        return -1;
      }

      stream.next_out  = transmission->scratch.buffer + transmission->scratch.length;
      stream.avail_out = transmission->scratch.size   - transmission->scratch.length;
    }

    inflateEnd(&stream);

    if (result != Z_STREAM_END)
    {
      // Decompression error
      return -1;
    }

    data   = transmission->scratch.buffer;
    length = transmission->scratch.length;
  }

  return transmission->function(transmission->closure, transmission, GRPCCLIENT_REASON_FRAME, flags, (char*)data, length);
}

static size_t HandleHeader(void* buffer, size_t size, size_t count, void* data)
{
  struct GRPCTransmission* transmission;
  char* header;

  transmission  = (struct GRPCTransmission*)data;
  header        = (char*)buffer;
  size         *= count;

  if ((size > (sizeof(GRPC_TRAILER_STATUS) - 1)) &&
      (strncasecmp((const char*)buffer, GRPC_TRAILER_STATUS, sizeof(GRPC_TRAILER_STATUS) - 1) == 0))
  {
    //
    transmission->status = strtol((char*)buffer + sizeof(GRPC_TRAILER_STATUS) - 1, NULL, 10);
  }

  if ((size > (sizeof(GRPC_TRAILER_MESSAGE) - 1)) &&
      (strncasecmp((const char*)buffer, GRPC_TRAILER_MESSAGE, sizeof(GRPC_TRAILER_MESSAGE) - 1) == 0))
  {
    free(transmission->message);
    transmission->message = strndup((char*)buffer + sizeof(GRPC_TRAILER_MESSAGE) - 1, size - sizeof(GRPC_TRAILER_MESSAGE) - 1);
  }

  return size;
}

static size_t HandleWrite(void* buffer, size_t size, size_t count, void* data)
{
  struct GRPCTransmission* transmission;
  struct gRPC* frame;
  uint8_t* pointer;
  size_t length;

  transmission  = (struct GRPCTransmission*)data;
  size         *= count;
  length        = transmission->inbound.length + size;

  if ((transmission->inbound.size < length) &&
      (ExpandBuffer(&transmission->inbound, length) == NULL))
  {
    // Cannot proceed chunk
    return CURL_WRITEFUNC_ERROR;
  }

  memcpy(transmission->inbound.buffer + transmission->inbound.length, buffer, size);
  transmission->inbound.length += size;

  while (transmission->inbound.length >= sizeof(struct gRPC))
  {
    frame  = (struct gRPC*)transmission->inbound.buffer;
    length = sizeof(struct gRPC) + be32toh(frame->length);

    if (length >= GRPC_FRAME_SIZE_LIMIT)
    {
      // It is too much :)
      return CURL_WRITEFUNC_ERROR;
    }

    if (transmission->inbound.length >= length)
    {
      if (HandleFrame(transmission, frame->flags, frame->data, length - sizeof(struct gRPC)) != 0)
      {
        // Cannot proceed frame
        return CURL_WRITEFUNC_ERROR;
      }

      memmove(transmission->inbound.buffer, transmission->inbound.buffer + length, transmission->inbound.length - length);
      transmission->inbound.length -= length;
      continue;
    }

    if ((transmission->inbound.size < length) &&
        (ExpandBuffer(&transmission->inbound, length) == NULL))
    {
      // Cannot proceed frame
      return CURL_WRITEFUNC_ERROR;
    }

    break;
  }

  return size;
}

static size_t HandleRead(void* buffer, size_t size, size_t count, void* data)
{
  struct GRPCTransmission* transmission;
  struct GRPCFrame* frame;

  transmission  = (struct GRPCTransmission*)data;
  frame         = transmission->head;
  size         *= count;

  if (frame != NULL)
  {
    if (frame->data == NULL)
    {
      // End of outbound stream
      return 0;
    }

    if (frame->length > size)
    {
      memcpy(buffer, frame->data, size);
      frame->data   += size;
      frame->length -= size;
      return size;
    }

    memcpy(buffer, frame->data, frame->length);
    transmission->head = frame->next;
    frame->next        = transmission->heap;
    transmission->heap = frame;
    return frame->length;
  }

  return CURL_READFUNC_PAUSE;
}

static void HandleFetch(struct FetchTransmission* super, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  struct GRPCTransmission* transmission;
  struct GRPCFrame* frame;
  void* closure;

  transmission = (struct GRPCTransmission*)super;

  if (code != 200)
  {
    // An HTTP or CURL/Fetch error occured
    transmission->function(transmission->closure, transmission, GRPCCLIENT_REASON_STATUS, code, data, 0);
  }
  else
  {
    // gRPC status
    transmission->function(transmission->closure, transmission, GRPCCLIENT_REASON_STATUS, transmission->status, transmission->message, 0);
  }

  while (frame = transmission->head)
  {
    transmission->head = frame->next;
    free(frame);
  }

  while (frame = transmission->heap)
  {
    transmission->heap = frame->next;
    free(frame);
  }

  free(transmission->message);
  free(transmission->inbound.buffer);
  free(transmission->scratch.buffer);
}

struct GRPCMethod* CreateGRPCMethod(const char* location, const char* package, const char* service, const char* name, const char* token, long timeout, char resolution)
{
  char* scheme;
  struct GRPCMethod* method;
  char buffer[GRPC_STRING_BUFFER_LENGTH];

  if (method = (struct GRPCMethod*)calloc(1, sizeof(struct GRPCMethod)))
  {
    method->location = curl_url();
    method->count    = 1;
    method->type     = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;

    if (package != NULL)  snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, "/%s.%s/%s", package, service, name);
    else                  snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, "/%s/%s", service, name);

    if ((method->location == NULL) ||
        (curl_url_set(method->location, CURLUPART_URL, location, 0) != CURLE_OK) ||
        (curl_url_set(method->location, CURLUPART_PATH, buffer, 0)  != CURLE_OK))
    {
      curl_url_cleanup(method->location);
      free(method);
      return NULL;
    }

    scheme = NULL;

    if ((curl_url_get(method->location, CURLUPART_SCHEME, &scheme, 0) == CURLE_OK) &&
        (scheme != NULL) &&
        (strcmp(scheme, "https") == 0))
    {
      //
      method->type = CURL_HTTP_VERSION_2TLS;
    }

    curl_free(scheme);

    curl_url_set(method->location, CURLUPART_USER,     NULL, 0);
    curl_url_set(method->location, CURLUPART_PASSWORD, NULL, 0);
    curl_url_set(method->location, CURLUPART_OPTIONS,  NULL, 0);
    curl_url_set(method->location, CURLUPART_QUERY,    NULL, 0);
    curl_url_set(method->location, CURLUPART_FRAGMENT, NULL, 0);

    method->headers = curl_slist_append(method->headers, GRPC_HEADER_TRAILERS);
    method->headers = curl_slist_append(method->headers, GRPC_HEADER_CONTENT_TYPE);
    method->headers = curl_slist_append(method->headers, GRPC_HEADER_ACCEPT_ENCODING);

    if ((timeout > 0) &&
        ((resolution == 'm') ||
         (resolution == 'S')))
    {
      snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, GRPC_HEADER_TIMEOUT, timeout, resolution);
      method->headers    = curl_slist_append(method->headers, buffer);
      method->timeout    = timeout;
      method->resolution = resolution;
    }

    if ((token    != NULL) &&
        (token[0] != '\0') &&
        (snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, GRPC_HEADER_AUTHORIZATION, token) > 0))
    {
      //
      method->headers = curl_slist_append(method->headers, buffer);
    }
  }

  return method;
}

void ReleaseGRPCMethod(struct GRPCMethod* method)
{
  if ((method != NULL) &&
      !(-- method->count))
  {
    curl_slist_free_all(method->headers);
    curl_url_cleanup(method->location);
    free(method);
  }
}

void HoldGRPCMethod(struct GRPCMethod* method)
{
  if (method != NULL)
  {
    //
    method->count ++;
  }
}

struct GRPCTransmission* MakeGRPCTransmission(struct Fetch* fetch, struct GRPCMethod* method, HandleGRPCEventFunction function, void* closure)
{
  struct GRPCTransmission* transmission;
  CURL* easy;

  transmission = NULL;
  easy         = NULL;

  if ((method       != NULL) &&
      (transmission  = (struct GRPCTransmission*)calloc(1, GRPC_TRANSMISSION_LENGTH)) &&
      (easy          = curl_easy_init()))
  {
    transmission->function = function;
    transmission->closure  = closure;

    curl_easy_setopt(easy, CURLOPT_UPLOAD,        1L);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(easy, CURLOPT_CURLU,         method->location);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER,    method->headers);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,  method->type);

    switch (method->resolution)
    {
      case 'm':  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, method->timeout);  break;
      case 'S':  curl_easy_setopt(easy, CURLOPT_TIMEOUT,    method->timeout);  break;
    }

    curl_easy_setopt(easy, CURLOPT_READFUNCTION,   HandleRead);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,  HandleWrite);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, HandleHeader);

    return (struct GRPCTransmission*)MakeExtendedFetchTransmission(fetch, &transmission->super, easy, FETCH_OPTION_SET_HANDLER_DATA, HandleFetch, NULL, NULL);
  }

  free(transmission);
  return NULL;
}

void CancelGRPCTransmission(struct GRPCTransmission* transmission)
{
  if (transmission != NULL)
  {
    // Resources will be released by HandleFetch()
    CancelFetchTransmission(&transmission->super);
  }
}

struct GRPCFrame* AllocateGRPCFrame(struct GRPCTransmission* transmission, size_t length)
{
  struct GRPCFrame* frame;
  size_t size;

  if (transmission == NULL)
  {
    //
    return NULL;
  }

  size = sizeof(struct GRPCFrame) + sizeof(struct gRPC) + length;
  size = (size + GRPC_ALLOCATION_GRANULARITY - 1) & ~(GRPC_ALLOCATION_GRANULARITY - 1);

  if (transmission->heap != NULL)
  {
    frame              = transmission->heap;
    transmission->heap = frame->next;

    if (size <= frame->size)
    {
      frame->next   = NULL;
      frame->data   = frame->buffer + sizeof(struct gRPC);
      frame->length = length;
      return frame;
    }

    free(frame);
  }

  if (frame = (struct GRPCFrame*)malloc(size))
  {
    frame->transmission = transmission;
    frame->size         = size;
    frame->next         = NULL;
    frame->data         = frame->buffer + sizeof(struct gRPC);
    frame->length       = length;
  }

  return frame;
}

void TransmitGRPCFrame(struct GRPCFrame* frame)
{
  struct GRPCTransmission* transmission;
  struct gRPC* header;

  transmission = frame->transmission;

  if (frame->data != NULL)
  {
    header          = (struct gRPC*)frame->buffer;
    header->flags   = 0;
    header->length  = htobe32(frame->length);
    frame->data     = frame->buffer;
    frame->length  += sizeof(struct gRPC);
  }

  if (transmission->head != NULL)
  {
    transmission->tail->next = frame;
    transmission->tail       = frame;
  }
  else
  {
    transmission->head = frame;
    transmission->tail = frame;
  }

  if (transmission->head == transmission->tail)
  {
    curl_easy_pause(transmission->super.easy, CURLPAUSE_SEND_CONT);
    TouchFetchTransmission(&transmission->super);
  }
}

int TransmitGRPCMessage(struct GRPCTransmission* transmission, const ProtobufCMessage* message, int final)
{
  struct GRPCFrame* frame;
  size_t length;
  int result;

  result = 0;

  if ((message != NULL) &&
      (length   = protobuf_c_message_get_packed_size(message)) &&
      (frame    = AllocateGRPCFrame(transmission, length)))
  {
    frame->length = protobuf_c_message_pack(message, frame->data);
    TransmitGRPCFrame(frame);
    result ++;
  }

  if ((final != 0) &&
      (frame  = AllocateGRPCFrame(transmission, 0)))
  {
    frame->data = NULL;
    TransmitGRPCFrame(frame);
    result ++;
  }

  return result;
}

// Service

static int HandleCall(void* closure, struct GRPCTransmission* transmission, int reason, int parameter, char* data, size_t length)
{
  struct GRPCCall* call;
  ProtobufCMessage* message;
  struct GRPCService* private;

  call    = (struct GRPCCall*)transmission;
  private = (struct GRPCService*)closure;

  switch (reason)
  {
    case GRPCCLIENT_REASON_FRAME:
      if ((call->function != NULL) && 
          (message         = protobuf_c_message_unpack(call->descriptor, NULL, length, (uint8_t*)data)))
      {
        call->function(message, call->closure);
        call->function = NULL;
        protobuf_c_message_free_unpacked(message, NULL);
      }
      break;

    case GRPCCLIENT_REASON_STATUS:
      if (call->function != NULL)
      {
        // Call handler has to be notified anyway
        call->function(NULL, call->closure);
      }

      if (((parameter        != GRPC_STATUS_OK) ||
           (call->function   != NULL))          &&
          (private->function != NULL))
      {
        // Error handler can observe an detailed error state
        private->function(private->closure, private, call->method, parameter, data);
      }

      private->super.destroy(&private->super);
      break;
  }

  return 0;
}

static void InvokeService(ProtobufCService* service, unsigned index, const ProtobufCMessage* input, ProtobufCClosure closure, void* data)
{
  struct GRPCCall* call;
  struct GRPCMethod* method;
  struct GRPCService* private;
  struct GRPCTransmission* transmission;
  const ProtobufCServiceDescriptor* descriptor1;
  const ProtobufCMethodDescriptor* descriptor2;
  char buffer[GRPC_STRING_BUFFER_LENGTH];

  private      = (struct GRPCService*)service;
  descriptor1  = service->descriptor;
  descriptor2  = descriptor1->methods + index;
  method       = private->methods     + index;

  if (method->location == NULL)
  {
    method->location   = curl_url_dup(private->location);
    method->headers    = private->headers;
    method->type       = private->type;
    method->timeout    = private->timeout;
    method->resolution = private->resolution;

    if ((snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, "/%s/%s", descriptor1->name, descriptor2->name) < 0) ||
        (curl_url_set(method->location, CURLUPART_PATH, buffer, 0) != CURLE_OK))
    {
      curl_url_cleanup(method->location);
      method->location = NULL;
    }
  }

  transmission = MakeGRPCTransmission(private->fetch, method, HandleCall, private);

  if ((transmission == NULL) ||
      (TransmitGRPCMessage(transmission, input, 1) != 2))
  {
    CancelGRPCTransmission(transmission);

    if (closure != NULL)
    {
      // Call handler has to be notified about error
      closure(NULL, data);
    }

    if (private->function != NULL)
    {
      // Error handler can observe an detailed error state
      private->function(private->closure, private, descriptor2->name, -CURLE_OUT_OF_MEMORY, NULL);
    }

    return;
  }

  call              = (struct GRPCCall*)transmission;
  call->method      = descriptor2->name;
  call->descriptor  = descriptor2->output;
  call->function    = closure;
  call->closure     = data;
  private->count   ++;
}

static void DestroyService(ProtobufCService* service)
{
  long index;
  struct GRPCService* private;
  const ProtobufCServiceDescriptor* descriptor;

  private    = (struct GRPCService*)service;
  descriptor = service->descriptor;

  if (!(-- private->count))
  {
    for (index = 0; index < descriptor->n_methods; index ++)
    {
      // Release all cached gRPC method locations
      curl_url_cleanup(private->methods[index].location);
    }

    curl_slist_free_all(private->headers);
    curl_url_cleanup(private->location);
    free(private);
  }
}

ProtobufCService* CreateGRPCService(struct Fetch* fetch, const ProtobufCServiceDescriptor* descriptor, const char* location, const char* token, long timeout, char resolution, HandleGRPCErrorFunction function, void* closure)
{
  char* scheme;
  struct GRPCService* private;
  char buffer[GRPC_STRING_BUFFER_LENGTH];

  if (private = (struct GRPCService*)calloc(1, sizeof(struct GRPCService) + sizeof(struct GRPCMethod) * descriptor->n_methods))
  {
    private->super.descriptor = descriptor;
    private->super.invoke     = InvokeService;
    private->super.destroy    = DestroyService;
    private->location         = curl_url();
    private->function         = function;
    private->closure          = closure;
    private->fetch            = fetch;
    private->count            = 1;
    private->type             = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;

    if ((private->location == NULL) ||
        (curl_url_set(private->location, CURLUPART_URL, location, 0) != CURLE_OK))
    {
      curl_url_cleanup(private->location);
      free(private);
      return NULL;
    }

    scheme = NULL;

    if ((curl_url_get(private->location, CURLUPART_SCHEME, &scheme, 0) == CURLE_OK) &&
        (scheme != NULL) &&
        (strcmp(scheme, "https") == 0))
    {
      //
      private->type = CURL_HTTP_VERSION_2TLS;
    }

    curl_free(scheme);

    curl_url_set(private->location, CURLUPART_USER,     NULL, 0);
    curl_url_set(private->location, CURLUPART_PASSWORD, NULL, 0);
    curl_url_set(private->location, CURLUPART_OPTIONS,  NULL, 0);
    curl_url_set(private->location, CURLUPART_QUERY,    NULL, 0);
    curl_url_set(private->location, CURLUPART_FRAGMENT, NULL, 0);

    private->headers = curl_slist_append(private->headers, GRPC_HEADER_TRAILERS);
    private->headers = curl_slist_append(private->headers, GRPC_HEADER_CONTENT_TYPE);
    private->headers = curl_slist_append(private->headers, GRPC_HEADER_ACCEPT_ENCODING);

    if ((timeout > 0) &&
        ((resolution == 'm') ||
         (resolution == 'S')))
    {
      snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, GRPC_HEADER_TIMEOUT, timeout, resolution);
      private->headers    = curl_slist_append(private->headers, buffer);
      private->timeout    = timeout;
      private->resolution = resolution;
    }

    if ((token    != NULL) &&
        (token[0] != '\0') &&
        (snprintf(buffer, GRPC_STRING_BUFFER_LENGTH, GRPC_HEADER_AUTHORIZATION, token) > 0))
    {
      //
      private->headers = curl_slist_append(private->headers, buffer);
    }
  }

  return (ProtobufCService*)private;
}
