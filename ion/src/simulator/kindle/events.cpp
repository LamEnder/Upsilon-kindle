#include "events.h"
#include "input.h"
#include <ion/events.h>

namespace Ion {
namespace Events {

void didPressNewKey() {
  Kindle::Input::keyPressConsumed();
}

char * sharedExternalTextBuffer() {
  static char buffer[sharedExternalTextBufferSize];
  return buffer;
}

const char * Event::text() const {
  if (*this == ExternalText) {
    return const_cast<const char *>(sharedExternalTextBuffer());
  }
  return defaultText();
}

Event getPlatformEvent() {
  if (Kindle::Input::quitRequested()) {
    return Termination;
  }
  // Ion::Events::getEvent() calls Keyboard::scan() right after this
  Kindle::Input::willScanFromEventLoop();
  return None;
}

}
}
