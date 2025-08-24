#include "CURLWSCore.h"

#include <malloc.h>
#include <string.h>

struct CWSQueue
{
  struct CWSMessage* head;
  struct CWSMessage* tail;
};

struct CWSContext
{
  struct FastRingDescriptor* descriptor;
  struct FastRingFlusher* flusher;
  struct CWSMessage* current;
  struct CWSMessage* heap;
  struct CWSQueue inbound;
  struct CWSQueue outbound;
  int state;
};

static void HandleFlushEvent(void* closure, int reason)
{
  struct FetchTransmission* transmission;
  HandleCWSEventFunction function;
  struct CWSContext* context;
  struct CWSMessage* message;

  if (reason == RING_REASON_COMPLETE)
  {
    transmission     = (struct FetchTransmission*)closure;
    context          = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
    function         = (HandleCWSEventFunction)GetFetchTransmissionParameter1(transmission);
    closure          = GetFetchTransmissionParameter2(transmission);
    context->flusher = NULL;

    while (message = context->inbound.head)
    {
      context->inbound.head = message->next;
      reason                = CWS_REASON_CONNECTED + !!message->type;

      if (function(closure, transmission, reason, message->type, message->buffer, message->length) < 0)
      {
        CancelFetchTransmission(transmission);
        free(message);
        return;
      }

      message->next = context->heap;
      context->heap = message;
    }
  }
}

static void SubmitInboundMessage(struct CWSMessage* message)
{
  struct FetchTransmission* transmission;
  struct CWSContext* context;
  struct FastRing* ring;

  transmission = message->transmission;
  context      = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  ring         = GetFetchTransmissionRing(transmission);

  if (context->inbound.head != NULL)
  {
    context->inbound.tail->next = message;
    context->inbound.tail       = message;
  }
  else
  {
    context->inbound.head = message;
    context->inbound.tail = message;
  }

  if (context->flusher == NULL)
  {
    // HandleFlushEvent should be only set once per cycle
    context->flusher = SetFastRingFlushHandler(ring, HandleFlushEvent, transmission);
  }
}

static void HandleFetchEvent(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  struct FastRingDescriptor* descriptor;
  HandleCWSEventFunction function;
  struct CWSContext* context;
  struct CWSMessage* message;
  struct FastRing* ring;
  void* closure;
  int reason;

  context  = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  function = (HandleCWSEventFunction)GetFetchTransmissionParameter1(transmission);
  closure  = GetFetchTransmissionParameter2(transmission);
  ring     = GetFetchTransmissionRing(transmission);
  code    *= (code < 0) || (context->state == 0);

  while (message = context->inbound.head)
  {
    context->inbound.head = message->next;
    reason                = CWS_REASON_CONNECTED + !!message->type;

    if ((function != NULL) &&
        (function(closure, transmission, reason, message->type, message->buffer, message->length) < 0))
    {
      // Handler may reject reception
      function = NULL;
    }

    free(message);
  }

  if (function != NULL)
  {
    // Handler may reject reception
    function(closure, transmission, CWS_REASON_CLOSED, code, data, length);
  }

  while (message = context->outbound.head)
  {
    context->outbound.head = message->next;
    free(message);
  }

  while (message = context->heap)
  {
    context->heap = message->next;
    free(message);
  }

  if (descriptor = context->descriptor)
  {
    context->descriptor  = NULL;
    descriptor->closure  = NULL;
    descriptor->function = NULL;
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_prep_poll_remove(&descriptor->submission, descriptor->identifier);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }

  free(context->current);
  RemoveFastRingFlushHandler(ring, context->flusher);
}

static size_t HandleSocketHeader(void* buffer, size_t size, size_t count, void* data)
{
  struct FetchTransmission* transmission;
  struct CWSContext* context;
  struct CWSMessage* message;
  CURL* easy;
  long code;

  transmission  = (struct FetchTransmission*)data;
  context       = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  easy          = GetFetchTransmissionHandle(transmission);
  size         *= count;

  if ((size == 2) &&
      (strncmp((const char*)buffer, "\r\n", 2) == 0) &&
      (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) &&
      ((code == 101) ||
       (code == 200)))
  {
    message = AllocateCWSMessage(transmission, 0, 0);

    if (message == NULL)
    {
      // Cannot create notification
      return CURL_WRITEFUNC_ERROR;
    }

    context->state ++;
    SubmitInboundMessage(message);
  }

  return size;
}

static size_t HandleSocketWrite(void* buffer, size_t size, size_t count, void* data)
{
  struct FetchTransmission* transmission;
  const struct curl_ws_frame* frame;
  struct CWSContext* context;
  CURL* easy;

  transmission  = (struct FetchTransmission*)data;
  context       = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  easy          = GetFetchTransmissionHandle(transmission);
  frame         = curl_ws_meta(easy);
  size         *= count;

  if (frame != NULL)
  {
    if ((frame->offset    == 0)    &&
        (context->current != NULL) ||
        (frame->offset    != 0)    &&
        (context->current == NULL))
    {
      // Frame integrity failure
      return CURL_WRITEFUNC_ERROR;      
    }

    if ( (context->current == NULL) &&
        !(context->current  = AllocateCWSMessage(transmission, frame->len, frame->flags)))
    {
      // Message allocation failure
      return CURL_WRITEFUNC_ERROR;
    }

    memcpy(context->current->buffer + frame->offset, buffer, size);

    if (frame->bytesleft == 0)
    {
      SubmitInboundMessage(context->current);
      context->current = NULL;
    }
  }

  return size;
}

#if (LIBCURL_VERSION_NUM >= 0x081000)

static size_t HandleSocketRead(void* buffer, size_t size, size_t count, void* data)
{
  struct FetchTransmission* transmission;
  struct CWSContext* context;
  struct CWSMessage* message;
  CURL* easy;

  transmission  = (struct FetchTransmission*)data;
  context       = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  easy          = GetFetchTransmissionHandle(transmission);
  message       = context->outbound.head;
  size         *= count;

  if (message != NULL)
  {
    if ((message->data == NULL) ||
        (message->data == message->buffer) &&
        (curl_ws_start_frame(easy, message->type, message->length) != CURLE_OK))
    {
      // Frame composition failure
      return CURL_READFUNC_ABORT;
    }

    if (message->length > size)
    {
      memcpy(buffer, message->data, size);
      message->data   += size;
      message->length -= size;
      return size;
    }

    memcpy(buffer, message->data, message->length);
    context->outbound.head = message->next;
    message->next          = context->heap;
    context->heap          = message;
    return message->length;
  }

  return CURL_READFUNC_PAUSE;
}

#else

static int HandleSocketCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FetchTransmission* transmission;
  struct CWSContext* context;
  struct CWSMessage* message;
  CURLcode result;
  size_t count;
  CURL* easy;

  if (reason == RING_REASON_COMPLETE)
  {
    transmission = (struct FetchTransmission*)descriptor->closure;
    context      = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
    easy         = GetFetchTransmissionHandle(transmission);
    message      = NULL;

    while ((context->state > 0) &&
           (message = context->outbound.head) &&
           (message->data != NULL))
    {
      count  = message->length * (message->data == message->buffer);
      result = curl_ws_send(easy, message->data, message->length, &count, count, message->type | CURLWS_OFFSET);

      if ((result == CURLE_AGAIN) ||
          (result == CURLE_OK) &&
          (count == 0))
      {
        // Socket is not ready
        break;
      }

      if ((result == CURLE_OK) &&
          (count != 0))
      {
        message->type    = 0;
        message->data   += count;
        message->length -= count;
      }

      if ((result != CURLE_OK) ||
          (message->length == 0))
      {
        context->outbound.head = message->next;
        message->next          = context->heap;
        context->heap          = message;
      }
    }

    if ((context->outbound.head != NULL) &&
        ((message       == NULL) ||
         (message->data != NULL)))
    {
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    context->descriptor = NULL;

    if ((message       != NULL) &&
        (message->data == NULL))
    {
      //
      CancelFetchTransmission(transmission);
    }
  }

  return 0;
}

#endif

struct FetchTransmission* MakeExtendedCWSTransmission(struct Fetch* fetch, CURL* easy, HandleCWSEventFunction function, void* closure)
{
#if (LIBCURL_VERSION_NUM >= 0x081000)
  curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(easy, CURLOPT_READFUNCTION, HandleSocketRead);
#endif

  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, HandleSocketWrite);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, HandleSocketHeader);

  return MakeExtendedFetchTransmission(fetch, easy, FETCH_OPTION_SET_HANDLER_DATA, HandleFetchEvent, function, closure);
}

struct FetchTransmission* MakeSimpleCWSTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, HandleCWSEventFunction function, void* closure)
{
  CURL* easy;

  easy = curl_easy_init();

  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(easy, CURLOPT_URL, location);

  if (headers != NULL)
  {
    // Headers could be overwritten by application
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  if (token != NULL)
  {
    curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
    curl_easy_setopt(easy, CURLOPT_XOAUTH2_BEARER, token);
  }

  return MakeExtendedCWSTransmission(fetch, easy, function, closure);
}

struct CWSMessage* AllocateCWSMessage(struct FetchTransmission* transmission, size_t length, int type)
{
  struct CWSContext* context;
  struct CWSMessage* message;
  size_t size;

  context = (struct CWSContext*)GetFetchTransmissionStorage(transmission);
  size    = sizeof(struct CWSMessage) + length + 1;

  if (context->heap != NULL)
  {
    message       = context->heap;
    context->heap = message->next;

    if (size <= message->size)
    {
      message->next   = NULL;
      message->type   = type;
      message->data   = message->buffer;
      message->length = length;
      return message;
    }

    free(message);
  }

  if (message = (struct CWSMessage*)malloc(size))
  {
    message->transmission = transmission;
    message->size         = size;
    message->next         = NULL;
    message->type         = type;
    message->data         = message->buffer;
    message->length       = length;
  }

  return message;
}

void TransmitCWSMessage(struct CWSMessage* message)
{
  struct FastRingDescriptor* descriptor;
  struct CWSContext* context;
  struct FastRing* ring;
  curl_socket_t handle;
  CURL* easy;

  context = (struct CWSContext*)GetFetchTransmissionStorage(message->transmission);
  easy    = GetFetchTransmissionHandle(message->transmission);
  ring    = GetFetchTransmissionRing(message->transmission);

  if (context->outbound.head != NULL)
  {
    context->outbound.tail->next = message;
    context->outbound.tail       = message;
  }
  else
  {
    context->outbound.head = message;
    context->outbound.tail = message;
  }

#if (LIBCURL_VERSION_NUM >= 0x081000)
  
  if (context->outbound.head == context->outbound.tail)
  {
    curl_easy_pause(easy, CURLPAUSE_SEND_CONT);
    TouchFetchTransmission(message->transmission);
  }

#else

  if ((context->descriptor == NULL) &&
      (curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &handle) == CURLE_OK) &&
      (descriptor = AllocateFastRingDescriptor(ring, HandleSocketCompletion, message->transmission)))
  {
    context->descriptor = descriptor;
    io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN | POLLOUT | POLLHUP | POLLERR);
    SubmitFastRingDescriptor(descriptor, 0);
  }

#endif
}
