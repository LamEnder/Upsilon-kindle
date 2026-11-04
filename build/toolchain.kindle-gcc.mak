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

# Cortex-A9 (i.MX6) and later, hard-float ABI
SFLAGS += -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pie
SFLAGS += -ffunction-sections -fdata-sections

# Fully static: runs regardless of the glibc shipped by the firmware
LDFLAGS += -static -no-pie -Wl,--gc-sections
