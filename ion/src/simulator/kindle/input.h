#ifndef ION_SIMULATOR_KINDLE_INPUT_H
#define ION_SIMULATOR_KINDLE_INPUT_H

namespace Ion {
namespace Kindle {
namespace Input {

void init();

/* Read touch events, drive key feedback and special keys. Called from
 * Ion::Keyboard::scan(). */
void pump();

/* Called by the event loop right before it scans the keyboard, so that
 * scans made from elsewhere can be told apart. */
void willScanFromEventLoop();

/* Called by the event loop when a key press turned into an event. */
void keyPressConsumed();

bool quitRequested();
void requestQuit();

}
}
}

#endif
