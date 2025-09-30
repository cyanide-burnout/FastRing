#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "CURLWSCore.h"

#define STATE_RUNNING  -1

atomic_int state = { STATE_RUNNING };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 0, memory_order_relaxed);
}

void HandleSocketIOMessage(struct CWSTransmission* transmission, char* data)
{
  struct CWSMessage* message;

  if ((strcmp(data, "2") == 0) &&
      (message = AllocateCWSMessage(transmission, 64, CURLWS_TEXT)))
  {
    message->length = sprintf(message->buffer, "3");
    TransmitCWSMessage(message);
  }

  if ((strncmp(data, "0{", 2) == 0) &&
      (message = AllocateCWSMessage(transmission, 64, CURLWS_TEXT)))
  {
    message->length = sprintf(message->buffer, "40");
    TransmitCWSMessage(message);
  }

  if ((strncmp(data, "40{", 3) == 0) &&
      (message = AllocateCWSMessage(transmission, 64, CURLWS_TEXT)))
  {
    message->length = sprintf(message->buffer, "42[\"join\", \"everything\"]");
    TransmitCWSMessage(message);
  }
}

int HandleEvent(void* closure, struct CWSTransmission* transmission, int reason, int parameter, char* data, size_t length)
{
  static int count = 0;

  switch (reason)
  {
    case CWS_REASON_CONNECTED:
      printf("Connected\n");
      break;

    case CWS_REASON_RECEIVED:
      data[length] = '\0';
      printf("Received %d: %s\n", parameter, data);
      HandleSocketIOMessage(transmission, data);
      break;

    case CWS_REASON_CLOSED:
      *(void**)closure = NULL;
      printf("Disconnected (%d)\n", parameter);
      atomic_store_explicit(&state, 0, memory_order_relaxed);
      break;
  }

  if (++ count > 200)
  {
    printf("Rejected\n");
    *(void**)closure = NULL;
    atomic_store_explicit(&state, 0, memory_order_relaxed);
    return -1;
  }

  return 0;
}

int main()
{
  struct sigaction action;
  struct FastRing* ring;
  struct Fetch* fetch;
  struct CWSTransmission* transmission;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  printf("Started\n");

  ring         = CreateFastRing(0);
  fetch        = CreateFetch(ring);
  transmission = MakeSimpleCWSTransmission(fetch, "wss://api.brandmeister.network/lh/?EIO=4&transport=websocket", NULL, NULL, HandleEvent, &transmission);

  while ((atomic_load_explicit(&state, memory_order_relaxed) == STATE_RUNNING) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  CloseCWSTransmission(transmission);
  ReleaseFetch(fetch);
  ReleaseFastRing(ring);

  printf("Stopped\n");

  return 0;
}
