#include <ion/battery.h>
#include <ion/usb.h>
#include <stdio.h>
#include <string.h>

/* Upsilon's own status-bar battery, fed from the Kindle's power supply in
 * sysfs. The paths depend on the PMIC; the first one that answers wins. */

namespace {

const char * const k_capacityPaths[] = {
  "/sys/class/power_supply/bd71827_bat/capacity",   // PW4, KT4
  "/sys/class/power_supply/max77796-battery/capacity", // KOA2/KOA3
  "/sys/devices/system/wario_battery/wario_battery0/battery_capacity",
  "/sys/devices/system/yoshi_battery/yoshi_battery0/battery_capacity",
  "/sys/class/power_supply/battery/capacity",
};

const char * const k_statusPaths[] = {
  "/sys/class/power_supply/bd71827_bat/status",
  "/sys/class/power_supply/max77796-battery/status",
  "/sys/class/power_supply/battery/status",
};

const char * const k_chargerOnlinePaths[] = {
  "/sys/class/power_supply/bd71827_ac/online",
  "/sys/class/power_supply/max77796-charger/online",
  "/sys/class/power_supply/usb/online",
  "/sys/class/power_supply/ac/online",
};

bool readFile(const char * path, char * buffer, size_t size) {
  FILE * f = fopen(path, "r");
  if (f == nullptr) {
    return false;
  }
  size_t n = fread(buffer, 1, size - 1, f);
  fclose(f);
  buffer[n] = 0;
  return n > 0;
}

template <size_t N>
int readInt(const char * const (&paths)[N]) {
  char buffer[32];
  for (const char * path : paths) {
    if (readFile(path, buffer, sizeof(buffer))) {
      int value = -1;
      if (sscanf(buffer, "%d", &value) == 1 && value >= 0) {
        return value;
      }
    }
  }
  return -1;
}

int capacity() {
  int c = readInt(k_capacityPaths);
  return c > 100 ? 100 : c;
}

}

bool Ion::Battery::isCharging() {
  char buffer[32];
  for (const char * path : k_statusPaths) {
    if (readFile(path, buffer, sizeof(buffer))) {
      return strncmp(buffer, "Charging", 8) == 0;
    }
  }
  return false;
}

Ion::Battery::Charge Ion::Battery::level() {
  int c = capacity();
  if (c < 0 || c > 50) {
    return Charge::FULL;
  }
  if (c > 20) {
    return Charge::SOMEWHERE_INBETWEEN;
  }
  /* Never EMPTY: on a calculator, EMPTY makes Upsilon show its "empty
   * battery" screen and loop until USB power shows up, ignoring input. The
   * Kindle warns and shuts down by itself, so the lowest we report is LOW. */
  return Charge::LOW;
}

float Ion::Battery::voltage() {
  /* Settings > About shows this, and turns it into a percentage with
   * (voltage - 3.6) * 166. Map the Kindle's own percentage onto that scale so
   * both read like the Kindle's battery gauge. */
  int c = capacity();
  return c < 0 ? 4.2f : 3.6f + (c + 0.5f) / 166.0f;
}

/* The charger state, for the "full and plugged in" tick in the status bar.
 * The rest of USB (DFU, enumeration) doesn't exist here: the Kindle never
 * sends USB events, so the calculator never tries to switch to USB mode. */
namespace Ion {
namespace USB {

bool isPlugged() {
  return readInt(k_chargerOnlinePaths) > 0;
}

bool isEnumerated() {
  return false;
}

void clearEnumerationInterrupt() {
}

void DFU(bool, void *) {
}

void enable() {
}

void disable() {
}

}
}
