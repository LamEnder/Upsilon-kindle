#ifndef ION_SIMULATOR_KINDLE_EVENTS_H
#define ION_SIMULATOR_KINDLE_EVENTS_H

#include <ion/events.h>
#include <stddef.h>

namespace Ion {
namespace Events {

static constexpr size_t sharedExternalTextBufferSize = 8;
char * sharedExternalTextBuffer();

}
}

#endif
