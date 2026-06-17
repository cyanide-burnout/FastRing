#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>

#include "SambarPipe.h"

struct ClientState
{
  struct FastRing* ring;
  struct SambarPipe* pipe;
  int done;
  int result;
};

static void HandleCrash(int signal)
{
  void* frames[64];
  int count;

  count = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
  fprintf(stderr, "Crash signal %d\n", signal);
  backtrace_symbols_fd(frames, count, fileno(stderr));
  _Exit(128 + signal);
}

static void HandlePipe(void* closure, int event, int status, void* data, uint32_t length)
{
  static const char payload[] = "ping";
  struct ClientState* state;

  state = (struct ClientState*)closure;

  switch (event)
  {
    case SAMBAR_PIPE_CONNECT:
      if (WriteSambarPipe(state->pipe, payload, sizeof(payload) - 1U) < 0)
      {
        fprintf(stderr, "WriteSambarPipe() failed\n");
        state->result = 1;
        CloseSambarPipe(state->pipe);
      }
      return;

    case SAMBAR_PIPE_READ:
      if ((length != sizeof(payload) - 1U) ||
          (memcmp(data, payload, sizeof(payload) - 1U) != 0))
      {
        fprintf(stderr, "unexpected payload: length=%u\n", length);
        state->result = 1;
      }

      CloseSambarPipe(state->pipe);
      return;

    case SAMBAR_PIPE_WRITE:
      if (status < 0)
      {
        fprintf(stderr, "pipe write failed: %d\n", status);
        state->result = 1;
      }
      return;

    case SAMBAR_PIPE_CLOSE:
      state->done = 1;
      return;

    case SAMBAR_PIPE_ERROR:
      fprintf(stderr, "pipe error: %d\n", status);
      state->done   = 1;
      state->result = 1;
      return;
  }
}

int main(int argc, char** argv)
{
  char address[256];
  struct sigaction action;
  struct ClientState state;

  memset(&action, 0, sizeof(struct sigaction));

  action.sa_handler = HandleCrash;
  action.sa_flags   = SA_NODEFER | SA_RESETHAND;

  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGABRT, &action, NULL);


  if (argc != 3)
  {
    fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
    return 2;
  }

  memset(&state, 0, sizeof(struct ClientState));

  state.ring = CreateFastRing(0);

  if (state.ring == NULL)
  {
    fprintf(stderr, "CreateFastRing() failed\n");
    return 1;
  }

  if (snprintf(address, sizeof(address), "%s:%s", argv[1], argv[2]) >= (int)sizeof(address))
  {
    fprintf(stderr, "address too long\n");
    ReleaseFastRing(state.ring);
    return 1;
  }

  state.pipe = OpenSambarPipe(state.ring, address, "guest", "", "echo", HandlePipe, &state);

  if (state.pipe == NULL)
  {
    fprintf(stderr, "OpenSambarPipe() failed\n");
    ReleaseFastRing(state.ring);
    return 1;
  }

  while (!state.done && (WaitForFastRing(state.ring, 200, NULL) >= 0));

  ReleaseFastRing(state.ring);
  return state.result;
}
