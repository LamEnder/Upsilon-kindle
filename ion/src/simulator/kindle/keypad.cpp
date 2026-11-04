#include "keypad.h"
#include "eink.h"

#include <kandinsky/font.h>
#include <kandinsky/framebuffer.h>
#include <kandinsky/framebuffer_context.h>
#include <stdlib.h>
#include <string.h>

using Ion::Keyboard::Key;

namespace Ion {
namespace Kindle {
namespace Keypad {

namespace {

enum class Style : uint8_t {
  Normal,
  Dark,
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
};

struct KeyDef {
  Key key;
  Special special;
  uint8_t section;   // 0: navigation, 1: functions, 2: numbers
  uint8_t col, row, colSpan, rowSpan;
  Style style;
  const char * label;       // '^' starts a superscript
  const char * shiftLabel;
  const char * alphaLabel;
};

constexpr int k_sectionColumns[] = {6, 6, 5};
constexpr int k_sectionRows[] = {2, 3, 4};
constexpr int k_sectionWeight[] = {100, 100, 100}; // relative row heights (%)

#define K(key, sec, c, r, label, shift, alpha) \
  KeyDef{Key::key, Special::None, sec, c, r, 1, 1, Style::Normal, label, shift, alpha}
#define KD(key, sec, c, r, label, shift, alpha) \
  KeyDef{Key::key, Special::None, sec, c, r, 1, 1, Style::Dark, label, shift, alpha}

const KeyDef k_keys[] = {
  // Navigation cluster
  KeyDef{Key::Home, Special::None, 0, 0, 0, 1, 1, Style::Normal, "home", nullptr, nullptr},
  KeyDef{Key::Up, Special::None, 0, 1, 0, 3, 1, Style::ArrowUp, nullptr, nullptr, nullptr},
  KeyDef{Key::OK, Special::None, 0, 4, 0, 1, 2, Style::Dark, "OK", nullptr, nullptr},
  KeyDef{Key::Back, Special::None, 0, 5, 0, 1, 1, Style::Normal, "back", nullptr, nullptr},
  KeyDef{Key::None, Special::Quit, 0, 0, 1, 1, 1, Style::Normal, "exit", "hold", nullptr},
  KeyDef{Key::Left, Special::None, 0, 1, 1, 1, 1, Style::ArrowLeft, nullptr, nullptr, nullptr},
  KeyDef{Key::Down, Special::None, 0, 2, 1, 1, 1, Style::ArrowDown, nullptr, nullptr, nullptr},
  KeyDef{Key::Right, Special::None, 0, 3, 1, 1, 1, Style::ArrowRight, nullptr, nullptr, nullptr},
  KeyDef{Key::None, Special::Refresh, 0, 5, 1, 1, 1, Style::Normal, "clean", "screen", nullptr},

  // Function keys
  KD(Shift, 1, 0, 0, "shift", nullptr, nullptr),
  KD(Alpha, 1, 1, 0, "alpha", nullptr, nullptr),
  K(XNT, 1, 2, 0, "x,n,t", "cut", ":"),
  K(Var, 1, 3, 0, "var", "copy", ";"),
  K(Toolbox, 1, 4, 0, "toolbox", "paste", "\""),
  K(Backspace, 1, 5, 0, "del", "clear", "%"),

  K(Exp, 1, 0, 1, "e^x", "[", "A"),
  K(Ln, 1, 1, 1, "ln", "]", "B"),
  K(Log, 1, 2, 1, "log", "{", "C"),
  K(Imaginary, 1, 3, 1, "i", "}", "D"),
  K(Comma, 1, 4, 1, ",", "_", "E"),
  K(Power, 1, 5, 1, "x^y", "\xe2\x86\x92", "F"),

  K(Sine, 1, 0, 2, "sin", "asin", "G"),
  K(Cosine, 1, 1, 2, "cos", "acos", "H"),
  K(Tangent, 1, 2, 2, "tan", "atan", "I"),
  K(Pi, 1, 3, 2, "\xcf\x80", "=", "J"),
  K(Sqrt, 1, 4, 2, "\xe2\x88\x9a", "<", "K"),
  K(Square, 1, 5, 2, "x^2", ">", "L"),

  // Number keys
  K(Seven, 2, 0, 0, "7", nullptr, "M"),
  K(Eight, 2, 1, 0, "8", nullptr, "N"),
  K(Nine, 2, 2, 0, "9", nullptr, "O"),
  K(LeftParenthesis, 2, 3, 0, "(", nullptr, "P"),
  K(RightParenthesis, 2, 4, 0, ")", nullptr, "Q"),

  K(Four, 2, 0, 1, "4", nullptr, "R"),
  K(Five, 2, 1, 1, "5", nullptr, "S"),
  K(Six, 2, 2, 1, "6", nullptr, "T"),
  K(Multiplication, 2, 3, 1, "\xc3\x97", nullptr, "U"),
  K(Division, 2, 4, 1, "\xc3\xb7", nullptr, "V"),

  K(One, 2, 0, 2, "1", nullptr, "W"),
  K(Two, 2, 1, 2, "2", nullptr, "X"),
  K(Three, 2, 2, 2, "3", nullptr, "Y"),
  K(Plus, 2, 3, 2, "+", nullptr, "Z"),
  K(Minus, 2, 4, 2, "-", nullptr, "space"),

  K(Zero, 2, 0, 3, "0", nullptr, "?"),
  K(Dot, 2, 1, 3, ".", nullptr, "!"),
  K(EE, 2, 2, 3, "\xc3\x97" "10^x", nullptr, nullptr),
  K(Ans, 2, 3, 3, "ans", nullptr, nullptr),
  KD(EXE, 2, 4, 3, "EXE", nullptr, nullptr),
};

#undef K
#undef KD

constexpr int k_numberOfKeys = sizeof(k_keys) / sizeof(k_keys[0]);

Rect sRects[k_numberOfKeys];
int sGap = 8;

/* ---- Low level drawing into the e-ink canvas ---- */

inline void putPixel(int x, int y, uint8_t g) {
  if (x < 0 || y < 0 || x >= EInk::width() || y >= EInk::height()) {
    return;
  }
  EInk::canvas()[y * EInk::width() + x] = g;
}

void hspan(int x0, int x1, int y, uint8_t g) {
  if (y < 0 || y >= EInk::height()) {
    return;
  }
  if (x0 < 0) x0 = 0;
  if (x1 > EInk::width()) x1 = EInk::width();
  if (x1 > x0) {
    memset(EInk::canvas() + y * EInk::width() + x0, g, x1 - x0);
  }
}

// Horizontal inset of a rounded rectangle's edge at a given row.
int cornerInset(int row, int height, int radius) {
  int d = -1;
  if (row < radius) {
    d = radius - row;
  } else if (row >= height - radius) {
    d = row - (height - radius - 1);
  }
  if (d <= 0) {
    return 0;
  }
  // inset = r - sqrt(r^2 - d^2), with integers
  int r2 = radius * radius;
  int t = r2 - d * d;
  int s = 0;
  while ((s + 1) * (s + 1) <= t) {
    s++;
  }
  return radius - s;
}

void fillRoundedRect(Rect r, int radius, uint8_t g) {
  if (radius * 2 > r.h) radius = r.h / 2;
  if (radius * 2 > r.w) radius = r.w / 2;
  for (int j = 0; j < r.h; j++) {
    int inset = cornerInset(j, r.h, radius);
    hspan(r.x + inset, r.x + r.w - inset, r.y + j, g);
  }
}

void fillTriangle(int cx, int cy, int size, Style dir, uint8_t g) {
  // Isosceles triangle pointing in "dir", fitting in a size x size box.
  int half = size / 2;
  for (int i = 0; i <= half * 2; i++) {
    // i runs from the tip to the base
    int spread = i / 2;
    switch (dir) {
      case Style::ArrowUp:
        hspan(cx - spread, cx + spread + 1, cy - half + i, g);
        break;
      case Style::ArrowDown:
        hspan(cx - spread, cx + spread + 1, cy + half - i, g);
        break;
      case Style::ArrowLeft:
        for (int k = -spread; k <= spread; k++) putPixel(cx - half + i, cy + k, g);
        break;
      case Style::ArrowRight:
        for (int k = -spread; k <= spread; k++) putPixel(cx + half - i, cy + k, g);
        break;
      default:
        break;
    }
  }
}

/* ---- Text rendering with Kandinsky's own fonts ---- */

struct TextRun {
  const char * text;
  int length;
  const KDFont * font;
  int scale;
  int raise;  // pixels above the baseline of the main run
};

KDSize runSize(const TextRun & run) {
  KDSize s = run.font->stringSize(run.text, run.length);
  return KDSize(s.width() * run.scale, s.height() * run.scale);
}

void drawRun(const TextRun & run, int x, int y, uint8_t fg, uint8_t bg) {
  KDSize s = run.font->stringSize(run.text, run.length);
  int w = s.width(), h = s.height();
  if (w <= 0 || h <= 0) {
    return;
  }
  KDColor * pixels = static_cast<KDColor *>(malloc(sizeof(KDColor) * w * h));
  if (pixels == nullptr) {
    return;
  }
  KDFrameBuffer frameBuffer(pixels, KDSize(w, h));
  KDFrameBufferContext ctx(&frameBuffer);
  ctx.fillRect(KDRect(0, 0, w, h), KDColorWhite);
  // drawString needs a NUL terminated string; copy the run
  char buffer[64];
  int len = run.length < (int)sizeof(buffer) - 1 ? run.length : (int)sizeof(buffer) - 1;
  memcpy(buffer, run.text, len);
  buffer[len] = 0;
  ctx.drawString(buffer, KDPointZero, run.font, KDColorBlack, KDColorWhite);
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      KDColor c = pixels[j * w + i];
      // Ink amount from the (gray) rendered pixel
      int ink = 255 - ((c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000);
      if (ink <= 8) {
        continue;
      }
      uint8_t g = bg + ((fg - bg) * ink) / 255;
      for (int dy = 0; dy < run.scale; dy++) {
        for (int dx = 0; dx < run.scale; dx++) {
          putPixel(x + i * run.scale + dx, y + j * run.scale + dy, g);
        }
      }
    }
  }
  free(pixels);
}

/* Draw a label (with optional "^superscript") centered on (cx, cy). The main
 * scale is the largest of maxScale..1 that fits in maxWidth. */
void drawLabel(const char * label, int cx, int cy, int maxWidth, int maxHeight, int maxScale, const KDFont * font, uint8_t fg, uint8_t bg) {
  if (label == nullptr) {
    return;
  }
  const char * caret = strchr(label, '^');
  int baseLength = caret ? (int)(caret - label) : (int)strlen(label);
  for (int scale = maxScale; scale >= 1; scale--) {
    TextRun base{label, baseLength, font, scale, 0};
    KDSize bs = runSize(base);
    TextRun sup{caret ? caret + 1 : "", caret ? (int)strlen(caret + 1) : 0, KDFont::LargeFont, scale > 1 ? scale - 1 : 1, 0};
    KDSize ss = sup.length > 0 ? runSize(sup) : KDSize(0, 0);
    int totalWidth = bs.width() + ss.width();
    if (scale > 1 && (totalWidth > maxWidth || bs.height() > maxHeight)) {
      continue;
    }
    int x = cx - totalWidth / 2;
    int y = cy - bs.height() / 2;
    drawRun(base, x, y, fg, bg);
    if (sup.length > 0) {
      drawRun(sup, x + bs.width(), y - ss.height() / 4, fg, bg);
    }
    return;
  }
}

void drawKey(int index, bool pressed) {
  const KeyDef & def = k_keys[index];
  Rect r = sRects[index];
  bool dark = (def.style == Style::Dark) != pressed;
  uint8_t fg = dark ? 0xFF : 0x00;
  uint8_t bg = dark ? 0x00 : 0xFF;
  int radius = r.h / 5;
  if (radius > 18) radius = 18;
  // Clear the surrounding gap so stale pixels never stay around the key
  EInk::fill(r.x, r.y, r.w, r.h, 0xFF);
  fillRoundedRect(r, radius, 0x00);
  int border = 3;
  Rect inner{r.x + border, r.y + border, r.w - 2 * border, r.h - 2 * border};
  fillRoundedRect(inner, radius > border ? radius - border : 0, bg);

  bool hasSecondary = def.shiftLabel != nullptr || def.alphaLabel != nullptr;
  const KDFont * small = KDFont::LargeFont;
  int smallHeight = small->glyphSize().height();
  int pad = 6;
  if (hasSecondary && def.style != Style::ArrowUp) {
    if (def.shiftLabel != nullptr) {
      TextRun run{def.shiftLabel, (int)strlen(def.shiftLabel), small, 1, 0};
      drawRun(run, inner.x + pad, inner.y + 2, fg, bg);
    }
    if (def.alphaLabel != nullptr) {
      TextRun run{def.alphaLabel, (int)strlen(def.alphaLabel), small, 1, 0};
      KDSize s = runSize(run);
      drawRun(run, inner.x + inner.w - pad - s.width(), inner.y + 2, fg, bg);
    }
  }
  int top = inner.y + (hasSecondary ? smallHeight + 2 : 0);
  int availableHeight = inner.y + inner.h - top;
  int cx = inner.x + inner.w / 2;
  int cy = top + availableHeight / 2;
  switch (def.style) {
    case Style::ArrowUp:
    case Style::ArrowDown:
    case Style::ArrowLeft:
    case Style::ArrowRight: {
      int size = (inner.h < inner.w ? inner.h : inner.w) * 45 / 100;
      fillTriangle(cx, inner.y + inner.h / 2, size, def.style, fg);
      break;
    }
    default: {
      int maxScale = def.section == 2 ? 3 : 2;
      drawLabel(def.label, cx, cy, inner.w - 2 * pad, availableHeight, maxScale, KDFont::LargeFont, fg, bg);
      break;
    }
  }
}

}

void layout(Rect area) {
  int margin = area.w / 60;
  sGap = area.w / 120;
  if (sGap < 4) sGap = 4;
  int sectionGap = sGap * 2;
  int innerX = area.x + margin;
  int innerW = area.w - 2 * margin;

  // Total weighted height of all rows, then split the available height
  int totalWeight = 0;
  int rowGaps = 0;
  for (int s = 0; s < 3; s++) {
    totalWeight += k_sectionRows[s] * k_sectionWeight[s];
    rowGaps += k_sectionRows[s] - 1;
  }
  int usable = area.h - 2 * sectionGap - rowGaps * sGap - sGap;
  int sectionY[3];
  int rowHeight[3];
  int y = area.y;
  for (int s = 0; s < 3; s++) {
    rowHeight[s] = usable * k_sectionWeight[s] / totalWeight;
    sectionY[s] = y;
    y += k_sectionRows[s] * rowHeight[s] + (k_sectionRows[s] - 1) * sGap + sectionGap;
  }
  for (int i = 0; i < k_numberOfKeys; i++) {
    const KeyDef & def = k_keys[i];
    int cols = k_sectionColumns[def.section];
    int colWidth = (innerW - (cols - 1) * sGap) / cols;
    int rh = rowHeight[def.section];
    Rect r;
    r.x = innerX + def.col * (colWidth + sGap);
    r.y = sectionY[def.section] + def.row * (rh + sGap);
    r.w = def.colSpan * colWidth + (def.colSpan - 1) * sGap;
    r.h = def.rowSpan * rh + (def.rowSpan - 1) * sGap;
    sRects[i] = r;
  }
}

void draw() {
  for (int i = 0; i < k_numberOfKeys; i++) {
    drawKey(i, false);
  }
}

int numberOfKeys() {
  return k_numberOfKeys;
}

int hitTest(int x, int y) {
  int half = sGap / 2 + 1;
  for (int i = 0; i < k_numberOfKeys; i++) {
    Rect r = sRects[i];
    Rect grown{r.x - half, r.y - half, r.w + 2 * half, r.h + 2 * half};
    if (grown.contains(x, y)) {
      return i;
    }
  }
  return -1;
}

int findByName(const char * name) {
  struct Alias { const char * name; Key key; };
  static const Alias aliases[] = {
    {"up", Key::Up}, {"down", Key::Down}, {"left", Key::Left}, {"right", Key::Right},
    {"ok", Key::OK}, {"*", Key::Multiplication}, {"/", Key::Division}, {"pi", Key::Pi},
    {"sqrt", Key::Sqrt}, {"square", Key::Square}, {"exp", Key::Exp}, {"power", Key::Power},
    {"ee", Key::EE}, {"backspace", Key::Backspace}, {"exe", Key::EXE}, {"xnt", Key::XNT},
    {"home", Key::Home}, {"back", Key::Back}, {"shift", Key::Shift}, {"alpha", Key::Alpha},
  };
  Key wanted = Key::None;
  for (const Alias & a : aliases) {
    if (strcmp(a.name, name) == 0) {
      wanted = a.key;
    }
  }
  for (int i = 0; i < k_numberOfKeys; i++) {
    const KeyDef & def = k_keys[i];
    if ((wanted != Key::None && def.key == wanted) || (def.label != nullptr && strcmp(def.label, name) == 0)) {
      return i;
    }
  }
  return -1;
}

Rect rectOf(int index) {
  return sRects[index];
}

Key keyOf(int index) {
  return k_keys[index].key;
}

Special specialOf(int index) {
  return k_keys[index].special;
}

void setPressed(int index, bool pressed) {
  if (index < 0 || index >= k_numberOfKeys) {
    return;
  }
  drawKey(index, pressed);
  Rect r = sRects[index];
  /* DU is fast and good enough for the inverted state; going back to the
   * normal look uses AUTO so anti-aliased label edges come back clean. */
  EInk::flush(r.x, r.y, r.w, r.h, pressed ? EInk::Waveform::Fast : EInk::Waveform::Auto, false);
}

}
}
}
