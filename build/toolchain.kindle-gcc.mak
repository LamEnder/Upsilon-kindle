# Works with Debian/Ubuntu's gcc-arm-linux-gnueabihf (default) or with the
# koxtoolchain "kindlehf" toolchain:
#   make PLATFORM=simulator TARGET=kindle KINDLE_CROSS=arm-kindlehf-linux-gnueabihf-
KINDLE_CROSS ?= arm-linux-gnueabihf-

CC = $(KINDLE_CROSS)gcc
CXX = $(KINDLE_CROSS)g++
LD = $(KINDLE_CROSS)g++
GDB = gdb-multiarch
OBJCOPY = $(KINDLE_CROSS)objcopy
SIZE = $(KINDLE_CROSS)size
STRIP = $(KINDLE_CROSS)strip
HOSTCC = gcc
HOSTCXX = g++
PYTHON = python3

# ARMv7 with NEON, hard-float ABI. Override to tune for a specific core,
# e.g. KINDLE_CPU_FLAGS="-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
KINDLE_CPU_FLAGS ?= -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
SFLAGS += $(KINDLE_CPU_FLAGS) -fno-pie
SFLAGS += -ffunction-sections -fdata-sections

ifeq ($(KINDLE_DIAGNOSTICS),0)
# Everything goes: no symbols, no unwind tables beyond what C++ needs
KINDLE_STRIP_FLAGS = --strip-all
else
# Unwind tables everywhere (C code too) so the watchdog and crash handler can
# print backtraces; the symbol table stays in the binary to name functions
SFLAGS += -funwind-tables
KINDLE_STRIP_FLAGS = --strip-debug
endif

ifeq ($(KINDLE_LTO),1)
SFLAGS += -flto=auto
LDFLAGS += -flto=auto
endif

# Fully static: runs regardless of the glibc shipped by the firmware
LDFLAGS += -static -no-pie -Wl,--gc-sections -pthread
