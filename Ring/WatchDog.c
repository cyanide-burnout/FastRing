#include "WatchDog.h"

#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>

#ifdef USE_LIGHTNOTIFIER
#include "LightNotifier.h"
#endif

#define STATE_INITIALIZING  0
#define STATE_RUNNING       1

#define NOTIFICATION_RATIO  (4ULL * 1000ULL)  // 4 times in allowed period, also convert microseconds to milliseconds
#define BUFFER_LENGTH       512

static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct WatchDog* state;

  static const char* messages[] =
  {
    "READY=1\n"
    "WATCHDOG=1\n",
    "WATCHDOG=1\n"
  };

  state = (struct WatchDog*)descriptor->closure;
  sd_notify(0, messages[state->state]);
  state->state = STATE_RUNNING;
}

struct WatchDog* CreateWatchDog(struct FastRing* ring)
{
  uint64_t interval;
  struct WatchDog* state;
  char buffer[BUFFER_LENGTH];

  if (sd_watchdog_enabled(0, &interval) <= 0)
  {
    // Watchdog is disabled or an error occurred
    return NULL;
  }

  sprintf(buffer, "MAINPID=%u\n", getpid());
  sd_notify(0, buffer);

  interval /= NOTIFICATION_RATIO;
  state     = (struct WatchDog*)calloc(1, sizeof(struct WatchDog));

  state->ring       = ring;
  state->state      = STATE_INITIALIZING;
  state->interval   = interval;
  state->descriptor = SetFastRingTimeout(state->ring, NULL, state->interval, TIMEOUT_FLAG_REPEAT, HandleTimeout, state);

  return state;
}

void ReleaseWatchDog(struct WatchDog* state)
{
  if (state != NULL)
  {
    SetFastRingTimeout(state->ring, state->descriptor, -1, 0, NULL, NULL);
    free(state);
  }
}
