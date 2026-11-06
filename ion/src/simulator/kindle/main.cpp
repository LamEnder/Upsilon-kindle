/* Upsilon for Kindle (kindlehf / PW4)
 *
 * Usage: upsilon [--state FILE] [--scale N] [epsilon options, e.g. --language fr]
 *
 * The panel is split between the calculator screen (scaled up by an integer
 * factor) and a touch keypad. See ion/src/simulator/kindle/README.md. */

#include "debug.h"
#include "eink.h"
#include "input.h"
#include "keypad.h"
#include "persistence.h"
#include "screen.h"
#include "touch.h"

#include <ion.h>
#include <ion/display.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

using Ion::Kindle::Keypad::Rect;

namespace {

void defaultStatePath(char * out, size_t size) {
  char exe[PATH_MAX - 32];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    snprintf(out, size, "upsilon.state");
    return;
  }
  exe[n] = 0;
  char * slash = strrchr(exe, '/');
  if (slash != nullptr) {
    *slash = 0;
  }
  // The binary lives in extensions/upsilon/bin/: keep the state one level up
  size_t len = strlen(exe);
  if (len >= 4 && strcmp(exe + len - 4, "/bin") == 0) {
    exe[len - 4] = 0;
  }
  snprintf(out, size, "%s/upsilon.state", exe);
}

void drawFrame(Rect screen) {
  int t = 3; // Border thickness, drawn just outside the calculator screen
  int gap = 2;
  Ion::Kindle::EInk::fill(screen.x - gap - t, screen.y - gap - t, screen.w + 2 * (gap + t), t, 0x00);
  Ion::Kindle::EInk::fill(screen.x - gap - t, screen.y + screen.h + gap, screen.w + 2 * (gap + t), t, 0x00);
  Ion::Kindle::EInk::fill(screen.x - gap - t, screen.y - gap, t, screen.h + 2 * gap, 0x00);
  Ion::Kindle::EInk::fill(screen.x + screen.w + gap, screen.y - gap, t, screen.h + 2 * gap, 0x00);
}

}

int main(int argc, char * argv[]) {
  setvbuf(stderr, nullptr, _IOLBF, 0);
  Ion::Kindle::Debug::init();
  Ion::Kindle::Debug::startWatchdog();

  char statePath[PATH_MAX];
  defaultStatePath(statePath, sizeof(statePath));
  int forcedScale = 0;
  const char * envScale = getenv("UPSILON_SCALE");
  if (envScale != nullptr) {
    forcedScale = atoi(envScale);
  }

  // Our own options are consumed; everything else is forwarded to Epsilon
  std::vector<const char *> args;
  args.push_back(argv[0]);
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
      snprintf(statePath, sizeof(statePath), "%s", argv[++i]);
    } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      forcedScale = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("Usage: %s [--state FILE] [--scale N] [--language CODE]\n", argv[0]);
      return 0;
    } else {
      args.push_back(argv[i]);
    }
  }

  Ion::Kindle::Debug::setPhase("opening the framebuffer");
  if (!Ion::Kindle::EInk::init()) {
    Ion::Kindle::Debug::log("cannot use the framebuffer, giving up");
    return 1;
  }
  const int W = Ion::Kindle::EInk::width();
  const int H = Ion::Kindle::EInk::height();
  const int dw = Ion::Display::Width, dh = Ion::Display::Height;
  const int margin = (W < H ? W : H) / 48;
  const bool portrait = H >= W;

  /* Pick the largest integer zoom that leaves enough room for the keypad:
   * at least 45% of the height in portrait, 40% of the width in landscape. */
  int scale = 1;
  for (int s = 6; s >= 1; s--) {
    bool fits = portrait
      ? (dw * s <= W - 2 * margin && H - (dh * s + 3 * margin) >= H * 45 / 100)
      : (dh * s <= H - 2 * margin && W - (dw * s + 3 * margin) >= W * 40 / 100);
    if (fits) {
      scale = s;
      break;
    }
  }
  if (forcedScale > 0 && dw * forcedScale <= W && dh * forcedScale <= H) {
    scale = forcedScale;
  }

  Rect screen, keypad;
  screen.w = dw * scale;
  screen.h = dh * scale;
  if (portrait) {
    screen.x = (W - screen.w) / 2;
    screen.y = margin + 4;
    keypad.x = 0;
    keypad.y = screen.y + screen.h + margin + 4;
    keypad.w = W;
    keypad.h = H - keypad.y - margin / 2;
  } else {
    screen.x = margin + 4;
    screen.y = (H - screen.h) / 2;
    keypad.x = screen.x + screen.w + margin;
    keypad.y = margin;
    keypad.w = W - keypad.x;
    keypad.h = H - 2 * margin;
  }
  Ion::Kindle::Debug::log("panel %dx%d, calculator screen x%d at (%d,%d)", W, H, scale, screen.x, screen.y);

  Ion::Kindle::Debug::setPhase("opening the touchscreen");
  bool touchOk = Ion::Kindle::Touch::init(W, H);
  if (!touchOk) {
    Ion::Kindle::Debug::log("continuing without touch input");
  }
  Ion::Kindle::Debug::setTouchFd(Ion::Kindle::Touch::fd());

  // Initial screen: white, frame, keypad, then one clean flashing refresh
  Ion::Kindle::Debug::setPhase("drawing the keypad");
  Ion::Kindle::EInk::fill(0, 0, W, H, 0xFF);
  drawFrame(screen);
  Ion::Kindle::Keypad::layout(keypad);
  Ion::Kindle::Keypad::draw();
  Ion::Kindle::Screen::init(screen, scale);
  Ion::Kindle::EInk::flushAll(true);

  Ion::Kindle::Input::init();
  Ion::Kindle::Debug::setPhase("loading the saved state");
  Ion::Kindle::Persistence::setPath(statePath);
  Ion::Kindle::Persistence::load();

  Ion::Kindle::Debug::setPhase("running");
  ion_main(args.size(), args.data());

  Ion::Kindle::Debug::setPhase("saving the state");
  Ion::Kindle::Persistence::save();
  Ion::Kindle::Debug::setPhase("exiting");
  Ion::Kindle::Touch::shutdown();
  // Leave a clean white panel behind; the launcher brings the Kindle UI back
  Ion::Kindle::EInk::fill(0, 0, W, H, 0xFF);
  Ion::Kindle::EInk::flushAll(true);
  Ion::Kindle::EInk::shutdown();
  Ion::Kindle::Debug::log("clean exit");
  return 0;
}
