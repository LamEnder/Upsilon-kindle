/* Kindle file manager, shown as "Files" in the External app.
 *
 * Copies Python scripts between a folder on the Kindle's storage (by default
 * /mnt/us/upsilon/files, see UPSILON_FILES in upsilon.sh) and the calculator's
 * memory, where the Python app can run them. Books (.txt, .urt) don't need
 * copying: the launcher starts Upsilon in that folder and the Reader app reads
 * them from there directly.
 *
 *   ◀ ▶        switch between the Kindle and Calculator lists
 *   ▲ ▼        select (shift ▲ ▼: a page at a time)
 *   OK / EXE   copy the selected script to the other side
 *   DEL        delete the selected script from the calculator
 *   Back/Home  leave
 */

#include <ion.h>
#include <kandinsky.h>
#include <escher.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../global_preferences.h"

namespace {

constexpr KDCoordinate k_width = 320;
constexpr KDCoordinate k_height = 240;
constexpr KDCoordinate k_headerHeight = 18;
constexpr KDCoordinate k_tabHeight = 22;
constexpr KDCoordinate k_footerHeight = 20;
constexpr KDCoordinate k_listTop = k_headerHeight + k_tabHeight;
constexpr KDCoordinate k_listBottom = k_height - k_footerHeight;

constexpr int k_maxEntries = 128;
constexpr int k_maxNameLength = 48;     // base name, without ".py"
constexpr size_t k_maxFileSize = 48 * 1024;
constexpr const char * k_extension = "py";

constexpr KDColor k_text = Palette::PrimaryText;
constexpr KDColor k_secondaryText = Palette::SecondaryText;
constexpr KDColor k_background = Palette::BackgroundHard;
constexpr KDColor k_bar = Palette::Toolbar;
constexpr KDColor k_barText = Palette::ToolbarText;

struct Entry {
  char name[k_maxNameLength + 8];
  size_t size;
};

enum class Side { Kindle = 0, Calculator = 1 };

enum class Pending { None, ImportReplace, ExportReplace, Delete };

class FileManager {
public:
  FileManager();
  void run();

private:
  void reload();
  void loadKindle();
  void loadCalculator();
  int count() const { return m_count[(int)m_side]; }
  Entry & selected() { return m_entries[(int)m_side][m_selected[(int)m_side]]; }

  void draw();
  void drawHeader();
  void drawTabs();
  void drawList();
  void drawFooter();
  void drawText(const char * text, KDCoordinate x, KDCoordinate y, KDCoordinate maxWidth, const KDFont * font, KDColor fg, KDColor bg);

  bool handle(Ion::Events::Event event); // false: leave
  void moveSelection(int delta);
  void activate();
  void confirm();
  void importSelected(bool replace);
  void exportSelected(bool replace);
  void deleteSelected();

  void setMessage(const char * format, ...);
  void kindlePath(const char * name, char * buffer, size_t size) const;

  char m_folder[256];
  bool m_folderOk;
  Entry m_entries[2][k_maxEntries];
  int m_count[2];
  int m_selected[2];
  int m_scroll[2];
  Side m_side;
  Pending m_pending;
  char m_pendingName[k_maxNameLength + 8];
  char m_message[96];
  bool m_messageIsError;
};

FileManager::FileManager() :
  m_folderOk(false),
  m_count{0, 0},
  m_selected{0, 0},
  m_scroll{0, 0},
  m_side(Side::Kindle),
  m_pending(Pending::None),
  m_pendingName{0},
  m_message{0},
  m_messageIsError(false)
{
  const char * folder = getenv("UPSILON_FILES");
  if (folder == nullptr || folder[0] == 0) {
    folder = ".";
  }
  if (realpath(folder, m_folder) == nullptr) {
    strlcpy(m_folder, folder, sizeof(m_folder));
  }
}

/* ---- Lists ---- */

bool hasExtension(const char * name, const char * extension) {
  size_t n = strlen(name);
  size_t e = strlen(extension);
  return n > e + 1 && name[n - e - 1] == '.' && strcmp(name + n - e, extension) == 0;
}

int compareEntries(const void * a, const void * b) {
  return strcmp(static_cast<const Entry *>(a)->name, static_cast<const Entry *>(b)->name);
}

void FileManager::reload() {
  loadKindle();
  loadCalculator();
  for (int s = 0; s < 2; s++) {
    if (m_selected[s] >= m_count[s]) {
      m_selected[s] = m_count[s] > 0 ? m_count[s] - 1 : 0;
    }
  }
}

void FileManager::loadKindle() {
  int & n = m_count[(int)Side::Kindle];
  n = 0;
  mkdir(m_folder, 0755);
  DIR * dir = opendir(m_folder);
  m_folderOk = dir != nullptr;
  if (dir == nullptr) {
    return;
  }
  struct dirent * d;
  while ((d = readdir(dir)) != nullptr && n < k_maxEntries) {
    if (d->d_name[0] == '.' || !hasExtension(d->d_name, k_extension) || strlen(d->d_name) >= sizeof(Entry::name)) {
      continue;
    }
    char path[512];
    kindlePath(d->d_name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }
    Entry & e = m_entries[(int)Side::Kindle][n++];
    strlcpy(e.name, d->d_name, sizeof(e.name));
    e.size = st.st_size;
  }
  closedir(dir);
  qsort(m_entries[(int)Side::Kindle], n, sizeof(Entry), compareEntries);
}

/* A script record is: one status byte, the source, a null terminator. */
size_t scriptLength(const Ion::Storage::Record::Data & data) {
  if (data.size < 1) {
    return 0;
  }
  return strnlen(static_cast<const char *>(data.buffer) + 1, data.size - 1);
}

void FileManager::loadCalculator() {
  int & n = m_count[(int)Side::Calculator];
  n = 0;
  Ion::Storage * storage = Ion::Storage::sharedStorage();
  int records = storage->numberOfRecordsWithExtension(k_extension);
  for (int i = 0; i < records && n < k_maxEntries; i++) {
    Ion::Storage::Record r = storage->recordWithExtensionAtIndex(k_extension, i);
    const char * name = r.fullName();
    if (name == nullptr || strlen(name) >= sizeof(Entry::name)) {
      continue;
    }
    Entry & e = m_entries[(int)Side::Calculator][n++];
    strlcpy(e.name, name, sizeof(e.name));
    e.size = scriptLength(r.value());
  }
  qsort(m_entries[(int)Side::Calculator], n, sizeof(Entry), compareEntries);
}

void FileManager::kindlePath(const char * name, char * buffer, size_t size) const {
  snprintf(buffer, size, "%s/%s", m_folder, name);
}

/* ---- Drawing ---- */

void FileManager::drawText(const char * text, KDCoordinate x, KDCoordinate y, KDCoordinate maxWidth, const KDFont * font, KDColor fg, KDColor bg) {
  KDCoordinate glyph = font->glyphSize().width();
  int maxChars = maxWidth / glyph;
  char buffer[128];
  int length = strlen(text);
  if (maxChars <= 0) {
    return;
  }
  if (length > maxChars && maxChars > 3 && maxChars < (int)sizeof(buffer)) {
    // Keep the end, it usually tells the files apart
    snprintf(buffer, sizeof(buffer), "...%s", text + length - (maxChars - 3));
    text = buffer;
  }
  KDIonContext::sharedContext()->drawString(text, KDPoint(x, y), font, fg, bg);
}

void FileManager::drawHeader() {
  KDContext * ctx = KDIonContext::sharedContext();
  ctx->fillRect(KDRect(0, 0, k_width, k_headerHeight), k_bar);
  const char * title = "FILES";
  KDSize size = KDFont::SmallFont->stringSize(title);
  ctx->drawString(title, KDPoint((k_width - size.width()) / 2, (k_headerHeight - size.height()) / 2), KDFont::SmallFont, k_barText, k_bar);
}

void FileManager::drawTabs() {
  KDContext * ctx = KDIonContext::sharedContext();
  const char * labels[2] = {"Kindle", "Calculator"};
  for (int s = 0; s < 2; s++) {
    bool active = s == (int)m_side;
    KDRect rect(s * k_width / 2, k_headerHeight, k_width / 2, k_tabHeight);
    KDColor bg = active ? k_background : Palette::BackgroundAppsSecondary;
    ctx->fillRect(rect, bg);
    char label[32];
    snprintf(label, sizeof(label), "%s (%d)", labels[s], m_count[s]);
    KDSize size = KDFont::SmallFont->stringSize(label);
    ctx->drawString(label, KDPoint(rect.x() + (rect.width() - size.width()) / 2, rect.y() + (k_tabHeight - size.height()) / 2), KDFont::SmallFont, active ? k_text : k_secondaryText, bg);
    if (active) {
      // Underline the active tab
      ctx->fillRect(KDRect(rect.x() + 8, rect.bottom() - 3, rect.width() - 16, 3), k_text);
    }
  }
  ctx->fillRect(KDRect(0, k_listTop - 1, k_width, 1), k_secondaryText);
}

void FileManager::drawList() {
  KDContext * ctx = KDIonContext::sharedContext();
  const KDFont * font = KDFont::LargeFont;
  const KDFont * small = KDFont::SmallFont;
  KDCoordinate rowHeight = font->glyphSize().height() + 6;
  int rows = (k_listBottom - k_listTop) / rowHeight;
  int s = (int)m_side;

  ctx->fillRect(KDRect(0, k_listTop, k_width, k_listBottom - k_listTop), k_background);

  if (m_count[s] == 0) {
    KDCoordinate y = k_listTop + 12;
    KDCoordinate line = small->glyphSize().height() + 4;
    if (m_side == Side::Kindle) {
      if (!m_folderOk) {
        drawText("Can't open the folder:", 10, y, k_width - 20, small, k_text, k_background);
      } else {
        drawText("No Python scripts (.py) in", 10, y, k_width - 20, small, k_text, k_background);
      }
      drawText(m_folder, 10, y + line, k_width - 20, small, k_text, k_background);
      drawText("Copy .py files there over USB, then", 10, y + 3 * line, k_width - 20, small, k_secondaryText, k_background);
      drawText("come back to this screen.", 10, y + 4 * line, k_width - 20, small, k_secondaryText, k_background);
      drawText("Books (.txt) in that folder open", 10, y + 6 * line, k_width - 20, small, k_secondaryText, k_background);
      drawText("directly in the Reader app.", 10, y + 7 * line, k_width - 20, small, k_secondaryText, k_background);
    } else {
      drawText("No Python scripts in the calculator.", 10, y, k_width - 20, small, k_text, k_background);
    }
    return;
  }

  int & scroll = m_scroll[s];
  int selectedRow = m_selected[s];
  if (selectedRow < scroll) {
    scroll = selectedRow;
  } else if (selectedRow >= scroll + rows) {
    scroll = selectedRow - rows + 1;
  }

  for (int i = 0; i < rows && scroll + i < m_count[s]; i++) {
    const Entry & e = m_entries[s][scroll + i];
    bool isSelected = scroll + i == selectedRow;
    KDColor bg = isSelected ? k_bar : k_background;
    KDColor fg = isSelected ? k_barText : k_text;
    KDCoordinate y = k_listTop + i * rowHeight;
    ctx->fillRect(KDRect(0, y, k_width, rowHeight), bg);

    char size[16];
    if (e.size < 10000) {
      snprintf(size, sizeof(size), "%u B", (unsigned)e.size);
    } else {
      snprintf(size, sizeof(size), "%u KB", (unsigned)((e.size + 512) / 1024));
    }
    KDSize sizeSize = small->stringSize(size);
    KDCoordinate sizeX = k_width - 8 - sizeSize.width();
    ctx->drawString(size, KDPoint(sizeX, y + (rowHeight - sizeSize.height()) / 2), small, isSelected ? k_barText : k_secondaryText, bg);
    drawText(e.name, 8, y + 3, sizeX - 16, font, fg, bg);
    if (!isSelected) {
      ctx->fillRect(KDRect(0, y + rowHeight - 1, k_width, 1), Palette::BackgroundAppsSecondary);
    }
  }

  // Scroll indicator
  if (m_count[s] > rows) {
    KDCoordinate height = k_listBottom - k_listTop;
    KDCoordinate barHeight = height * rows / m_count[s];
    KDCoordinate barY = k_listTop + height * scroll / m_count[s];
    ctx->fillRect(KDRect(k_width - 3, barY, 3, barHeight), k_secondaryText);
  }
}

void FileManager::drawFooter() {
  KDContext * ctx = KDIonContext::sharedContext();
  KDRect rect(0, k_listBottom, k_width, k_footerHeight);
  ctx->fillRect(rect, Palette::BackgroundAppsSecondary);
  ctx->fillRect(KDRect(0, k_listBottom, k_width, 1), k_secondaryText);
  const char * text;
  char hint[96];
  KDColor fg = k_text;
  if (m_message[0] != 0) {
    text = m_message;
    fg = m_messageIsError ? k_text : k_secondaryText;
  } else if (count() == 0) {
    text = "Left/Right: switch list   Back: leave";
    fg = k_secondaryText;
  } else if (m_side == Side::Kindle) {
    snprintf(hint, sizeof(hint), "OK: copy to calculator   Back: leave");
    text = hint;
    fg = k_secondaryText;
  } else {
    snprintf(hint, sizeof(hint), "OK: copy to Kindle   DEL: delete");
    text = hint;
    fg = k_secondaryText;
  }
  drawText(text, 6, k_listBottom + (k_footerHeight - KDFont::SmallFont->glyphSize().height()) / 2 + 1, k_width - 12, KDFont::SmallFont, fg, Palette::BackgroundAppsSecondary);
}

void FileManager::draw() {
  KDIonContext * ctx = KDIonContext::sharedContext();
  ctx->setOrigin(KDPointZero);
  ctx->setClippingRect(KDRect(0, 0, k_width, k_height));
  drawHeader();
  drawTabs();
  drawList();
  drawFooter();
}

/* ---- Actions ---- */

void FileManager::setMessage(const char * format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(m_message, sizeof(m_message), format, args);
  va_end(args);
}

void FileManager::moveSelection(int delta) {
  int s = (int)m_side;
  if (m_count[s] == 0) {
    return;
  }
  int n = m_selected[s] + delta;
  if (n < 0) {
    n = (delta == -1) ? m_count[s] - 1 : 0;
  } else if (n >= m_count[s]) {
    n = (delta == 1) ? 0 : m_count[s] - 1;
  }
  m_selected[s] = n;
}

/* Python app rule for script names: [a-z_][a-z0-9_]*.py */
void compliantName(const char * fileName, char * out, size_t size) {
  size_t length = strlen(fileName) - strlen(k_extension) - 1; // without ".py"
  size_t j = 0;
  for (size_t i = 0; i < length && j + 1 < size && j < (size_t)k_maxNameLength; i++) {
    char c = fileName[i];
    if (c >= 'A' && c <= 'Z') {
      c = c - 'A' + 'a';
    }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      c = '_';
    }
    if (j == 0 && c >= '0' && c <= '9' && j + 2 < size) {
      out[j++] = '_';
    }
    out[j++] = c;
  }
  if (j == 0 && j + 1 < size) {
    out[j++] = '_';
  }
  out[j] = 0;
  snprintf(out + j, size - j, ".%s", k_extension);
}

void FileManager::activate() {
  if (count() == 0) {
    return;
  }
  if (m_side == Side::Kindle) {
    importSelected(false);
  } else {
    exportSelected(false);
  }
}

void FileManager::confirm() {
  Pending pending = m_pending;
  m_pending = Pending::None;
  m_message[0] = 0;
  switch (pending) {
    case Pending::ImportReplace: importSelected(true); break;
    case Pending::ExportReplace: exportSelected(true); break;
    case Pending::Delete: deleteSelected(); break;
    default: break;
  }
}

void FileManager::importSelected(bool replace) {
  m_messageIsError = true;
  if (GlobalPreferences::sharedGlobalPreferences()->isInExamMode()) {
    setMessage("Not available in exam mode");
    return;
  }
  const Entry & e = selected();
  char target[sizeof(Entry::name)];
  compliantName(e.name, target, sizeof(target));
  Ion::Storage * storage = Ion::Storage::sharedStorage();
  Ion::Storage::Record existing = storage->recordNamed(target);
  if (!replace && !existing.isNull()) {
    m_pending = Pending::ImportReplace;
    strlcpy(m_pendingName, e.name, sizeof(m_pendingName));
    m_messageIsError = true;
    setMessage("Replace %s? OK: yes  Back: no", target);
    return;
  }
  if (e.size > k_maxFileSize) {
    setMessage("Too big for the calculator (%u KB)", (unsigned)(e.size / 1024));
    return;
  }

  char path[512];
  kindlePath(e.name, path, sizeof(path));
  FILE * f = fopen(path, "rb");
  if (f == nullptr) {
    setMessage("Can't read %s: %s", e.name, strerror(errno));
    return;
  }
  // Status byte, source with \r removed and tabs expanded (4x), terminator
  size_t capacity = 1 + 4 * e.size + 1;
  char * value = static_cast<char *>(malloc(capacity));
  if (value == nullptr) {
    fclose(f);
    setMessage("Out of memory");
    return;
  }
  size_t length = 0;
  value[length++] = 1; // auto-import in the Python shell, like new scripts
  int c;
  while ((c = fgetc(f)) != EOF && length + 5 < capacity) {
    if (c == '\r' || c == 0) {
      continue;
    }
    if (c == '\t') {
      for (int i = 0; i < 4; i++) {
        value[length++] = ' ';
      }
      continue;
    }
    value[length++] = c;
  }
  fclose(f);
  value[length++] = 0;

  size_t available = storage->availableSize() + (existing.isNull() ? 0 : existing.value().size);
  if (length > available) {
    free(value);
    setMessage("Calculator memory full (needs %u B more)", (unsigned)(length - available));
    return;
  }

  Ion::Storage::Record::ErrorStatus status;
  if (!existing.isNull()) {
    existing.destroy();
  }
  status = storage->createRecordWithFullName(target, value, length);
  free(value);
  if (status != Ion::Storage::Record::ErrorStatus::None) {
    setMessage(status == Ion::Storage::Record::ErrorStatus::NotEnoughSpaceAvailable ? "Calculator memory full" : "Couldn't save %s", target);
    reload();
    return;
  }
  m_messageIsError = false;
  if (strcmp(target, e.name) != 0) {
    setMessage("Copied as %s", target);
  } else {
    setMessage("Copied %s to the calculator", target);
  }
  reload();
}

void FileManager::exportSelected(bool replace) {
  m_messageIsError = true;
  const Entry & e = selected();
  char path[512];
  kindlePath(e.name, path, sizeof(path));
  struct stat st;
  if (!replace && stat(path, &st) == 0) {
    m_pending = Pending::ExportReplace;
    strlcpy(m_pendingName, e.name, sizeof(m_pendingName));
    setMessage("Replace %s on the Kindle? OK: yes", e.name);
    return;
  }
  Ion::Storage::Record r = Ion::Storage::sharedStorage()->recordNamed(e.name);
  if (r.isNull()) {
    setMessage("%s is gone", e.name);
    reload();
    return;
  }
  Ion::Storage::Record::Data data = r.value();
  size_t length = scriptLength(data);
  const char * source = static_cast<const char *>(data.buffer) + 1;

  // Write to a temporary file, then rename, so a full disk can't leave half
  // a script behind
  char temp[520];
  snprintf(temp, sizeof(temp), "%s.part", path);
  FILE * f = fopen(temp, "wb");
  if (f == nullptr) {
    setMessage("Can't write to the Kindle: %s", strerror(errno));
    return;
  }
  bool ok = fwrite(source, 1, length, f) == length;
  if (ok && length > 0 && source[length - 1] != '\n') {
    ok = fputc('\n', f) != EOF;
  }
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(temp, path) != 0) {
    int error = errno;
    unlink(temp);
    setMessage("Couldn't write %s: %s", e.name, strerror(error));
    return;
  }
  m_messageIsError = false;
  setMessage("Copied %s to the Kindle", e.name);
  reload();
}

void FileManager::deleteSelected() {
  const Entry & e = selected();
  Ion::Storage::Record r = Ion::Storage::sharedStorage()->recordNamed(e.name);
  if (!r.isNull()) {
    r.destroy();
  }
  m_messageIsError = false;
  setMessage("Deleted %s from the calculator", e.name);
  reload();
}

bool FileManager::handle(Ion::Events::Event event) {
  if (event == Ion::Events::Termination) {
    return false;
  }
  if (m_pending != Pending::None) {
    // The selection may have moved under a pending question: only confirm
    // for the entry it was asked about
    if ((event == Ion::Events::OK || event == Ion::Events::EXE) && count() > 0 && strcmp(selected().name, m_pendingName) == 0) {
      confirm();
    } else {
      m_pending = Pending::None;
      m_message[0] = 0;
    }
    return true;
  }
  m_message[0] = 0;
  if (event == Ion::Events::Back || event == Ion::Events::Home) {
    return false;
  } else if (event == Ion::Events::Left || event == Ion::Events::Right) {
    m_side = m_side == Side::Kindle ? Side::Calculator : Side::Kindle;
    reload(); // pick up files copied over USB meanwhile
  } else if (event == Ion::Events::Up) {
    moveSelection(-1);
  } else if (event == Ion::Events::Down) {
    moveSelection(1);
  } else if (event == Ion::Events::ShiftUp) {
    moveSelection(-8);
  } else if (event == Ion::Events::ShiftDown) {
    moveSelection(8);
  } else if (event == Ion::Events::OK || event == Ion::Events::EXE) {
    activate();
  } else if (event == Ion::Events::Backspace && m_side == Side::Calculator && count() > 0) {
    m_pending = Pending::Delete;
    strlcpy(m_pendingName, selected().name, sizeof(m_pendingName));
    m_messageIsError = true;
    setMessage("Delete %s? OK: yes  Back: no", selected().name);
  }
  return true;
}

void FileManager::run() {
  m_pending = Pending::None;
  m_message[0] = 0;
  reload();
  draw();
  for (;;) {
    int timeout = 60000;
    Ion::Events::Event event = Ion::Events::getEvent(&timeout);
    if (event == Ion::Events::None || event == Ion::Events::Shift || event == Ion::Events::Alpha) {
      continue;
    }
    if (!handle(event)) {
      return;
    }
    draw();
  }
}

}

extern "C" void extapp_main() {
  // ~16 KB of lists: keep it off the stack
  static FileManager * manager = nullptr;
  if (manager == nullptr) {
    manager = new FileManager();
  }
  manager->run();
}
