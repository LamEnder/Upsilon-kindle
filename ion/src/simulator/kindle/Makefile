ion_src += $(addprefix ion/src/simulator/kindle/, \
  battery.cpp \
  clipboard.cpp \
  console.cpp \
  eink.cpp \
  events.cpp \
  input.cpp \
  keypad.cpp \
  main.cpp \
  persistence.cpp \
  preferences.cpp \
  screen.cpp \
  timing.cpp \
  touch.cpp \
)

ion_src += ion/src/shared/collect_registers.cpp

# SDL-based pieces of the generic simulator, replaced by the files above
kindle_replaced_src += $(addprefix ion/src/simulator/shared/, \
  actions.cpp \
  clipboard.cpp \
  console.cpp \
  display.cpp \
  events.cpp \
  events_platform.cpp \
  framebuffer.cpp \
  keyboard.cpp \
  layout.cpp \
  main.cpp \
  state_file.cpp \
  timing.cpp \
  window.cpp \
)
kindle_replaced_src += ion/src/shared/dummy/battery.cpp

ion_src := $(filter-out $(kindle_replaced_src),$(ion_src))

SFLAGS := $(filter-out -Iion/src/simulator/external/sdl/include,$(SFLAGS))

# preferences.cpp reads the apps' settings objects, which need the generated
# i18n header.
$(call object_for,ion/src/simulator/kindle/preferences.cpp): $(BUILD_DIR)/apps/i18n.h
