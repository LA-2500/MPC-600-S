# The MPC 600 S. Run from the repository root:
#   make -f os/Makefile           bin/mpc-os and bin/mpc-os.zip
#   make -f os/Makefile test      host tests
#   make -f os/Makefile desktop   build, then copy the zip to the Desktop folder
BUILD = build/os
BIN = bin
WARN = -Wall -Wextra -std=c11
FLAGS = -D_DEFAULT_SOURCE $(WARN) -I os -I os/stems
ZIGDEV = zig cc -target aarch64-linux-musl
ZIGHOST = zig cc

PANEL = os/panel.c os/files.c
COMMON = os/engine.c os/wav.c os/os.c os/lcd.c os/seq.c os/seq_screen.c os/s600.c os/record.c os/recorder.c os/zones.c os/undo.c os/eq.c os/midi.c os/stems/umx.c os/onset.c os/fx.c os/fxpage.c os/mute.c os/help.c os/master.c os/song.c os/step.c os/warp.c os/prefs.c os/disk.c os/arpedit.c os/multi.c
OS_HDR = $(wildcard os/*.h) $(wildcard os/stems/*.h)
DEV_SRC = $(COMMON) os/alsa_out.c os/power_key.c os/midi_out.c os/main.c $(PANEL)
TEST_SRC = $(COMMON) os/test.c $(PANEL)

# Where `make desktop` copies the zip: set DESKTOP in os/local.mk (not in git), or on the command line.
-include os/local.mk
DESKTOP ?= $(BUILD)/desktop

.PHONY: all test desktop

all: $(BIN)/mpc-os.zip

$(BIN)/mpc-os: $(DEV_SRC) $(OS_HDR)
	mkdir -p $(BIN)
	$(ZIGDEV) -static -O2 -s $(FLAGS) -o $@ $(DEV_SRC) -lm -lpthread

$(BIN)/mpc-os.zip: $(BIN)/mpc-os tools/make_autorun.py
	python3 tools/make_autorun.py mpc-os $< $(BUILD)/pkg
	cp $(BUILD)/pkg/mpc-os.zip $@

$(BUILD)/os-test: $(TEST_SRC) $(OS_HDR)
	mkdir -p $(BUILD)
	$(ZIGHOST) -g -O1 $(FLAGS) -o $@ $(TEST_SRC) -lm

test: $(BUILD)/os-test
	./$(BUILD)/os-test $(SHOT_DIR)

$(BIN)/ctl-dump: os/probe/ctl_dump.c
	mkdir -p $(BIN)
	$(ZIGDEV) -static -O2 -s $(WARN) -D_DEFAULT_SOURCE -o $@ $<

$(BIN)/mpc-probe.zip: $(BIN)/ctl-dump os/probe/mpc-probe tools/make_autorun.py
	python3 tools/make_autorun.py mpc-probe os/probe/mpc-probe $(BUILD)/probe $(BIN)/ctl-dump
	cp $(BUILD)/probe/mpc-probe.zip $@

desktop: $(BIN)/mpc-os.zip
	mkdir -p "$(DESKTOP)"
	cp $(BIN)/mpc-os.zip "$(DESKTOP)/mpc-os.zip"
