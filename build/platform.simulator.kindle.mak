# Kindle e-readers running the "kindlehf" (ARMv7 hard-float) firmware,
# i.e. firmware 5.16.3 and later: Paperwhite 4, Basic 4, Oasis 2/3...
TOOLCHAIN = kindle-gcc
EXE = bin

EPSILON_TELEMETRY ?= 0

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
