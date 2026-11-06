# Kindle e-readers running the "kindlehf" (ARMv7 hard-float) firmware,
# i.e. firmware 5.16.3 and later: Paperwhite 4, Basic 4, Oasis 2/3...
TOOLCHAIN = kindle-gcc
EXE = bin

EPSILON_TELEMETRY ?= 0

# KINDLE_DIAGNOSTICS=1 (default): watchdog, crash reports with backtraces,
#   verbose log on demand (see ion/src/simulator/kindle/debug.h).
# KINDLE_DIAGNOSTICS=0: "fast" build. Diagnostics compiled out, optimized for
#   speed (-O2 and link-time optimization) instead of size, fully stripped.
#   Errors are still logged. Built in its own directory (.../kindle-fast) so
#   objects from the two configurations never mix.
KINDLE_DIAGNOSTICS ?= 1

ifeq ($(KINDLE_DIAGNOSTICS),0)
BUILD_DIR := $(BUILD_DIR)-fast
ifneq ($(DEBUG),1)
SFLAGS := $(filter-out -Os,$(SFLAGS))
SFLAGS += -O2
endif
KINDLE_LTO ?= 1
else
KINDLE_LTO ?= 0
endif
SFLAGS += -DKINDLE_DIAGNOSTICS=$(KINDLE_DIAGNOSTICS)

HANDY_TARGETS_EXTENSIONS = zip

# Plain static executable: no PIE, so raw expression trees kept in the state
# file stay valid between runs of the same binary (see kindle/persistence.h).
SFLAGS := $(filter-out -fPIE, $(SFLAGS))

# Expressions in functions and sequences are stored as text, which keeps them
# loadable after an update.
SFLAGS += -DSTRING_STORAGE

# Grayscale-friendly theme unless one is given on the command line
ifeq ($(origin THEME_NAME),file)
THEME_NAME := kindle_eink
endif
