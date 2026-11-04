#include <ion/battery.h>
#include <stdio.h>
#include <string.h>

/* Report the Kindle's own battery in Upsilon's status bar. The sysfs path
 * depends on the PMIC; try the known ones, fall back to "full". */

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

int capacity() {
  char buffer[32];
  for (const char * path : k_capacityPaths) {
    if (readFile(path, buffer, sizeof(buffer))) {
      int value = -1;
      if (sscanf(buffer, "%d", &value) == 1 && value >= 0) {
        return value;
      }
    }
  }
  return -1;
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
  if (c > 5) {
    return Charge::LOW;
  }
  return Charge::EMPTY;
}

float Ion::Battery::voltage() {
  int c = capacity();
  // Rough mapping of percentage to a Li-ion voltage, only used for display
  return c < 0 ? 4.2f : 3.6f + 0.006f * c;
}
