#ifndef CURLWSCORE_H
#define CURLWSCORE_H

#include "FastRing.h"
#include "Fetch.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CWS_REASON_CLOSED     0
#define CWS_REASON_CONNECTED  1
#define CWS_REASON_RECEIVED   2

#define CWS_STATE_CONNECTING  0
#define CWS_STATE_CONNECTED   1
#define CWS_STATE_REJECTED    2

struct CWSMessage;
struct CWSTransmission;

typedef int (*HandleCWSEventFunction)(void* closure, struct CWSTransmission* transmission, int reason, int parameter, char* data, size_t length);

struct CWSMessage
{
  struct CWSTransmission* transmission;  //
  struct CWSMessage* next;               //
  size_t size;                           // Size of allocation

  int type;                              // WebSocket frame type (CURLWS_TEXT, CURLWS_BINARY, CURLWS_CONT, CURLWS_PING, CURLWS_PONG)
  char* data;                            // Pointer to data (will be set to buffer by default, use NULL to close connection)
  size_t length;                         // Length of data

  char buffer[0];                        //
};

struct CWSQueue
{
  struct CWSMessage* head;
  struct CWSMessage* tail;
};

struct CWSTransmission
{
  struct FetchTransmission super;

  HandleCWSEventFunction function;
  void* closure;

  struct FastRingDescriptor* descriptor;
  struct FastRingFlusher* flusher;
  struct CWSMessage* current[2];
  struct CWSMessage* heap;
  struct CWSQueue inbound;
  struct CWSQueue outbound;
  int state;
};

struct CWSTransmission* MakeExtendedCWSTransmission(struct Fetch* fetch, CURL* easy, HandleCWSEventFunction function, void* closure);
struct CWSTransmission* MakeSimpleCWSTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, HandleCWSEventFunction function, void* closure);
void CloseCWSTransmission(struct CWSTransmission* transmission);

struct CWSMessage* AllocateCWSMessage(struct CWSTransmission* transmission, size_t length, int type);
void TransmitCWSMessage(struct CWSMessage* message);

#ifdef __cplusplus
}
#endif

#endif