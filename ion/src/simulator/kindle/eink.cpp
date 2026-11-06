#include "eink.h"
#include "debug.h"
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* EPDC interface definitions. These mirror the userspace-visible parts of the
 * mxcfb.h header shipped in Amazon's kernel sources. Lab126 changed the
 * layout of the update structure several times; since the size of the
 * structure is encoded in the ioctl number, sending the wrong variant just
 * fails with ENOTTY/EINVAL and we try the next one. */

namespace {

struct mxcfb_rect {
  uint32_t top;
  uint32_t left;
  uint32_t width;
  uint32_t height;
};

struct mxcfb_alt_buffer_data {
  uint32_t phys_addr;
  uint32_t width;
  uint32_t height;
  mxcfb_rect alt_update_region;
};

// PW4 / KT4 ("Rex")
struct mxcfb_update_data_rex {
  mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int temp;
  unsigned int flags;
  int dither_mode;
  int quant_bit;
  mxcfb_alt_buffer_data alt_buffer_data;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
};

// KOA2 / KOA3 ("Zelda")
struct mxcfb_update_data_zelda {
  mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int temp;
  unsigned int flags;
  int dither_mode;
  int quant_bit;
  mxcfb_alt_buffer_data alt_buffer_data;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
  uint32_t ts_pxp;
  uint32_t ts_epdc;
};

// PW2 / KV / PW3 / KOA / KT2 / KT3
struct mxcfb_update_data_legacy {
  mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
  int temp;
  unsigned int flags;
  mxcfb_alt_buffer_data alt_buffer_data;
};

struct mxcfb_update_marker_data {
  uint32_t update_marker;
  uint32_t collision_test;
};

#define MXCFB_SEND_UPDATE_REX _IOW('F', 0x2E, mxcfb_update_data_rex)
#define MXCFB_SEND_UPDATE_ZELDA _IOW('F', 0x2E, mxcfb_update_data_zelda)
#define MXCFB_SEND_UPDATE_LEGACY _IOW('F', 0x2E, mxcfb_update_data_legacy)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE _IOWR('F', 0x2F, mxcfb_update_marker_data)

constexpr uint32_t UPDATE_MODE_PARTIAL = 0x0;
constexpr uint32_t UPDATE_MODE_FULL = 0x1;

constexpr uint32_t WAVEFORM_MODE_DU = 0x1;
constexpr uint32_t WAVEFORM_MODE_GC16 = 0x2;
constexpr uint32_t WAVEFORM_MODE_GL16_LEGACY = 0x5;
constexpr uint32_t WAVEFORM_MODE_GL16_ZELDA = 0x3; // Also used by Rex
constexpr uint32_t WAVEFORM_MODE_AUTO = 257;

constexpr int TEMP_USE_AMBIENT = 0x1000;

constexpr int GRAYSCALE_8BIT_INVERTED = 0x2;

enum class Api : uint8_t {
  Unknown,
  Rex,
  Zelda,
  Legacy,
  None, // No EPDC ioctl worked: assume a plain (e.g. LCD or emulated) fb
};

int sFd = -1;
uint8_t * sFbMemory = nullptr;
size_t sFbMemorySize = 0;
uint32_t sLineLength = 0;
uint32_t sBpp = 8;
uint32_t sYOffset = 0;
bool sInverted = false;
fb_var_screeninfo sVarInfo;

int sWidth = 0;
int sHeight = 0;
uint8_t * sCanvas = nullptr;
bool sFake = false;

Api sApi = Api::Unknown;
uint32_t sMarker = 0;

uint32_t waveformValue(Ion::Kindle::EInk::Waveform waveform, bool full) {
  using Ion::Kindle::EInk::Waveform;
  switch (waveform) {
    case Waveform::Fast:
      return WAVEFORM_MODE_DU;
    case Waveform::Quality:
      return WAVEFORM_MODE_GC16;
    case Waveform::Text:
      return sApi == Api::Legacy ? WAVEFORM_MODE_GL16_LEGACY : WAVEFORM_MODE_GL16_ZELDA;
    case Waveform::Auto:
    default:
      return full ? WAVEFORM_MODE_GC16 : WAVEFORM_MODE_AUTO;
  }
}

bool sendRex(const mxcfb_rect & r, uint32_t wfm, uint32_t mode) {
  mxcfb_update_data_rex u;
  memset(&u, 0, sizeof(u));
  u.update_region = r;
  u.waveform_mode = wfm;
  u.update_mode = mode;
  u.update_marker = sMarker;
  u.temp = TEMP_USE_AMBIENT;
  u.hist_bw_waveform_mode = WAVEFORM_MODE_DU;
  u.hist_gray_waveform_mode = WAVEFORM_MODE_GC16;
  return ioctl(sFd, MXCFB_SEND_UPDATE_REX, &u) == 0;
}

bool sendZelda(const mxcfb_rect & r, uint32_t wfm, uint32_t mode) {
  mxcfb_update_data_zelda u;
  memset(&u, 0, sizeof(u));
  u.update_region = r;
  u.waveform_mode = wfm;
  u.update_mode = mode;
  u.update_marker = sMarker;
  u.temp = TEMP_USE_AMBIENT;
  u.hist_bw_waveform_mode = WAVEFORM_MODE_DU;
  u.hist_gray_waveform_mode = WAVEFORM_MODE_GC16;
  return ioctl(sFd, MXCFB_SEND_UPDATE_ZELDA, &u) == 0;
}

bool sendLegacy(const mxcfb_rect & r, uint32_t wfm, uint32_t mode) {
  mxcfb_update_data_legacy u;
  memset(&u, 0, sizeof(u));
  u.update_region = r;
  u.waveform_mode = wfm;
  u.update_mode = mode;
  u.update_marker = sMarker;
  u.temp = TEMP_USE_AMBIENT;
  u.hist_bw_waveform_mode = WAVEFORM_MODE_DU;
  u.hist_gray_waveform_mode = WAVEFORM_MODE_GC16;
  return ioctl(sFd, MXCFB_SEND_UPDATE_LEGACY, &u) == 0;
}

uint32_t elapsedMs(const struct timespec & start) {
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &end);
  return (uint32_t)((end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000);
}

constexpr uint32_t k_slowIoctlMs = 500;

void sendUpdate(const mxcfb_rect & r, Ion::Kindle::EInk::Waveform waveform, bool full) {
  if (sApi == Api::None) {
    return;
  }
  sMarker = (sMarker % 0xFFFF) + 1;
  uint32_t mode = full ? UPDATE_MODE_FULL : UPDATE_MODE_PARTIAL;
  if (sApi != Api::Unknown) {
    uint32_t wfm = waveformValue(waveform, full);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool ok = sApi == Api::Rex ? sendRex(r, wfm, mode)
            : sApi == Api::Zelda ? sendZelda(r, wfm, mode)
            : sendLegacy(r, wfm, mode);
    int savedErrno = errno;
    uint32_t ms = elapsedMs(start);
    Ion::Kindle::Debug::Counters & c = Ion::Kindle::Debug::counters();
    c.einkUpdates++;
    if (ms > c.einkMaxMs) {
      c.einkMaxMs = ms;
    }
    if (!ok) {
      c.einkFailures++;
      Ion::Kindle::Debug::log("e-ink: update #%u %ux%u+%u+%u failed: %s", sMarker, r.width, r.height, r.left, r.top, strerror(savedErrno));
    } else if (ms >= k_slowIoctlMs) {
      Ion::Kindle::Debug::log("e-ink: update #%u %ux%u+%u+%u took %u ms (slow)", sMarker, r.width, r.height, r.left, r.top, ms);
    } else {
      KINDLE_VLOG("e-ink: update #%u %ux%u+%u+%u waveform %u%s, %u ms", sMarker, r.width, r.height, r.left, r.top,
          wfm, full ? " full" : "", ms);
    }
    return;
  }
  // First update: probe which structure layout this kernel understands.
  const Api candidates[] = {Api::Rex, Api::Zelda, Api::Legacy};
  for (Api candidate : candidates) {
    sApi = candidate;
    uint32_t wfm = waveformValue(waveform, full);
    bool ok = candidate == Api::Rex ? sendRex(r, wfm, mode)
            : candidate == Api::Zelda ? sendZelda(r, wfm, mode)
            : sendLegacy(r, wfm, mode);
    if (ok) {
      Ion::Kindle::Debug::log("e-ink: using %s EPDC update API",
          candidate == Api::Rex ? "Rex (PW4/KT4)" : candidate == Api::Zelda ? "Zelda (KOA2/KOA3)" : "legacy mxcfb");
      return;
    }
  }
  Ion::Kindle::Debug::log("e-ink: no update ioctl accepted (%s); display will not refresh", strerror(errno));
  sApi = Api::None;
}

inline uint8_t toDevice(uint8_t gray) {
  return sInverted ? 255 - gray : gray;
}

void copyToFramebuffer(int x, int y, int w, int h) {
  if (sFbMemory == nullptr) {
    return;
  }
  for (int j = y; j < y + h; j++) {
    const uint8_t * src = sCanvas + j * sWidth + x;
    uint8_t * row = sFbMemory + (size_t)(j + sYOffset) * sLineLength;
    switch (sBpp) {
      case 8: {
        uint8_t * dst = row + x;
        if (!sInverted) {
          memcpy(dst, src, w);
        } else {
          for (int i = 0; i < w; i++) {
            dst[i] = 255 - src[i];
          }
        }
        break;
      }
      case 16: {
        uint16_t * dst = reinterpret_cast<uint16_t *>(row) + x;
        for (int i = 0; i < w; i++) {
          uint8_t g = toDevice(src[i]);
          dst[i] = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
        }
        break;
      }
      case 24: {
        uint8_t * dst = row + x * 3;
        for (int i = 0; i < w; i++) {
          uint8_t g = toDevice(src[i]);
          dst[3 * i] = dst[3 * i + 1] = dst[3 * i + 2] = g;
        }
        break;
      }
      case 32: {
        uint32_t * dst = reinterpret_cast<uint32_t *>(row) + x;
        for (int i = 0; i < w; i++) {
          uint32_t g = toDevice(src[i]);
          dst[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
        break;
      }
      default:
        return;
    }
  }
}

bool clip(int & x, int & y, int & w, int & h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sWidth) { w = sWidth - x; }
  if (y + h > sHeight) { h = sHeight - y; }
  return w > 0 && h > 0;
}

}

namespace Ion {
namespace Kindle {
namespace EInk {

bool init() {
  const char * fake = getenv("UPSILON_FAKE_FB");
  if (fake != nullptr && *fake != 0) {
    int w = 0, h = 0;
    if (sscanf(fake, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
      w = 1072;
      h = 1448;
    }
    sFake = true;
    sApi = Api::None;
    sWidth = w;
    sHeight = h;
  } else {
    const char * path = getenv("UPSILON_FB_DEVICE");
    sFd = open(path ? path : "/dev/fb0", O_RDWR | O_CLOEXEC);
    if (sFd < 0) {
      Ion::Kindle::Debug::log("cannot open framebuffer: %s", strerror(errno));
      return false;
    }
    fb_fix_screeninfo fixInfo;
    if (ioctl(sFd, FBIOGET_VSCREENINFO, &sVarInfo) != 0 || ioctl(sFd, FBIOGET_FSCREENINFO, &fixInfo) != 0) {
      Ion::Kindle::Debug::log("cannot query framebuffer: %s", strerror(errno));
      close(sFd);
      sFd = -1;
      return false;
    }
    sWidth = sVarInfo.xres;
    sHeight = sVarInfo.yres;
    sBpp = sVarInfo.bits_per_pixel;
    sLineLength = fixInfo.line_length;
    sYOffset = sVarInfo.yoffset;
    sInverted = (sBpp == 8 && sVarInfo.grayscale == GRAYSCALE_8BIT_INVERTED);
    sFbMemorySize = fixInfo.smem_len;
    void * mem = mmap(nullptr, sFbMemorySize, PROT_READ | PROT_WRITE, MAP_SHARED, sFd, 0);
    if (mem == MAP_FAILED) {
      Ion::Kindle::Debug::log("cannot map framebuffer: %s", strerror(errno));
      close(sFd);
      sFd = -1;
      return false;
    }
    sFbMemory = static_cast<uint8_t *>(mem);
    Ion::Kindle::Debug::log("framebuffer %dx%d, %ubpp, stride %u%s, rotate %u",
        sWidth, sHeight, sBpp, sLineLength, sInverted ? ", inverted" : "", sVarInfo.rotate);
    if (sBpp != 8 && sBpp != 16 && sBpp != 24 && sBpp != 32) {
      Ion::Kindle::Debug::log("unsupported framebuffer depth");
      shutdown();
      return false;
    }
  }
  sCanvas = static_cast<uint8_t *>(malloc((size_t)sWidth * sHeight));
  if (sCanvas == nullptr) {
    shutdown();
    return false;
  }
  memset(sCanvas, 0xFF, (size_t)sWidth * sHeight);
  return true;
}

void shutdown() {
  if (sFd >= 0) {
    waitForCompletion();
  }
  if (sFbMemory != nullptr) {
    munmap(sFbMemory, sFbMemorySize);
    sFbMemory = nullptr;
  }
  if (sFd >= 0) {
    close(sFd);
    sFd = -1;
  }
  free(sCanvas);
  sCanvas = nullptr;
}

bool isFake() {
  return sFake;
}

int width() {
  return sWidth;
}

int height() {
  return sHeight;
}

uint8_t * canvas() {
  return sCanvas;
}

void fill(int x, int y, int w, int h, uint8_t gray) {
  if (!clip(x, y, w, h)) {
    return;
  }
  for (int j = y; j < y + h; j++) {
    memset(sCanvas + j * sWidth + x, gray, w);
  }
}

void flush(int x, int y, int w, int h, Waveform waveform, bool full) {
  if (!clip(x, y, w, h)) {
    return;
  }
  copyToFramebuffer(x, y, w, h);
  if (sFd >= 0) {
    mxcfb_rect r;
    r.top = y;
    r.left = x;
    r.width = w;
    r.height = h;
    sendUpdate(r, waveform, full);
  }
}

void flushAll(bool full) {
  flush(0, 0, sWidth, sHeight, full ? Waveform::Quality : Waveform::Auto, full);
}

void waitForCompletion() {
  if (sFd < 0 || sApi == Api::None || sApi == Api::Unknown || sMarker == 0) {
    return;
  }
  mxcfb_update_marker_data m;
  m.update_marker = sMarker;
  m.collision_test = 0;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int result = ioctl(sFd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
  uint32_t ms = elapsedMs(start);
  if (result < 0 || ms >= k_slowIoctlMs) {
    Ion::Kindle::Debug::log("e-ink: waiting for update #%u took %u ms%s%s", sMarker, ms,
        result < 0 ? ", error: " : "", result < 0 ? strerror(errno) : "");
  }
}

bool snapshot(const char * path) {
  if (sCanvas == nullptr) {
    return false;
  }
  FILE * f = fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  fprintf(f, "P5\n%d %d\n255\n", sWidth, sHeight);
  fwrite(sCanvas, 1, (size_t)sWidth * sHeight, f);
  fclose(f);
  return true;
}

}
}
}
