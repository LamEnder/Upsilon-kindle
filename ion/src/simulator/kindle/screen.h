#ifndef ION_SIMULATOR_KINDLE_SCREEN_H
#define ION_SIMULATOR_KINDLE_SCREEN_H

#include "keypad.h"

/* The calculator's 320x240 RGB565 framebuffer, shown scaled on the panel.
 *
 * Ion::Display::pushRect & co. only write into an in-memory framebuffer and
 * mark it dirty. refresh() is called whenever the OS polls the keyboard, i.e.
 * when it is idle or between steps of a long computation: it converts the
 * framebuffer to grayscale, diffs it against what the panel currently shows
 * and sends one partial e-ink update covering the changed area. */

namespace Ion {
namespace Kindle {
namespace Screen {

void init(Keypad::Rect area, int scale);

/* Push pending changes to the panel. Rate limited unless "force". */
void refresh(bool force = false);

/* Redraw the calculator screen region and request a flashing update. */
void redrawAll(bool flash);

Keypad::Rect area();

}
}
}

#endif
