SHELL := /bin/sh

.DEFAULT_GOAL := all

BUILD_DIR ?= build
CMAKE ?= cmake
CTEST ?= ctest
CONFIGURE_STAMP := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all configure clean rebuild_cache test editor

$(CONFIGURE_STAMP): CMakeLists.txt
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) -S . -B $(BUILD_DIR)

configure: $(CONFIGURE_STAMP)

all: $(CONFIGURE_STAMP)
	$(CMAKE) --build $(BUILD_DIR) --parallel

editor: $(CONFIGURE_STAMP)
	$(CMAKE) --build $(BUILD_DIR) --target editor --parallel

test: $(CONFIGURE_STAMP)
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure $(ARGS)

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

rebuild_cache: clean configure
