# MPC TIC-80 Runtime & Player for Akai MPC Sample.
# `make` cross-builds build/mpc-tic80 (aarch64-linux-musl static binary),
# then packs build/autorun/ and build/mpc-tic80.zip.
# `make test` builds and runs the host unit test suite.
# `make deploy` uploads the zip to the MPC via SSH / mpc-boot-thingy.

WARN = -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE
HOSTCC = cc
ZIGCC = zig cc -target aarch64-linux-musl

UI = mpc-ui-toolkit/src
BUILD = build
APP = $(BUILD)/mpc-tic80
PACK = mpc-ui-toolkit/tools/pack-autorun.sh
ZIP = $(BUILD)/mpc-tic80.zip

HOST_TIC = $(BUILD)/host/tic80
DEV_TIC = $(BUILD)/device/tic80

INCLUDES = -I src -I $(UI) -I TIC-80/include -I TIC-80/src -I TIC-80/vendor/lua

HOST_CFLAGS = $(WARN) -O2 -g $(INCLUDES)
DEV_CFLAGS = $(WARN) -O2 $(INCLUDES)

SRC_APP = src/tic_runner.c src/input_map.c src/audio_engine.c src/ui_overlay.c src/config.c src/cJSON.c src/savestate.c
UI_COMMON_SRC = $(UI)/ui.c $(UI)/widgets.c $(UI)/filebrowser.c $(UI)/input.c
UI_DEVICE_SRC = $(UI)/app.c $(UI)/screen.c $(UI)/surface.c $(UI_COMMON_SRC)

TIC_LIBS = -L$$libdir -ltic80core -llua -lluaapi -lpng -lzlib -lblipbuf -lgiflib -lm -pthread

.PHONY: all test tic80-host tic80-device deploy clean

all: $(ZIP)

# -----------------------------------------------------------------------------
# Host TIC-80 & Unit Tests
# -----------------------------------------------------------------------------
tic80-host:
	mkdir -p $(HOST_TIC)
	cmake -S TIC-80 -B $(HOST_TIC) -DCMAKE_BUILD_TYPE=Release \
	      -DBUILD_SDL=OFF -DBUILD_SOKOL=OFF -DBUILD_PLAYER=OFF \
	      -DBUILD_STATIC=ON -DBUILD_WITH_LUA=ON
	cmake --build $(HOST_TIC) --target tic80core lua luaapi zlib png blipbuf giflib -j

UNAME_S := $(shell uname -s)

# macOS 26.4+ deadlocks in AddressSanitizer init. The shim feeds dyld's
# header through a getter that does not allocate. Other hosts run as-is.
ifeq ($(UNAME_S),Darwin)
ASAN_SHIM = $(BUILD)/host/asan_dyld_shim.dylib
TEST_ENV = DYLD_INSERT_LIBRARIES=$(CURDIR)/$(ASAN_SHIM)
$(ASAN_SHIM): test/asan_dyld_shim.c | $(BUILD)/host
	$(HOSTCC) -dynamiclib -fblocks -Wl,-U,__dyld_get_dyld_header -o $@ test/asan_dyld_shim.c
else
ASAN_SHIM =
TEST_ENV =
endif

$(BUILD)/host/test-runner: tic80-host $(SRC_APP) $(UI_COMMON_SRC) test/test_runner.c $(ASAN_SHIM) | $(BUILD)/host
	mkdir -p $(BUILD)/host/obj
	libdir=$(CURDIR)/$(HOST_TIC)/lib; \
	$(HOSTCC) $(HOST_CFLAGS) -fsanitize=address,undefined -o $@ \
	  test/test_runner.c $(SRC_APP) $(UI_COMMON_SRC) \
	  -L$$libdir -ltic80core -llua -lluaapi -lpng -lzlib -lblipbuf -lgiflib -lm -pthread

test: $(BUILD)/host/test-runner
	$(TEST_ENV) ./$(BUILD)/host/test-runner

# -----------------------------------------------------------------------------
# Device Cross-Compilation (aarch64-linux-musl)
# -----------------------------------------------------------------------------
tic80-device: cmake/zig-cc cmake/zig-cxx cmake/zig-ar cmake/zig-ranlib
	chmod +x cmake/zig-cc cmake/zig-cxx cmake/zig-ar cmake/zig-ranlib
	mkdir -p $(DEV_TIC)
	cmake -S TIC-80 -B $(DEV_TIC) -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/aarch64-linux-musl.cmake \
	      -DBUILD_SDL=OFF -DBUILD_SOKOL=OFF -DBUILD_PLAYER=OFF \
	      -DBUILD_STATIC=ON -DBUILD_WITH_LUA=ON
	cmake --build $(DEV_TIC) --target tic80core lua luaapi zlib png blipbuf giflib -j

$(APP): tic80-device src/main.c $(SRC_APP) $(UI_DEVICE_SRC) | $(BUILD)
	mkdir -p $(BUILD)/device/obj
	libdir=$(CURDIR)/$(DEV_TIC)/lib; \
	for s in src/main.c $(SRC_APP) $(UI_DEVICE_SRC); do \
	  obj=$(BUILD)/device/obj/$$(basename $$s .c).o; \
	  $(ZIGCC) $(DEV_CFLAGS) -c -o $$obj $$s; \
	done; \
	$(ZIGCC) -static -O2 -s -o $@ $(BUILD)/device/obj/*.o \
	  -L$$libdir -ltic80core -llua -lluaapi -lpng -lzlib -lblipbuf -lgiflib -lm -pthread

$(BUILD) $(BUILD)/host:
	mkdir -p $@

$(ZIP): $(APP) $(PACK)
	$(PACK) --name mpc-tic80 --bin $(APP) --out $(BUILD)

deploy: $(ZIP)
	../mpc-boot-thingy/setup.sh --ssh --add $(ZIP) --title "mpc-tic80"

clean:
	rm -rf build bin
