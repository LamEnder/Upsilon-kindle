#ifndef ION_SIMULATOR_KINDLE_KEYPAD_H
#define ION_SIMULATOR_KINDLE_KEYPAD_H

#include <ion/keyboard.h>

/* On-screen NumWorks keypad, drawn below (or beside) the calculator screen.
 * Layout follows the physical calculator: navigation cluster, three rows of
 * six function keys, four rows of five number keys. Each key shows its main
 * label, plus its shift (top left) and alpha (top right) labels. */

namespace Ion {
namespace Kindle {
namespace Keypad {

enum class Special : uint8_t {
  None,
  Quit,     // Hold to leave Upsilon and go back to the Kindle UI
  Refresh,  // Full flashing refresh of the whole screen (clears ghosting)
};

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

/* Compute key positions inside the given area of the canvas. */
void layout(Rect area);

/* Draw every key into the canvas (no panel refresh). */
void draw();

int numberOfKeys();
int hitTest(int x, int y);      // -1 when the point is not on a key
Rect rectOf(int index);
Ion::Keyboard::Key keyOf(int index);
Special specialOf(int index);

/* Find a key by label or alias ("7", "EXE", "up", "*", "pi", "exit"...).
 * Used by scripted test input. */
int findByName(const char * name);

/* Redraw a key inverted (pressed) or normal and refresh it on the panel. */
void setPressed(int index, bool pressed);

}
}
}

#endif
