#ifndef ION_SIMULATOR_KINDLE_PERSISTENCE_H
#define ION_SIMULATOR_KINDLE_PERSISTENCE_H

#include <stddef.h>
#include <stdint.h>

/* Keeps the calculator's storage (Python scripts, functions, sequences,
 * variables...) and settings in a file between runs.
 *
 * Some records hold raw expression trees that are only meaningful to the
 * exact binary that wrote them. The file therefore carries a fingerprint of
 * the executable: when it does not match (after an update), only records
 * that are safe across builds are restored (Python scripts, functions and
 * sequences, which are stored as text thanks to STRING_STORAGE). */

namespace Ion {
namespace Kindle {
namespace Persistence {

void setPath(const char * path);
void load();
void save();

/* Saves when storage changed, at most every few seconds. */
void autosave();

}

/* Settings (Poincare & global preferences), implemented in preferences.cpp
 * because it needs the apps' headers. */
namespace Preferences {
size_t size();
void save(uint8_t * buffer);
void load(const uint8_t * buffer, size_t size);
}

}
}

#endif
