#ifndef FASTGLOOP_H
#define FASTGLOOP_H

#include <glib.h>
#include <ucontext.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Set report proxy:                         g_log_set_default_handler(HandleGLogReport, (gpointer)report);
// Set adapter as a thread-default context:  g_main_context_push_thread_default(adapter->context);
// Remove thread-default context:            g_main_context_pop_thread_default(adapter->context);

#ifdef FASTGLOOP_INTERNAL
struct FastGLoopPoolData
{
  int result;                             // Result value
  uint32_t cycle;                         // Current cycle number
  uint32_t current;                       // Actual requested flags
  uint32_t previous;                      // Last requested flags
  struct FastRingDescriptor* descriptor;  // Pending descriptor

};
#endif

struct FastGLoop
{
  GMainLoop* loop;                        //
  GMainContext* context;                  //
  struct FastRing* ring;                  //

#ifdef FASTGLOOP_INTERNAL
  ucontext_t fibers[2];                   // Fibers (green threads) for FastRing (FIBER_MAIN) and GMainLoop (FIBER_LOOP)

  int condition;                          // Condition of FastRing's HaundleFlushFunction (installed, need to switch to GMainLoop)
  uint32_t cycle;                         // Current cycle number
  uint32_t length;                        // Files array length in elements
  struct FastGLoopPoolData* files;        // Files to track
  struct FastRingDescriptor* descriptor;  // Timeout escriptor

  struct __kernel_timespec timeout;       // |
  GPollFD* entries;                       // |- Arguments passed to GPollFunc
  guint count;                            // |
  gint result;                            // Return value of GPollFunc
  int error;                              // Return value of errno
#endif
};

struct FastGLoop* CreateFastGLoop(struct FastRing* ring, int interval);
void ReleaseFastGLoop(struct FastGLoop* loop);
void StopFastGLoop(struct FastGLoop* loop);
int IsInFastGLoop();

void HandleGLogReport(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data);

#ifdef __cplusplus
}
#endif

#endif
