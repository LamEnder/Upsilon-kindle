#ifndef ION_SIMULATOR_KINDLE_TOUCH_H
#define ION_SIMULATOR_KINDLE_TOUCH_H

#include <stdint.h>

/* Single-finger touchscreen reader on top of Linux evdev.
 *
 * The touch panel is discovered by scanning /dev/input/event*, picking the
 * first device that reports absolute (multi-touch or single-touch)
 * coordinates. Raw coordinates are scaled to the framebuffer size.
 *
 * Environment overrides, for panels that are rotated relative to the fb:
 *   UPSILON_TOUCH_DEVICE=/dev/input/eventN  use that device
 *   UPSILON_TOUCH_SWAP_XY=1                 swap raw X and Y axes
 *   UPSILON_TOUCH_MIRROR_X=1                mirror (after swapping)
 *   UPSILON_TOUCH_MIRROR_Y=1
 *   UPSILON_TOUCH_GRAB=0                    don't grab the device exclusively
 *
 * When UPSILON_FAKE_INPUT=path is set, no device is opened; events are read
 * from a script file instead (see main.cpp for the syntax). */

namespace Ion {
namespace Kindle {
namespace Touch {

struct Event {
  enum class Type : uint8_t { Down, Move, Up };
  Type type;
  int x;
  int y;
};

bool init(int screenWidth, int screenHeight);
void shutdown();

/* Non-blocking: returns true and fills *event if an event is pending. */
bool poll(Event * event);

/* Inject an event (used by the scripted fake input). */
void inject(Event event);

/* File descriptor of the touchscreen, -1 if none */
int fd();

}
}
}

#endif
