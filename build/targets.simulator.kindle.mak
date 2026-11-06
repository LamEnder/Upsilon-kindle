# make PLATFORM=simulator TARGET=kindle upsilon.zip
# builds a package to unzip at the root of the Kindle's storage:
#   upsilon/                         the calculator, launcher, settings, state
#   extensions/upsilon/              KUAL menu entry
#   documents/Upsilon.sh             Scriptlet (library entry)
#   koreader/plugins/upsilon.koplugin  KOReader plugin

KINDLE_PACKAGE_SRC = ion/src/simulator/kindle/package
KINDLE_PACKAGE_DIR = $(BUILD_DIR)/package
KINDLE_PACKAGE_FILES = $(shell find $(KINDLE_PACKAGE_SRC) -type f) ion/src/simulator/kindle/README.md

$(BUILD_DIR)/upsilon.zip: $(BUILD_DIR)/epsilon.$(EXE) $(KINDLE_PACKAGE_FILES)
	$(call rule_label,ZIP)
	$(Q) rm -rf $(KINDLE_PACKAGE_DIR) $@
	$(Q) mkdir -p $(KINDLE_PACKAGE_DIR)
	$(Q) cp -R $(KINDLE_PACKAGE_SRC)/. $(KINDLE_PACKAGE_DIR)/
	$(Q) mkdir -p $(KINDLE_PACKAGE_DIR)/upsilon/bin
	$(Q) cp ion/src/simulator/kindle/README.md $(KINDLE_PACKAGE_DIR)/upsilon/
	$(Q) $(STRIP) $(KINDLE_STRIP_FLAGS) -o $(KINDLE_PACKAGE_DIR)/upsilon/bin/upsilon $(BUILD_DIR)/epsilon.$(EXE)
ifeq ($(KINDLE_DIAGNOSTICS),0)
	$(Q) $(PYTHON) -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m["items"]=[i for i in m["items"] if "--debug" not in i["params"]]; json.dump(m,open(p,"w"),indent=2)' $(KINDLE_PACKAGE_DIR)/extensions/upsilon/menu.json
	$(Q) sed -i '/@diagnostics-begin/,/@diagnostics-end/d' $(KINDLE_PACKAGE_DIR)/koreader/plugins/upsilon.koplugin/main.lua
endif
	$(Q) chmod 755 $(KINDLE_PACKAGE_DIR)/upsilon/bin/upsilon $(KINDLE_PACKAGE_DIR)/upsilon/upsilon.sh $(KINDLE_PACKAGE_DIR)/upsilon/debug.sh $(KINDLE_PACKAGE_DIR)/documents/Upsilon.sh
	$(Q) cd $(KINDLE_PACKAGE_DIR) && zip -qr ../upsilon.zip upsilon extensions documents koreader

HANDY_TARGETS += upsilon
