#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <stdatomic.h>

#include "SambarAdapter.h"
#include "Sombrero.h"

#define DEFAULT_PORT  4450

struct ServerState
{
  struct FastRing* ring;
  struct SambarListener* listener;
  struct SombreroCore sombrero;
};

static atomic_int state = 0;

static void HandleSignal(int signal)
{
  atomic_fetch_add_explicit(&state, 1, memory_order_relaxed);
}

static void HandleCrash(int signal)
{
  void* frames[64];
  int count;

  count = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
  fprintf(stderr, "Crash signal %d\n", signal);
  backtrace_symbols_fd(frames, count, fileno(stderr));
  _Exit(128 + signal);
}

static void HandleAccept(struct SombreroCore* core, struct smb2_context* context, void* closure)
{
  smb2_set_version(context, SMB2_VERSION_ANY);

  printf("Accepted named-pipe client context %p on sombrero %p closure=%p\n", (void*)context, (void*)core, closure);
}

int main(int argc, char** argv)
{
  struct sigaction action;
  struct ServerState server;
  uint16_t port;

  memset(&server, 0, sizeof(struct ServerState));
  memset(&action, 0, sizeof(struct sigaction));

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  action.sa_handler = HandleCrash;
  action.sa_flags   = SA_NODEFER | SA_RESETHAND;

  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGABRT, &action, NULL);

  port        = argc > 1 ? (uint16_t)strtoul(argv[1], NULL, 0) : DEFAULT_PORT;
  server.ring = CreateFastRing(0);

  if (server.ring == NULL)
  {
    fprintf(stderr, "CreateFastRing() failed\n");
    return 1;
  }

  server.sombrero.ring                   = server.ring;
  server.sombrero.server.port            = port;
  server.sombrero.server.signing_enabled = 0;
  server.sombrero.server.allow_anonymous = 1;

  PrepareSombreroCore(&server.sombrero, HandleAccept, &server);

  server.listener = OpenSambarListener(server.ring, port, HandleSombreroAccept, &server.sombrero);

  if (server.listener == NULL)
  {
    fprintf(stderr, "OpenSambarListener() failed\n");
    ReleaseFastRing(server.ring);
    return 1;
  }

  printf("Started IPC$ named-pipe echo server on port %u\n", port);

  while ((atomic_load_explicit(&state, memory_order_relaxed) < 1) &&
         (WaitForFastRing(server.ring, 200, NULL) >= 0));

  CloseSambarListener(server.listener);
  ReleaseFastRing(server.ring);

  printf("Stopped\n");
  return 0;
}
