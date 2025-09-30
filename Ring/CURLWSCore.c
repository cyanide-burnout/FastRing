#include "CURLWSCore.h"

#include <malloc.h>
#include <string.h>

static void HandleFlushEvent(void* closure, int reason)
{
  struct CWSTransmission* transmission;
  struct CWSMessage* message;

  if (reason == RING_REASON_COMPLETE)
  {
    transmission          = (struct CWSTransmission*)closure;
    transmission->flusher = NULL;

    while (message = transmission->inbound.head)
    {
      transmission->inbound.head = message->next;
      reason                = CWS_REASON_CONNECTED + !!message->type;

      if (transmission->function(transmission->closure, transmission, reason, message->type, message->buffer, message->length) < 0)
      {
        transmission->state = CWS_STATE_REJECTED;
        CancelFetchTransmission(&transmission->super);
        free(message);
        return;
      }

      message->next      = transmission->heap;
      transmission->heap = message;
    }
  }
}

static void SubmitInboundMessage(struct CWSMessage* message)
{
  struct CWSTransmission* transmission;

  transmission = message->transmission;

  if (transmission->inbound.head != NULL)
  {
    transmission->inbound.tail->next = message;
    transmission->inbound.tail       = message;
  }
  else
  {
    transmission->inbound.head = message;
    transmission->inbound.tail = message;
  }

  if (transmission->flusher == NULL)
  {
    // HandleFlushEvent should be only set once per cycle
    transmission->flusher = SetFastRingFlushHandler(transmission->super.fetch->ring, HandleFlushEvent, transmission);
  }
}

static void HandleFetchEvent(struct FetchTransmission* super, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  struct FastRingDescriptor* descriptor;
  struct CWSTransmission* transmission;
  struct CWSMessage* message;
  int reason;

  transmission  = (struct CWSTransmission*)super;
  code         *= (code < 0) || (transmission->state == CWS_STATE_CONNECTING);

  while (message = transmission->inbound.head)
  {
    transmission->inbound.head = message->next;
    reason                     = CWS_REASON_CONNECTED + !!message->type;

    if ((transmission->state == CWS_STATE_CONNECTED) &&
        (transmission->function(transmission->closure, transmission, reason, message->type, message->buffer, message->length) < 0))
    {
      // Handler may reject reception
      transmission->state = CWS_STATE_REJECTED;
    }

    free(message);
  }

  if (transmission->state != CWS_STATE_REJECTED)
  {
    // Handler may reject reception
    transmission->function(transmission->closure, transmission, CWS_REASON_CLOSED, code, data, length);
  }

  while (message = transmission->outbound.head)
  {
    transmission->outbound.head = message->next;
    free(message);
  }

  while (message = transmission->heap)
  {
    transmission->heap = message->next;
    free(message);
  }

#if (LIBCURL_VERSION_NUM < 0x081000)
  if (descriptor = transmission->descriptor)
  {
    transmission->descriptor = NULL;
    descriptor->function     = NULL;
    descriptor->closure      = NULL;
    atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
    io_uring_initialize_sqe(&descriptor->submission);
    io_uring_prep_poll_remove(&descriptor->submission, descriptor->identifier);
    SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
  }
#endif

  free(transmission->current);
  RemoveFastRingFlushHandler(transmission->super.fetch->ring, transmission->flusher);
}

static size_t HandleSocketHeader(void* buffer, size_t size, size_t count, void* data)
{
  struct CWSTransmission* transmission;
  struct CWSMessage* message;
  long code;

  transmission  = (struct CWSTransmission*)data;
  size         *= count;

  if ((size == 2) &&
      (strncmp((const char*)buffer, "\r\n", 2) == 0) &&
      (curl_easy_getinfo(transmission->super.easy, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) &&
      ((code == 101) ||
       (code == 200)))
  {
    message = AllocateCWSMessage(transmission, 0, 0);

    if (message == NULL)
    {
      // Cannot create notification
      return CURL_WRITEFUNC_ERROR;
    }

    transmission->state = CWS_STATE_CONNECTED;
    SubmitInboundMessage(message);
  }

  return size;
}

static size_t HandleSocketWrite(void* buffer, size_t size, size_t count, void* data)
{
  struct CWSTransmission* transmission;
  const struct curl_ws_frame* frame;

  transmission  = (struct CWSTransmission*)data;
  frame         = curl_ws_meta(transmission->super.easy);
  size         *= count;

  if (frame != NULL)
  {
    if ((frame->offset         == 0)    &&
        (transmission->current != NULL) ||
        (frame->offset         != 0)    &&
        (transmission->current == NULL))
    {
      // Frame integrity failure
      return CURL_WRITEFUNC_ERROR;      
    }

    if ( (transmission->current == NULL) &&
        !(transmission->current  = AllocateCWSMessage(transmission, frame->len, frame->flags)))
    {
      // Message allocation failure
      return CURL_WRITEFUNC_ERROR;
    }

    memcpy(transmission->current->buffer + frame->offset, buffer, size);

    if (frame->bytesleft == 0)
    {
      SubmitInboundMessage(transmission->current);
      transmission->current = NULL;
    }
  }

  return size;
}

#if (LIBCURL_VERSION_NUM >= 0x081000)

static size_t HandleSocketRead(void* buffer, size_t size, size_t count, void* data)
{
  struct CWSTransmission* transmission;
  struct CWSMessage* message;

  transmission  = (struct CWSTransmission*)data;
  message       = transmission->outbound.head;
  size         *= count;

  if (message != NULL)
  {
    if ((message->data == NULL) ||
        (message->data == message->buffer) &&
        (curl_ws_start_frame(transmission->super.easy, message->type, message->length) != CURLE_OK))
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
    transmission->outbound.head = message->next;
    message->next               = transmission->heap;
    transmission->heap          = message;
    return message->length;
  }

  return CURL_READFUNC_PAUSE;
}

#else

static int HandleSocketCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct CWSTransmission* transmission;
  struct CWSMessage* message;
  CURLcode result;
  size_t count;

  if (reason == RING_REASON_COMPLETE)
  {
    transmission = (struct CWSTransmission*)descriptor->closure;
    message      = NULL;

    while ((transmission->state == CWS_STATE_CONNECTED) &&
           (message = transmission->outbound.head) &&
           (message->data != NULL))
    {
      count  = message->length * (message->data == message->buffer);
      result = curl_ws_send(transmission->super.easy, message->data, message->length, &count, count, message->type | CURLWS_OFFSET);

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
        transmission->outbound.head = message->next;
        message->next               = transmission->heap;
        transmission->heap          = message;
      }
    }

    if ((transmission->outbound.head != NULL) &&
        ((message       == NULL) ||
         (message->data != NULL)))
    {
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }

    transmission->descriptor = NULL;

    if ((message       != NULL) &&
        (message->data == NULL))
    {
      //
      CancelFetchTransmission(&transmission->super);
    }
  }

  return 0;
}

#endif

struct CWSTransmission* MakeExtendedCWSTransmission(struct Fetch* fetch, CURL* easy, HandleCWSEventFunction function, void* closure)
{
  struct CWSTransmission* transmission;

  if (transmission = (struct CWSTransmission*)calloc(1, sizeof(struct CWSTransmission)))
  {
    transmission->function = function;
    transmission->closure  = closure;

#if (LIBCURL_VERSION_NUM >= 0x081000)
    curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(easy, CURLOPT_READFUNCTION, HandleSocketRead);
#endif

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, HandleSocketWrite);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, HandleSocketHeader);

    return (struct CWSTransmission*)MakeExtendedFetchTransmission(fetch, &transmission->super, easy, FETCH_OPTION_SET_HANDLER_DATA, HandleFetchEvent, NULL, NULL);
  }

  curl_easy_cleanup(easy);
  return NULL;
}

struct CWSTransmission* MakeSimpleCWSTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, HandleCWSEventFunction function, void* closure)
{
  CURL* easy;

  if (easy = curl_easy_init())
  {
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

  return NULL;
}

void CloseCWSTransmission(struct CWSTransmission* transmission)
{
  if (transmission != NULL)
  {
    // Resources will be released by HandleFetchEvent()
    CancelFetchTransmission(&transmission->super);
  }
}

struct CWSMessage* AllocateCWSMessage(struct CWSTransmission* transmission, size_t length, int type)
{
  struct CWSMessage* message;
  size_t size;

  if (transmission == NULL)
  {
    //
    return NULL;
  }

  size = sizeof(struct CWSMessage) + length + 1;

  if (transmission->heap != NULL)
  {
    message            = transmission->heap;
    transmission->heap = message->next;

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
  struct CWSTransmission* transmission;
  curl_socket_t handle;

  transmission = message->transmission;

  if (transmission->outbound.head != NULL)
  {
    transmission->outbound.tail->next = message;
    transmission->outbound.tail       = message;
  }
  else
  {
    transmission->outbound.head = message;
    transmission->outbound.tail = message;
  }

#if (LIBCURL_VERSION_NUM >= 0x081000)
  
  if (transmission->outbound.head == transmission->outbound.tail)
  {
    curl_easy_pause(transmission->super.easy, CURLPAUSE_SEND_CONT);
    TouchFetchTransmission(&transmission->super);
  }

#else

  if ((transmission->descriptor == NULL) &&
      (curl_easy_getinfo(transmission->super.easy, CURLINFO_ACTIVESOCKET, &handle) == CURLE_OK) &&
      (descriptor = AllocateFastRingDescriptor(transmission->super.fetch->ring, HandleSocketCompletion, message->transmission)))
  {
    transmission->descriptor = descriptor;
    io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN | POLLOUT | POLLHUP | POLLERR);
    SubmitFastRingDescriptor(descriptor, 0);
  }

#endif
}
