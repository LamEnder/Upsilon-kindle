#include "persistence.h"

#include <apps/global_preferences.h>
#include <poincare/preferences.h>
#include <string.h>

/* Raw copies of the two settings objects. They are only restored by the very
 * same binary that saved them (see Persistence::load), so layout and pointer
 * values (e.g. the selected font) are guaranteed to be identical. */

namespace Ion {
namespace Kindle {
namespace Preferences {

size_t size() {
  return sizeof(GlobalPreferences) + sizeof(Poincare::Preferences);
}

void save(uint8_t * buffer) {
  memcpy(buffer, static_cast<void *>(GlobalPreferences::sharedGlobalPreferences()), sizeof(GlobalPreferences));
  memcpy(buffer + sizeof(GlobalPreferences), static_cast<void *>(Poincare::Preferences::sharedPreferences()), sizeof(Poincare::Preferences));
}

void load(const uint8_t * buffer, size_t length) {
  if (length != size()) {
    return;
  }
  memcpy(static_cast<void *>(GlobalPreferences::sharedGlobalPreferences()), buffer, sizeof(GlobalPreferences));
  memcpy(static_cast<void *>(Poincare::Preferences::sharedPreferences()), buffer + sizeof(GlobalPreferences), sizeof(Poincare::Preferences));
}

}
}
}
