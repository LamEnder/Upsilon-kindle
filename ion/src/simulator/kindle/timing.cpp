#include <ion/timing.h>
#include <time.h>
#include <errno.h>

static uint64_t monotonicMillis() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const uint64_t sStart = monotonicMillis();

namespace Ion {
namespace Timing {

uint64_t millis() {
  return monotonicMillis() - sStart;
}

void usleep(uint32_t us) {
  struct timespec ts;
  ts.tv_sec = us / 1000000;
  ts.tv_nsec = (long)(us % 1000000) * 1000;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

void msleep(uint32_t ms) {
  usleep(ms * 1000);
}

}
}
