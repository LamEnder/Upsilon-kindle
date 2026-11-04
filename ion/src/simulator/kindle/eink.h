#ifndef ION_SIMULATOR_KINDLE_EINK_H
#define ION_SIMULATOR_KINDLE_EINK_H

#include <stdint.h>

/* Minimal e-ink framebuffer driver for Kindle devices built on the
 * Freescale/NXP mxc_epdc controller (PW2/PW3, KOA2/KOA3, PW4/KT4).
 *
 * All drawing happens in an 8-bit grayscale "canvas" (0 = black, 255 = white)
 * that has the same dimensions as the visible framebuffer. flush() copies a
 * rectangle of the canvas into the real framebuffer (converting to whatever
 * pixel format it uses) and asks the EPDC to update that region of the panel.
 *
 * When the environment variable UPSILON_FAKE_FB=WIDTHxHEIGHT is set, no device
 * is opened at all: the canvas lives in memory and can be dumped to a PGM file
 * with snapshot(). This is used for testing on a desktop / under qemu. */

namespace Ion {
namespace Kindle {
namespace EInk {

enum class Waveform : uint8_t {
  Auto,     // Let the EPDC pick (DU for pure B&W regions, GC16 otherwise)
  Fast,     // DU: ~260ms, black & white only. Used for keypad feedback.
  Quality,  // GC16: high fidelity, 16 gray levels
  Text,     // GL16: high fidelity, less flashing on white backgrounds
};

bool init();
void shutdown();
bool isFake();

int width();
int height();
uint8_t * canvas();          // width() * height() bytes, row-major, no padding

void fill(int x, int y, int w, int h, uint8_t gray);

/* Push a canvas rectangle to the panel. "full" requests a flashing update,
 * which clears ghosting at the cost of a black flash. */
void flush(int x, int y, int w, int h, Waveform waveform, bool full);
void flushAll(bool full);

/* Block until every update submitted so far has been displayed. */
void waitForCompletion();

/* Fake framebuffer only: write the canvas to a binary PGM file. */
bool snapshot(const char * path);

}
}
}

#endif
