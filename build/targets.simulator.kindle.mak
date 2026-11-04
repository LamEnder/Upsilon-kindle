# make PLATFORM=simulator TARGET=kindle upsilon.zip
# builds a KUAL extension ready to unzip at the root of the Kindle's storage.

KINDLE_PACKAGE_DIR = $(BUILD_DIR)/package
KINDLE_EXTENSION_FILES = $(wildcard ion/src/simulator/kindle/extension/*) ion/src/simulator/kindle/README.md

$(BUILD_DIR)/upsilon.zip: $(BUILD_DIR)/epsilon.$(EXE) $(KINDLE_EXTENSION_FILES)
	$(call rule_label,ZIP)
	$(Q) rm -rf $(KINDLE_PACKAGE_DIR) $@
	$(Q) mkdir -p $(KINDLE_PACKAGE_DIR)/extensions/upsilon/bin
	$(Q) cp ion/src/simulator/kindle/extension/* ion/src/simulator/kindle/README.md $(KINDLE_PACKAGE_DIR)/extensions/upsilon/
	$(Q) $(STRIP) -o $(KINDLE_PACKAGE_DIR)/extensions/upsilon/bin/upsilon $(BUILD_DIR)/epsilon.$(EXE)
	$(Q) chmod 755 $(KINDLE_PACKAGE_DIR)/extensions/upsilon/bin/upsilon $(KINDLE_PACKAGE_DIR)/extensions/upsilon/upsilon.sh
	$(Q) cd $(KINDLE_PACKAGE_DIR) && zip -qr ../upsilon.zip extensions

HANDY_TARGETS += upsilon
