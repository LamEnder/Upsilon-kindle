#include "input.h"
#include "eink.h"
#include "keypad.h"
#include "persistence.h"
#include "screen.h"
#include "touch.h"

#include <ion/keyboard.h>
#include <ion/timing.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using Ion::Keyboard::Key;
using Ion::Keyboard::State;

namespace {

/* Translating touches into calculator key states.
 *
 * Ion's event loop detects a key press as an up -> down transition between
 * two consecutive Keyboard::scan() calls made during the same getEvent()
 * call. A finger tap can be shorter than the interval between scans, or
 * happen while the OS is busy, so taps are queued. For each tap we report
 * at least one "no key" scan, then report the key until the event loop
 * acknowledges it (didPressNewKey), then keep reporting it for as long as the
 * finger stays on it so that auto-repeat works (arrows, backspace). */

struct Tap {
  Key key;
  int keyIndex;
  bool held;       // Finger still down on it
  bool consumed;   // Turned into an event (or seen by a direct poll)
  uint64_t firstPolled; // When a scan outside the event loop first saw it
};

constexpr int k_maxTaps = 16;
Tap sTaps[k_maxTaps];
int sTapHead = 0;
int sTapCount = 0;
bool sLastReportEmpty = true;
bool sEventLoopScan = false;
constexpr uint64_t k_pollVisibilityMs = 60;

int sPressedIndex = -1;        // Key under the finger, -1 if none
uint64_t sPressStart = 0;
bool sQuitTriggered = false;
volatile sig_atomic_t sQuitRequested = 0;
constexpr uint64_t k_quitHoldMs = 600;

Tap * frontTap() { return sTapCount > 0 ? &sTaps[sTapHead] : nullptr; }

void popTap() {
  sTapHead = (sTapHead + 1) % k_maxTaps;
  sTapCount--;
}

Tap * lastTap() { return sTapCount > 0 ? &sTaps[(sTapHead + sTapCount - 1) % k_maxTaps] : nullptr; }

void pushTap(Key key, int index) {
  if (sTapCount == k_maxTaps) {
    return;
  }
  sTaps[(sTapHead + sTapCount) % k_maxTaps] = Tap{key, index, true, false, 0};
  sTapCount++;
}

void fingerDown(int x, int y) {
  int index = Ion::Kindle::Keypad::hitTest(x, y);
  sPressedIndex = index;
  sPressStart = Ion::Timing::millis();
  sQuitTriggered = false;
  if (index < 0) {
    return;
  }
  Ion::Kindle::Keypad::setPressed(index, true);
  Key key = Ion::Kindle::Keypad::keyOf(index);
  if (key != Key::None) {
    pushTap(key, index);
  }
}

void fingerUp() {
  int index = sPressedIndex;
  sPressedIndex = -1;
  if (index < 0) {
    return;
  }
  Ion::Kindle::Keypad::setPressed(index, false);
  Tap * tap = lastTap();
  if (tap != nullptr && tap->keyIndex == index) {
    tap->held = false;
  }
  if (Ion::Kindle::Keypad::specialOf(index) == Ion::Kindle::Keypad::Special::Refresh) {
    Ion::Kindle::Screen::refresh(true);
    Ion::Kindle::EInk::flushAll(true);
  }
}

void checkHold() {
  if (sPressedIndex < 0 || sQuitTriggered) {
    return;
  }
  if (Ion::Kindle::Keypad::specialOf(sPressedIndex) == Ion::Kindle::Keypad::Special::Quit
      && Ion::Timing::millis() - sPressStart >= k_quitHoldMs) {
    sQuitTriggered = true;
    sQuitRequested = 1;
  }
}

/* ---- Scripted input, for testing without a touchscreen ----
 *
 * UPSILON_FAKE_INPUT=script.txt, one command per line:
 *   key NAME         tap the key with that label/alias (see Keypad::findByName)
 *   hold NAME MS     press a key for MS milliseconds
 *   tap X Y          tap at panel coordinates
 *   wait MS          do nothing for MS milliseconds
 *   shot FILE.pgm    dump the panel (fake framebuffer) to a PGM file
 *   quit             request exit
 * Lines starting with # are ignored. */

FILE * sScript = nullptr;
uint64_t sScriptNextTime = 0;
int sScriptReleaseIndex = -2;  // Pending "finger up" (-2: none)
int sScriptReleaseX = 0, sScriptReleaseY = 0;

void scriptTapAt(int x, int y, uint64_t holdMs) {
  using Ion::Kindle::Touch::Event;
  Ion::Kindle::Touch::inject(Event{Event::Type::Down, x, y});
  sScriptReleaseIndex = 0;
  sScriptReleaseX = x;
  sScriptReleaseY = y;
  sScriptNextTime = Ion::Timing::millis() + holdMs;
}

void runScript() {
  if (sScript == nullptr) {
    return;
  }
  uint64_t now = Ion::Timing::millis();
  if (now < sScriptNextTime) {
    return;
  }
  if (sScriptReleaseIndex != -2) {
    using Ion::Kindle::Touch::Event;
    Ion::Kindle::Touch::inject(Event{Event::Type::Up, sScriptReleaseX, sScriptReleaseY});
    sScriptReleaseIndex = -2;
    sScriptNextTime = now + 120; // Leave the OS time to process the key
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), sScript) != nullptr) {
    char cmd[32] = {0}, arg[200] = {0};
    long n1 = 0, n2 = 0;
    if (line[0] == '#' || sscanf(line, "%31s", cmd) != 1) {
      continue;
    }
    if (strcmp(cmd, "key") == 0 || strcmp(cmd, "hold") == 0) {
      long ms = 60;
      if (strcmp(cmd, "hold") == 0) {
        sscanf(line, "%*s %199s %ld", arg, &ms);
      } else {
        sscanf(line, "%*s %199s", arg);
      }
      int index = Ion::Kindle::Keypad::findByName(arg);
      if (index < 0) {
        fprintf(stderr, "script: unknown key '%s'\n", arg);
        continue;
      }
      Ion::Kindle::Keypad::Rect r = Ion::Kindle::Keypad::rectOf(index);
      scriptTapAt(r.x + r.w / 2, r.y + r.h / 2, ms);
      return;
    }
    if (strcmp(cmd, "tap") == 0 && sscanf(line, "%*s %ld %ld", &n1, &n2) == 2) {
      scriptTapAt((int)n1, (int)n2, 60);
      return;
    }
    if (strcmp(cmd, "wait") == 0 && sscanf(line, "%*s %ld", &n1) == 1) {
      sScriptNextTime = now + n1;
      return;
    }
    if (strcmp(cmd, "shot") == 0 && sscanf(line, "%*s %199s", arg) == 1) {
      Ion::Kindle::Screen::refresh(true);
      Ion::Kindle::EInk::snapshot(arg);
      fprintf(stderr, "script: wrote %s\n", arg);
      continue;
    }
    if (strcmp(cmd, "quit") == 0) {
      sQuitRequested = 1;
      return;
    }
    fprintf(stderr, "script: cannot parse '%s'\n", line);
  }
  // End of script
  fclose(sScript);
  sScript = nullptr;
}

void onSignal(int) {
  sQuitRequested = 1;
}

}

namespace Ion {
namespace Kindle {
namespace Input {

void init() {
  const char * script = getenv("UPSILON_FAKE_INPUT");
  if (script != nullptr && *script != 0) {
    sScript = fopen(script, "r");
    if (sScript == nullptr) {
      fprintf(stderr, "upsilon: cannot open input script %s\n", script);
    }
    sScriptNextTime = Ion::Timing::millis() + 300;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
}

void pump() {
  runScript();
  Touch::Event e;
  while (Touch::poll(&e)) {
    switch (e.type) {
      case Touch::Event::Type::Down:
        fingerDown(e.x, e.y);
        break;
      case Touch::Event::Type::Up:
        fingerUp();
        break;
      case Touch::Event::Type::Move:
        // Keys are latched on touch down, like physical keys; sliding the
        // finger does not change the key.
        break;
    }
  }
  checkHold();
}

void willScanFromEventLoop() {
  sEventLoopScan = true;
}

void keyPressConsumed() {
  Tap * tap = frontTap();
  if (tap != nullptr && !tap->consumed) {
    tap->consumed = true;
  }
}

bool quitRequested() {
  return sQuitRequested != 0;
}

void requestQuit() {
  sQuitRequested = 1;
}

}
}
}

namespace Ion {
namespace Keyboard {

State scan() {
  // The OS polls the keyboard when idle: good time to update the panel.
  Kindle::Screen::refresh();
  Kindle::Input::pump();
  Kindle::Persistence::autosave();

  bool fromEventLoop = sEventLoopScan;
  sEventLoopScan = false;

  State state;
  Tap * tap = frontTap();
  if (tap != nullptr && tap->consumed && !tap->held) {
    popTap();
    sLastReportEmpty = true;
    return state;
  }
  if (tap == nullptr) {
    sLastReportEmpty = true;
    return state;
  }

  if (!fromEventLoop) {
    /* Someone polls the keyboard directly: Python checking for an
     * interruption, ion.keydown(), Poincare's circuit breaker... They want
     * the current state. A tap they see is not turned into an event later,
     * just like a hardware key held while the OS was busy. Released taps
     * stay visible for a short while so that pollers needing a key to be
     * down across two scans still notice it. */
    if (!tap->consumed) {
      uint64_t now = Ion::Timing::millis();
      if (tap->firstPolled == 0) {
        tap->firstPolled = now;
      }
      if (tap->held || now - tap->firstPolled > k_pollVisibilityMs) {
        tap->consumed = true;
      }
    }
    sLastReportEmpty = false;
    state.setKey(tap->key);
    return state;
  }

  if (!tap->consumed) {
    if (!sLastReportEmpty) {
      // Make sure the OS sees the key going up before the new press
      sLastReportEmpty = true;
      return state;
    }
    sLastReportEmpty = false;
    state.setKey(tap->key);
    return state;
  }
  // Consumed and still held: keep it down so auto-repeat works
  sLastReportEmpty = false;
  state.setKey(tap->key);
  return state;
}

}
}
