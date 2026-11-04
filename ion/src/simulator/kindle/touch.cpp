#include "touch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr int k_maxQueue = 64;
constexpr int k_maxSlots = 16;

int sFd = -1;
bool sGrabbed = false;
int sScreenW = 0;
int sScreenH = 0;

bool sSwapXY = false;
bool sMirrorX = false;
bool sMirrorY = false;

input_absinfo sAbsX;
input_absinfo sAbsY;
bool sHasMT = false;

// Multi-touch slot state (protocol B)
struct Slot {
  int trackingId;
  int x;
  int y;
};
Slot sSlots[k_maxSlots];
int sCurrentSlot = 0;
int sPrimarySlot = -1;   // Slot we are following, -1 when no finger is down
bool sProtocolASeen = false;

// Single-touch state (also used for protocol A panels)
bool sBtnTouch = false;
int sSingleX = 0;
int sSingleY = 0;

// Last emitted state
bool sDown = false;
int sLastX = 0;
int sLastY = 0;


Ion::Kindle::Touch::Event sQueue[k_maxQueue];
int sQueueHead = 0;
int sQueueCount = 0;

void push(Ion::Kindle::Touch::Event e) {
  if (sQueueCount == k_maxQueue) {
    // Drop the oldest move to make room
    sQueueHead = (sQueueHead + 1) % k_maxQueue;
    sQueueCount--;
  }
  sQueue[(sQueueHead + sQueueCount) % k_maxQueue] = e;
  sQueueCount++;
}

bool envFlag(const char * name, bool defaultValue = false) {
  const char * v = getenv(name);
  if (v == nullptr || *v == 0) {
    return defaultValue;
  }
  return strcmp(v, "0") != 0 && strcmp(v, "no") != 0 && strcmp(v, "false") != 0;
}

bool testBit(const unsigned long * bits, int bit) {
  constexpr int bitsPerLong = 8 * sizeof(unsigned long);
  return (bits[bit / bitsPerLong] >> (bit % bitsPerLong)) & 1;
}

bool probe(const char * path) {
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  constexpr int bitsPerLong = 8 * sizeof(unsigned long);
  unsigned long absBits[(ABS_MAX + bitsPerLong) / bitsPerLong];
  unsigned long evBits[(EV_MAX + bitsPerLong) / bitsPerLong];
  memset(absBits, 0, sizeof(absBits));
  memset(evBits, 0, sizeof(evBits));
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0 || !testBit(evBits, EV_ABS)
      || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
    close(fd);
    return false;
  }
  bool mt = testBit(absBits, ABS_MT_POSITION_X) && testBit(absBits, ABS_MT_POSITION_Y);
  bool st = testBit(absBits, ABS_X) && testBit(absBits, ABS_Y);
  if (!mt && !st) {
    close(fd);
    return false;
  }
  input_absinfo ax, ay;
  int codeX = mt ? ABS_MT_POSITION_X : ABS_X;
  int codeY = mt ? ABS_MT_POSITION_Y : ABS_Y;
  if (ioctl(fd, EVIOCGABS(codeX), &ax) < 0 || ioctl(fd, EVIOCGABS(codeY), &ay) < 0
      || ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) {
    close(fd);
    return false;
  }
  char name[128] = "?";
  ioctl(fd, EVIOCGNAME(sizeof(name)), name);
  fprintf(stderr, "upsilon: touch device %s \"%s\" (%s), x %d..%d, y %d..%d\n",
      path, name, mt ? "multi-touch" : "single-touch", ax.minimum, ax.maximum, ay.minimum, ay.maximum);
  sFd = fd;
  sHasMT = mt;
  sAbsX = ax;
  sAbsY = ay;
  return true;
}

void mapPoint(int rawX, int rawY, int * outX, int * outY) {
  input_absinfo ax = sAbsX, ay = sAbsY;
  if (sSwapXY) {
    int t = rawX; rawX = rawY; rawY = t;
    input_absinfo ti = ax; ax = ay; ay = ti;
  }
  long spanX = (long)ax.maximum - ax.minimum + 1;
  long spanY = (long)ay.maximum - ay.minimum + 1;
  long x = ((long)(rawX - ax.minimum) * sScreenW) / spanX;
  long y = ((long)(rawY - ay.minimum) * sScreenH) / spanY;
  if (sMirrorX) { x = sScreenW - 1 - x; }
  if (sMirrorY) { y = sScreenH - 1 - y; }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= sScreenW) x = sScreenW - 1;
  if (y >= sScreenH) y = sScreenH - 1;
  *outX = (int)x;
  *outY = (int)y;
}

void emit(bool down, int rawX, int rawY) {
  using Ion::Kindle::Touch::Event;
  if (!down) {
    if (sDown) {
      sDown = false;
      push(Event{Event::Type::Up, sLastX, sLastY});
    }
    return;
  }
  int x, y;
  mapPoint(rawX, rawY, &x, &y);
  if (!sDown) {
    sDown = true;
    sLastX = x;
    sLastY = y;
    push(Event{Event::Type::Down, x, y});
  } else if (x != sLastX || y != sLastY) {
    sLastX = x;
    sLastY = y;
    push(Event{Event::Type::Move, x, y});
  }
}

void onSynReport() {
  if (sHasMT && !sProtocolASeen) {
    // Protocol B: follow the first finger that went down
    if (sPrimarySlot >= 0 && sSlots[sPrimarySlot].trackingId < 0) {
      sPrimarySlot = -1;
    }
    if (sPrimarySlot < 0 && !sDown) {
      for (int i = 0; i < k_maxSlots; i++) {
        if (sSlots[i].trackingId >= 0) {
          sPrimarySlot = i;
          break;
        }
      }
    }
    if (sPrimarySlot >= 0) {
      emit(true, sSlots[sPrimarySlot].x, sSlots[sPrimarySlot].y);
    } else {
      // Some panels never send a tracking id but do send BTN_TOUCH
      emit(sBtnTouch && sSlots[0].trackingId == -2, sSlots[0].x, sSlots[0].y);
    }
    return;
  }
  emit(sBtnTouch, sSingleX, sSingleY);
}

void handle(const input_event & ev) {
  switch (ev.type) {
    case EV_ABS:
      switch (ev.code) {
        case ABS_MT_SLOT:
          sCurrentSlot = (ev.value >= 0 && ev.value < k_maxSlots) ? ev.value : 0;
          break;
        case ABS_MT_TRACKING_ID:
          sSlots[sCurrentSlot].trackingId = ev.value < 0 ? -1 : ev.value;
          break;
        case ABS_MT_POSITION_X:
          sSlots[sCurrentSlot].x = ev.value;
          if (sCurrentSlot == 0) { sSingleX = ev.value; }
          break;
        case ABS_MT_POSITION_Y:
          sSlots[sCurrentSlot].y = ev.value;
          if (sCurrentSlot == 0) { sSingleY = ev.value; }
          break;
        case ABS_X:
          if (!sHasMT) { sSingleX = ev.value; }
          break;
        case ABS_Y:
          if (!sHasMT) { sSingleY = ev.value; }
          break;
      }
      break;
    case EV_KEY:
      if (ev.code == BTN_TOUCH) {
        sBtnTouch = ev.value != 0;
        if (sHasMT && sBtnTouch && sSlots[0].trackingId == -1 && sPrimarySlot < 0) {
          // Panel without tracking ids: mark slot 0 as "touching"
          sSlots[0].trackingId = -2;
        } else if (!sBtnTouch && sSlots[0].trackingId == -2) {
          sSlots[0].trackingId = -1;
        }
      }
      break;
    case EV_SYN:
      if (ev.code == SYN_MT_REPORT) {
        // Protocol A: a finger report just ended. Treat as single touch.
        sProtocolASeen = true;
        sBtnTouch = true;
      } else if (ev.code == SYN_REPORT) {
        onSynReport();
        if (sProtocolASeen) {
          // In protocol A an empty report means all fingers lifted; we only
          // keep "down" while reports keep coming with MT data.
          sBtnTouch = false;
        }
      }
      break;
  }
}

void readDevice() {
  if (sFd < 0) {
    return;
  }
  input_event events[32];
  while (true) {
    ssize_t n = read(sFd, events, sizeof(events));
    if (n <= 0) {
      break;
    }
    int count = n / sizeof(input_event);
    for (int i = 0; i < count; i++) {
      handle(events[i]);
    }
  }
}

}

namespace Ion {
namespace Kindle {
namespace Touch {

bool init(int screenWidth, int screenHeight) {
  sScreenW = screenWidth;
  sScreenH = screenHeight;
  for (int i = 0; i < k_maxSlots; i++) {
    sSlots[i] = Slot{-1, 0, 0};
  }
  if (getenv("UPSILON_FAKE_INPUT") != nullptr) {
    return true;
  }
  sSwapXY = envFlag("UPSILON_TOUCH_SWAP_XY");
  sMirrorX = envFlag("UPSILON_TOUCH_MIRROR_X");
  sMirrorY = envFlag("UPSILON_TOUCH_MIRROR_Y");

  const char * forced = getenv("UPSILON_TOUCH_DEVICE");
  if (forced != nullptr && *forced != 0) {
    probe(forced);
  } else {
    for (int i = 0; i < 32 && sFd < 0; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      probe(path);
    }
  }
  if (sFd < 0) {
    fprintf(stderr, "upsilon: no touchscreen found\n");
    return false;
  }
  if (envFlag("UPSILON_TOUCH_GRAB", true)) {
    sGrabbed = ioctl(sFd, EVIOCGRAB, 1) == 0;
  }
  return true;
}

void shutdown() {
  if (sFd >= 0) {
    if (sGrabbed) {
      ioctl(sFd, EVIOCGRAB, 0);
    }
    close(sFd);
    sFd = -1;
  }
}

bool poll(Event * event) {
  if (sQueueCount == 0) {
    readDevice();
  }
  if (sQueueCount == 0) {
    return false;
  }
  *event = sQueue[sQueueHead];
  sQueueHead = (sQueueHead + 1) % k_maxQueue;
  sQueueCount--;
  return true;
}

void inject(Event event) {
  push(event);
}

}
}
}
