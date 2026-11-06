#ifndef ION_SIMULATOR_KINDLE_DEBUG_H
#define ION_SIMULATOR_KINDLE_DEBUG_H

#include <stdint.h>

/* Diagnostics for the Kindle port. Everything goes to stderr, which the
 * launcher redirects to upsilon/upsilon.log.
 *
 * - log(): timestamped line ("[  12.345] ..."), seconds since start.
 * - Verbose mode (UPSILON_DEBUG=1): touch events, key states, e-ink updates
 *   and a status line every 10 s.
 * - Watchdog thread: the main loop polls the keyboard every ~10 ms when idle.
 *   If it stops doing so for 4 s, the watchdog logs what the main thread is
 *   doing: kernel state, wait channel, current system call, kernel stack and
 *   a symbolized user-space backtrace. It repeats while the stall lasts.
 * - While stalled, holding a finger anywhere on the screen for 3 s exits
 *   Upsilon (the launcher then brings the Kindle UI back).
 * - kill -QUIT <pid> writes the same report on demand.
 * - Crashes (SIGSEGV, SIGABRT...) are logged with a backtrace. */

namespace Ion {
namespace Kindle {
namespace Debug {

void init();
bool verbose();

void log(const char * format, ...) __attribute__((format(printf, 1, 2)));

/* Current startup/shutdown step, reported by the watchdog. Must be a string
 * literal (kept as a pointer). */
void setPhase(const char * phase);

/* Called on every keyboard scan: proof that the main loop is alive. */
void heartbeat();

void startWatchdog();

/* Touchscreen used for the rescue gesture */
void setTouchFd(int fd);

/* Counters reported in the verbose status line */
struct Counters {
  uint32_t scans;
  uint32_t touchEvents;
  uint32_t taps;
  uint32_t einkUpdates;
  uint32_t einkFailures;
  uint32_t einkMaxMs;
};
Counters & counters();

}
}
}

#define KINDLE_VLOG(...) \
  do { \
    if (Ion::Kindle::Debug::verbose()) { \
      Ion::Kindle::Debug::log(__VA_ARGS__); \
    } \
  } while (0)

#endif
