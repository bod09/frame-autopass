# Host targets (tests) build with the system compiler. Device targets are
# cross-compiled for the headset (aarch64, glibc) with the Zig toolchain
# fetched by scripts/fetch-deps.sh.

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
BUILD := build/host

ZIG := toolchain/zig-x86_64-linux-0.16.0/zig
# The headset runs glibc 2.39; target a little lower so an OS update that
# holds glibc back does not strand the binaries.
TARGET := aarch64-linux-gnu.2.35
DEV := build/aarch64
DEVCXX := $(ZIG) c++ -target $(TARGET)
DEVFLAGS := -std=c++17 -O2 -g -Wall -Wextra

OPENVR := third_party/openvr
OPENVR_SRC := $(addprefix $(OPENVR)/src/,openvr_api_public.cpp jsoncpp.cpp \
	vrcore/dirtools_public.cpp vrcore/envvartools_public.cpp vrcore/pathtools_public.cpp \
	vrcore/sharedlibtools_public.cpp vrcore/hmderrors_public.cpp \
	vrcore/vrpathregistry_public.cpp vrcore/strtools_public.cpp)
OPENVR_OBJ := $(patsubst $(OPENVR)/src/%.cpp,$(DEV)/openvr/%.o,$(OPENVR_SRC))
OPENVR_DEFS := -DVR_API_PUBLIC -DOPENVR_BUILD_STATIC -DLINUX -DPOSIX -DLINUXARM64 -DVRCORE_NO_PLATFORM

.PHONY: dist test device deploy clean

HOST_TESTS := test_light_policy test_sensors test_xrservice_log test_daemon_config test_passthrough_state

test: $(addprefix $(BUILD)/,$(HOST_TESTS))
	@for t in $(HOST_TESTS); do $(BUILD)/$$t || exit 1; done

$(BUILD)/test_light_policy: tests/test_light_policy.cpp src/light_policy.cpp src/light_policy.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_light_policy.cpp src/light_policy.cpp

$(BUILD)/test_sensors: tests/test_sensors.cpp src/sensors.cpp src/sensors.hpp src/xrservice_log.cpp src/xrservice_log.hpp src/light_policy.cpp src/light_policy.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_sensors.cpp src/sensors.cpp src/xrservice_log.cpp src/light_policy.cpp

$(BUILD)/test_xrservice_log: tests/test_xrservice_log.cpp src/xrservice_log.cpp src/xrservice_log.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_xrservice_log.cpp src/xrservice_log.cpp

$(BUILD)/test_daemon_config: tests/test_daemon_config.cpp src/daemon_config.cpp src/daemon_config.hpp src/light_policy.cpp src/sensors.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_daemon_config.cpp src/daemon_config.cpp src/light_policy.cpp

$(BUILD)/test_passthrough_state: tests/test_passthrough_state.cpp src/passthrough_state.cpp src/passthrough_state.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/test_passthrough_state.cpp src/passthrough_state.cpp

# Device builds. autopassd does not link SteamVR at all; only autopass does (for
# the brief background connection that performs a switch).
device: $(DEV)/autopassd $(DEV)/autopass

$(ZIG) $(OPENVR)/headers/openvr.h:
	scripts/fetch-deps.sh

$(DEV)/openvr/%.o: $(OPENVR)/src/%.cpp | $(ZIG)
	@mkdir -p $(dir $@)
	$(DEVCXX) -std=c++17 -O2 -w -Wno-nullability-completeness $(OPENVR_DEFS) -I$(OPENVR)/headers -I$(OPENVR)/src -I$(OPENVR)/src/vrcore -c -o $@ $<

$(DEV)/libopenvr_loader.a: $(OPENVR_OBJ)
	$(ZIG) ar rcs $@ $^

DAEMON_SRC := src/autopassd.cpp src/daemon_config.cpp src/app_paths.cpp src/passthrough_state.cpp \
	src/light_policy.cpp src/sensors.cpp src/xrservice_log.cpp
CTL_SRC := tools/autopass.cpp src/app_paths.cpp src/light_policy.cpp src/passthrough_state.cpp src/private_camera.cpp
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
RELEASE_FLAGS := -DAUTOPASS_VERSION='"$(VERSION)"' -std=c++17 -Os -Wall -Wextra -Wno-nullability-completeness -ffunction-sections -fdata-sections -Wl,--gc-sections -s

$(DEV)/autopassd: $(DAEMON_SRC) $(wildcard src/*.hpp) | $(ZIG)
	@mkdir -p $(DEV)
	$(DEVCXX) $(RELEASE_FLAGS) -Isrc -o $@ $(DAEMON_SRC)

$(DEV)/autopass: $(CTL_SRC) $(wildcard src/*.hpp) $(DEV)/libopenvr_loader.a
	$(DEVCXX) $(RELEASE_FLAGS) -DOPENVR_BUILD_STATIC -Isrc -isystem $(OPENVR)/headers -o $@ $(CTL_SRC) $(DEV)/libopenvr_loader.a -ldl

# Copies autopassd and autopass to the headset's install directory, replacing
# running binaries safely (rename). Does not install or start the unit:
# run `autopass install` on the headset for that.
deploy: $(DEV)/autopassd $(DEV)/autopass
	ssh frame 'mkdir -p ~/.local/share/frame-autopass'
	scp -q $(DEV)/autopassd frame:.local/share/frame-autopass/autopassd.new
	scp -q $(DEV)/autopass frame:.local/share/frame-autopass/autopass.new
	ssh frame 'cd ~/.local/share/frame-autopass && mv autopassd.new autopassd && mv autopass.new autopass'

# Release package: dist/frame-autopass-aarch64.tar.gz (+ .sha256) and
# dist/install.sh. Always a clean build, so VERSION is baked in.
dist:
	rm -rf dist $(DEV)/autopassd $(DEV)/autopass
	$(MAKE) $(DEV)/autopassd $(DEV)/autopass
	mkdir -p dist/frame-autopass
	cp $(DEV)/autopassd $(DEV)/autopass tools/autopass-settings tools/autopass.svg install.sh README.md LICENSE dist/frame-autopass/
	tar -C dist -czf dist/frame-autopass-aarch64.tar.gz frame-autopass
	cd dist && sha256sum frame-autopass-aarch64.tar.gz > frame-autopass-aarch64.tar.gz.sha256
	cp install.sh dist/install.sh
	rm -rf dist/frame-autopass

clean:
	rm -rf build dist
