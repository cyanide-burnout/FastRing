#define _GNU_SOURCE

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "Fetch.h"

#define STATE_RUNNING  -1

// Requests the loop below waits for; the stalled one is not counted, it is
// reaped by ReleaseFetch()
#define REQUEST_COUNT  4

static atomic_int state = { STATE_RUNNING };
static int count        = 0;

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 0, memory_order_relaxed);
}

static void HandleResponseEvent(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2)
{
  const char* name;
  char* type;
  curl_off_t time;
  size_t position;

  name = (const char*)parameter1;

  if (parameter2 != NULL)
  {
    // The transmission is released as soon as this handler returns, so an
    // application must drop every pointer it holds to it
    *(struct FetchTransmission**)parameter2 = NULL;
  }

  switch (code)
  {
    case FETCH_STATUS_CANCELLED:
      // CancelFetchTransmission() has been called, there is no response
      printf("%-10s cancelled\n", name);
      break;

    case FETCH_STATUS_INCOMPLETE:
      // ReleaseFetch() has been called while the transfer was still running
      printf("%-10s incomplete\n", name);
      return;

    default:
      if (code < 0)
      {
        // Any other negative code is a negated CURLcode, and data holds the
        // text of curl_easy_strerror()
        printf("%-10s failed: %s (%d)\n", name, data, -code);
        break;
      }

      // A non-negative code is the HTTP response code, and data / length is
      // the body accumulated by FETCH_OPTION_HANDLE_CONTENT
      type = NULL;
      time = 0;

      curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type);
      curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME_T, &time);

      printf("%-10s HTTP %d, %s, %zu bytes in %.3f s\n", name, code, (type != NULL) ? type : "-", length, time / 1000000.);

      if ((data != NULL) &&
          (length > 0))
      {
        // The accumulated body is always terminated with a zero byte
        position = strcspn(data, "\r\n");
        position = (position < 64) ? position : 64;

        printf("%-10s payload: %.*s ...\n", name, (int)position, data);
      }
      break;
  }

  count ++;
}

int main()
{
  struct sigaction action;
  struct FastRing* ring;
  struct Fetch* fetch;
  struct FetchTransmission* transmission;
  struct curl_slist* headers;
  struct curl_slist* addresses;
  struct sockaddr_in address;
  CURLU* location;
  CURL* easy;
  socklen_t size;
  int handle;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  printf("Started\n");

  curl_global_init(CURL_GLOBAL_DEFAULT);

  ring  = CreateFastRing(0);
  fetch = CreateFetch(ring);

  // libcurl does not copy an URL handle nor a header list, so both have to
  // outlive every transmission referring to them
  headers   = AppendFetchList(NULL, 64, "Accept-Language: %s", "en-US");
  headers   = AppendFetchList(headers, 64, "X-Request-Number: %d", 1);
  addresses = NULL;
  location  = curl_url();

  curl_url_set(location, CURLUPART_URL, "https://www.google.com/search", 0);
  AppendFetchParameter(location, 64, "q=%s", "FastRing io_uring");

  // A plain GET: Fetch creates the easy handle, follows redirects and
  // accumulates the response body
  MakeSimpleFetchTransmission(fetch, "https://www.google.com/", NULL, NULL, NULL, 0, HandleResponseEvent, "simple", NULL);

  // An application-owned easy handle, so every libcurl option is available
  if (easy = curl_easy_init())
  {
    curl_easy_setopt(easy, CURLOPT_CURLU,             location);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER,        headers);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,         "FastRing/Fetch");
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING,   "");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,    1);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,        4000L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 2000L);

    MakeExtendedFetchTransmission(fetch, NULL, easy, FETCH_OPTION_HANDLE_CONTENT, HandleResponseEvent, "extended", NULL);
  }

  // A transport failure is reported as a negated CURLcode
  MakeSimpleFetchTransmission(fetch, "https://www.google.invalid/", NULL, NULL, NULL, 0, HandleResponseEvent, "failing", NULL);

  // CancelFetchTransmission() calls the handler in place, before it returns
  transmission = MakeSimpleFetchTransmission(fetch, "https://www.google.com/robots.txt", NULL, NULL, NULL, 0, HandleResponseEvent, "cancelled", &transmission);
  CancelFetchTransmission(transmission);

  // A transfer that never completes: a socket nobody accepts from finishes the
  // TCP handshake in the backlog and then stays silent forever, so the TLS
  // handshake hangs and ReleaseFetch() below has to reap the transmission
  memset(&address, 0, sizeof(struct sockaddr_in));

  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  size   = sizeof(struct sockaddr_in);
  handle = socket(AF_INET, SOCK_STREAM, 0);

  bind(handle, (const struct sockaddr*)&address, size);
  listen(handle, 1);
  getsockname(handle, (struct sockaddr*)&address, &size);

  if ((addresses = MakeFetchConnectAddress((const struct sockaddr*)&address)) &&
      (easy      = curl_easy_init()))
  {
    curl_easy_setopt(easy, CURLOPT_URL,        "https://www.google.com/");
    curl_easy_setopt(easy, CURLOPT_CONNECT_TO, addresses);

    MakeExtendedFetchTransmission(fetch, NULL, easy, FETCH_OPTION_HANDLE_CONTENT, HandleResponseEvent, "stalled", NULL);
  }

  while ((atomic_load_explicit(&state, memory_order_relaxed) == STATE_RUNNING) &&
         (count < REQUEST_COUNT) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  printf("Still running: %d\n", GetFetchTransmissionCount(fetch));

  ReleaseFetch(fetch);
  ReleaseFastRing(ring);

  close(handle);

  curl_slist_free_all(addresses);
  curl_slist_free_all(headers);
  curl_url_cleanup(location);
  curl_global_cleanup();

  printf("Stopped\n");

  return 0;
}
