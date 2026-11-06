#include "persistence.h"
#include "debug.h"

#include <ion/storage.h>
#include <ion/timing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr uint32_t k_magic = 0x4B535055; // "UPSK"
constexpr uint32_t k_version = 1;
constexpr uint64_t k_autosaveIntervalMs = 5000;

char sPath[512] = "";
uint64_t sFingerprint = 0;
uint32_t sSavedChecksum = 0;
uint8_t * sSavedPrefs = nullptr;
size_t sPrefsSize = 0;
uint64_t sLastCheck = 0;

uint64_t computeFingerprint() {
  uint64_t hash = 1469598103934665603ULL; // FNV-1a
  FILE * f = fopen("/proc/self/exe", "rb");
  if (f == nullptr) {
    return 0;
  }
  static uint8_t buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    for (size_t i = 0; i < n; i++) {
      hash ^= buffer[i];
      hash *= 1099511628211ULL;
    }
  }
  fclose(f);
  return hash;
}

bool hasSafeExtension(const char * name) {
  const char * dot = strrchr(name, '.');
  if (dot == nullptr) {
    return false;
  }
  return strcmp(dot, ".py") == 0 || strcmp(dot, ".func") == 0 || strcmp(dot, ".seq") == 0;
}

template <typename T>
bool readValue(FILE * f, T * value) {
  return fread(value, sizeof(T), 1, f) == 1;
}

template <typename T>
void writeValue(FILE * f, T value) {
  fwrite(&value, sizeof(T), 1, f);
}

void rememberCurrentState() {
  sSavedChecksum = Ion::Storage::sharedStorage()->checksum();
  if (sSavedPrefs != nullptr) {
    Ion::Kindle::Preferences::save(sSavedPrefs);
  }
}

}

namespace Ion {
namespace Kindle {
namespace Persistence {

void setPath(const char * path) {
  strncpy(sPath, path, sizeof(sPath) - 1);
  sPath[sizeof(sPath) - 1] = 0;
}

void load() {
  uint64_t start = Ion::Timing::millis();
  sFingerprint = computeFingerprint();
  Ion::Kindle::Debug::log("binary fingerprint computed in %llu ms", (unsigned long long)(Ion::Timing::millis() - start));
  sPrefsSize = Preferences::size();
  sSavedPrefs = static_cast<uint8_t *>(calloc(1, sPrefsSize));
  Ion::Storage * storage = Ion::Storage::sharedStorage();

  FILE * f = sPath[0] ? fopen(sPath, "rb") : nullptr;
  if (f == nullptr) {
    rememberCurrentState();
    return;
  }
  uint32_t magic = 0, version = 0, prefsSize = 0, count = 0;
  uint64_t fingerprint = 0;
  if (!readValue(f, &magic) || magic != k_magic || !readValue(f, &version) || version != k_version
      || !readValue(f, &fingerprint) || !readValue(f, &prefsSize)) {
    Ion::Kindle::Debug::log("%s is not a valid state file, ignoring it", sPath);
    fclose(f);
    rememberCurrentState();
    return;
  }
  bool sameBinary = fingerprint == sFingerprint && fingerprint != 0;
  if (!sameBinary) {
    Ion::Kindle::Debug::log("state was saved by another build, restoring scripts, functions and sequences only");
  }
  uint8_t * prefs = static_cast<uint8_t *>(malloc(prefsSize ? prefsSize : 1));
  bool prefsOk = fread(prefs, 1, prefsSize, f) == prefsSize;
  if (prefsOk && sameBinary && prefsSize == sPrefsSize) {
    Preferences::load(prefs, prefsSize);
  }
  free(prefs);

  int restored = 0;
  if (prefsOk && readValue(f, &count)) {
    for (uint32_t i = 0; i < count; i++) {
      uint16_t nameLength = 0;
      uint32_t size = 0;
      char name[256];
      if (!readValue(f, &nameLength) || nameLength >= sizeof(name)
          || fread(name, 1, nameLength, f) != nameLength || !readValue(f, &size)
          || size > Ion::Storage::k_storageSize) {
        Ion::Kindle::Debug::log("state file is truncated");
        break;
      }
      name[nameLength] = 0;
      void * data = malloc(size ? size : 1);
      if (fread(data, 1, size, f) != size) {
        free(data);
        Ion::Kindle::Debug::log("state file is truncated");
        break;
      }
      if (sameBinary || hasSafeExtension(name)) {
        Ion::Storage::Record existing = storage->recordNamed(name);
        if (!existing.isNull()) {
          existing.destroy();
        }
        if (storage->createRecordWithFullName(name, data, size) == Ion::Storage::Record::ErrorStatus::None) {
          restored++;
        }
      }
      free(data);
    }
  }
  fclose(f);
  Ion::Kindle::Debug::log("restored %d records from %s", restored, sPath);
  rememberCurrentState();
}

void save() {
  if (sPath[0] == 0) {
    return;
  }
  uint64_t start = Ion::Timing::millis();
  char tmpPath[sizeof(sPath) + 8];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", sPath);
  FILE * f = fopen(tmpPath, "wb");
  if (f == nullptr) {
    Ion::Kindle::Debug::log("cannot write %s", tmpPath);
    return;
  }
  writeValue<uint32_t>(f, k_magic);
  writeValue<uint32_t>(f, k_version);
  writeValue<uint64_t>(f, sFingerprint);
  writeValue<uint32_t>(f, (uint32_t)sPrefsSize);
  uint8_t * prefs = static_cast<uint8_t *>(malloc(sPrefsSize ? sPrefsSize : 1));
  Preferences::save(prefs);
  fwrite(prefs, 1, sPrefsSize, f);
  free(prefs);

  Ion::Storage * storage = Ion::Storage::sharedStorage();
  int count = storage->numberOfRecords();
  writeValue<uint32_t>(f, (uint32_t)count);
  for (int i = 0; i < count; i++) {
    Ion::Storage::Record record = storage->recordAtIndex(i);
    const char * name = record.fullName();
    Ion::Storage::Record::Data value = record.value();
    uint16_t nameLength = strlen(name);
    writeValue<uint16_t>(f, nameLength);
    fwrite(name, 1, nameLength, f);
    writeValue<uint32_t>(f, (uint32_t)value.size);
    fwrite(value.buffer, 1, value.size, f);
  }
  fflush(f);
  fsync(fileno(f));
  bool ok = !ferror(f);
  fclose(f);
  if (ok && rename(tmpPath, sPath) == 0) {
    rememberCurrentState();
    uint64_t ms = Ion::Timing::millis() - start;
    if (ms >= 300) {
      Ion::Kindle::Debug::log("state saved in %llu ms (slow storage)", (unsigned long long)ms);
    } else {
      KINDLE_VLOG("state saved in %llu ms", (unsigned long long)ms);
    }
  } else {
    Ion::Kindle::Debug::log("failed to save state to %s", sPath);
    unlink(tmpPath);
  }
}

void autosave() {
  uint64_t now = Ion::Timing::millis();
  if (sPath[0] == 0 || sSavedPrefs == nullptr || now - sLastCheck < k_autosaveIntervalMs) {
    return;
  }
  sLastCheck = now;
  bool changed = Ion::Storage::sharedStorage()->checksum() != sSavedChecksum;
  if (!changed && sPrefsSize > 0) {
    uint8_t * current = static_cast<uint8_t *>(malloc(sPrefsSize));
    Preferences::save(current);
    changed = memcmp(current, sSavedPrefs, sPrefsSize) != 0;
    free(current);
  }
  if (changed) {
    save();
  }
}

}
}
}
