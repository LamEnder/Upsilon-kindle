#include "screen.h"
#include "eink.h"

#include <ion/display.h>
#include <ion/timing.h>
#include <kandinsky.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int W = Ion::Display::Width;
constexpr int H = Ion::Display::Height;

KDColor sPixels[W * H];
uint8_t sShown[W * H];      // Grayscale content currently on the panel
uint8_t sGray[W * H];       // Scratch: grayscale version of sPixels
uint8_t sLut[65536];        // RGB565 -> gray

bool sDirty = true;
Ion::Kindle::Keypad::Rect sArea{0, 0, W, H};
int sScale = 1;
uint64_t sLastFlush = 0;
int sPartialCount = 0;
int sFlashEvery = 40;
constexpr uint64_t k_minIntervalMs = 100;

KDFrameBuffer sFrameBuffer(sPixels, KDSize(W, H));

void buildLut() {
  float gamma = 1.0f;
  const char * g = getenv("UPSILON_GAMMA");
  if (g != nullptr) {
    gamma = atof(g);
    if (gamma < 0.2f || gamma > 5.0f) {
      gamma = 1.0f;
    }
  }
  for (int v = 0; v < 65536; v++) {
    KDColor c = KDColor::RGB16(v);
    float luma = (c.red() * 0.299f + c.green() * 0.587f + c.blue() * 0.114f) / 255.0f;
    if (gamma != 1.0f) {
      luma = powf(luma, gamma);
    }
    int gray = (int)(luma * 255.0f + 0.5f);
    sLut[v] = gray < 0 ? 0 : (gray > 255 ? 255 : gray);
  }
}

// Scale a block of the grayscale calculator screen onto the canvas
void blit(int x0, int y0, int x1, int y1) {
  uint8_t * canvas = Ion::Kindle::EInk::canvas();
  int stride = Ion::Kindle::EInk::width();
  for (int y = y0; y < y1; y++) {
    uint8_t * dst = canvas + (sArea.y + y * sScale) * stride + sArea.x + x0 * sScale;
    const uint8_t * src = sGray + y * W;
    uint8_t * p = dst;
    for (int x = x0; x < x1; x++) {
      memset(p, src[x], sScale);
      p += sScale;
    }
    int rowBytes = (x1 - x0) * sScale;
    for (int k = 1; k < sScale; k++) {
      memcpy(dst + k * stride, dst, rowBytes);
    }
  }
}

}

namespace Ion {
namespace Display {

void pushRect(KDRect r, const KDColor * pixels) {
  sDirty = true;
  sFrameBuffer.pushRect(r, pixels);
}

void pushRectUniform(KDRect r, KDColor c) {
  sDirty = true;
  sFrameBuffer.pushRectUniform(r, c);
}

void pullRect(KDRect r, KDColor * pixels) {
  sFrameBuffer.pullRect(r, pixels);
}

}
}

namespace Ion {
namespace Kindle {
namespace Screen {

void init(Keypad::Rect area, int scale) {
  sArea = area;
  sScale = scale;
  const char * every = getenv("UPSILON_FLASH_EVERY");
  if (every != nullptr) {
    sFlashEvery = atoi(every);
  }
  buildLut();
  for (int i = 0; i < W * H; i++) {
    sPixels[i] = KDColorWhite;
  }
  memset(sShown, 0xFF, sizeof(sShown));
  memset(sGray, 0xFF, sizeof(sGray));
  sDirty = true;
}

Keypad::Rect area() {
  return sArea;
}

void refresh(bool force) {
  if (!sDirty) {
    return;
  }
  uint64_t now = Ion::Timing::millis();
  if (!force && now - sLastFlush < k_minIntervalMs) {
    return;
  }
  sDirty = false;

  int minX = W, minY = H, maxX = -1, maxY = -1;
  int changed = 0;
  for (int y = 0; y < H; y++) {
    const KDColor * src = sPixels + y * W;
    uint8_t * gray = sGray + y * W;
    const uint8_t * shown = sShown + y * W;
    int rowMin = -1, rowMax = -1;
    for (int x = 0; x < W; x++) {
      uint8_t g = sLut[(uint16_t)src[x]];
      gray[x] = g;
      if (g != shown[x]) {
        if (rowMin < 0) {
          rowMin = x;
        }
        rowMax = x;
        changed++;
      }
    }
    if (rowMin >= 0) {
      if (rowMin < minX) minX = rowMin;
      if (rowMax > maxX) maxX = rowMax;
      if (y < minY) minY = y;
      maxY = y;
    }
  }
  if (maxX < 0) {
    return; // Nothing visible changed
  }
  int x1 = maxX + 1, y1 = maxY + 1;
  blit(minX, minY, x1, y1);
  for (int y = minY; y < y1; y++) {
    memcpy(sShown + y * W + minX, sGray + y * W + minX, x1 - minX);
  }

  /* A flashing refresh now and then clears the ghosting that partial updates
   * leave behind. Big changes (switching apps, opening a menu) are a natural
   * moment to do it. */
  bool flash = false;
  sPartialCount++;
  if (changed > (W * H) / 2 || (sFlashEvery > 0 && sPartialCount >= sFlashEvery)) {
    flash = true;
    sPartialCount = 0;
  }
  if (flash) {
    EInk::flush(sArea.x, sArea.y, sArea.w, sArea.h, EInk::Waveform::Quality, true);
  } else {
    EInk::flush(sArea.x + minX * sScale, sArea.y + minY * sScale,
        (x1 - minX) * sScale, (y1 - minY) * sScale, EInk::Waveform::Auto, false);
  }
  sLastFlush = now;
}

void redrawAll(bool flash) {
  for (int i = 0; i < W * H; i++) {
    sGray[i] = sLut[(uint16_t)sPixels[i]];
  }
  memcpy(sShown, sGray, sizeof(sShown));
  blit(0, 0, W, H);
  sDirty = false;
  sPartialCount = 0;
  EInk::flush(sArea.x, sArea.y, sArea.w, sArea.h, flash ? EInk::Waveform::Quality : EInk::Waveform::Auto, flash);
}

}
}
}
