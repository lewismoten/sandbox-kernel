# SPDX-License-Identifier: MIT
# Sandbox JS: JS.ELF for the board, and jsrun for the host (the same runtime over
# stdio). Built only against this repository's SDK (../sdk), QuickJS-ng in
# third_party/quickjs-ng and the toolchain's newlib. Usage (from js/ or `make -C js`):
#   make            build/JS.ELF (stripped) and build/JS.full.elf
#   make host       build/host/jsrun
#   make test       the host tests and fixtures (tests/run.py)
#   make size       the ELF and PT_LOAD size report against 1 MiB and 8 MiB
#   make repro      build twice into scratch folders, fail unless byte-identical
# BUILD= moves the output folder. JS_GAME=1 (default 0) would add the <game>
# element; it needs gamemaker-os's play core, which is not imported (slice 2).
TOOLCHAIN ?= /Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-
CC := $(TOOLCHAIN)gcc
STRIP := $(TOOLCHAIN)strip
SIZE := $(TOOLCHAIN)size
HOSTCC ?= cc
PYTHON ?= python3
BUILD ?= build
JS_GAME ?= 0
SDK := ../sdk
WARN := -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wmisleading-indentation -Wimplicit-fallthrough -Wformat=2 -Wmissing-prototypes -Wstrict-prototypes
QJS := quickjs libregexp libunicode dtoa
# -ffile-prefix-map and --build-id=none keep the bytes independent of the checkout path.
ARMF := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -Os -ffunction-sections -fdata-sections -ffile-prefix-map=$(CURDIR)=.
QJSFLAGS := $(ARMF) -D__DJGPP -DJS_HAVE_THREADS=0 -DNDEBUG -Wno-implicit-function-declaration -w
JSFLAGS := $(ARMF) -std=gnu11 $(WARN) -Wno-main -DJS_GAME=$(JS_GAME) -I$(SDK)/include -Isrc -isystem third_party/quickjs-ng
LFLAGS := -nostartfiles -T$(SDK)/lib/sandbox.ld -Wl,-z,noexecstack -Wl,--build-id=none -Wl,-z,max-page-size=4096 -Wl,--gc-sections
JSC := src/jsrt.c src/dom.c src/gif.c src/font_data.c
JSBOARD := src/main.c src/plat_sbx.c src/sys.c
HOSTDIR := $(BUILD)/host
.PHONY: all host test size repro clean
all: $(BUILD)/JS.ELF
$(BUILD) $(HOSTDIR) $(BUILD)/q:
	mkdir -p $@
$(BUILD)/prelude.c: src/prelude.js tools/embed_js.py | $(BUILD)
	$(PYTHON) tools/embed_js.py $< $@ jsrt_prelude
$(BUILD)/picker.c: src/picker.js tools/embed_js.py | $(BUILD)
	$(PYTHON) tools/embed_js.py $< $@ jsrt_picker
$(BUILD)/q/%.o: third_party/quickjs-ng/%.c $(wildcard third_party/quickjs-ng/*.h) Makefile | $(BUILD)/q
	$(CC) $(QJSFLAGS) -c $< -o $@
# Objects are compiled one by one: a single compile-and-link command would put the driver's
# random temporary object names into the symbol table and the bytes would differ per build.
BOBJ := $(BUILD)/o/crt0.o $(BUILD)/o/prelude.o $(BUILD)/o/picker.o $(patsubst src/%.c,$(BUILD)/o/%.o,$(JSBOARD) $(JSC))
$(BUILD)/o:
	mkdir -p $@
$(BUILD)/o/crt0.o: $(SDK)/lib/crt0.S Makefile | $(BUILD)/o
	$(CC) $(JSFLAGS) -c $< -o $@
$(BUILD)/o/%.o: src/%.c $(wildcard src/*.h) Makefile | $(BUILD)/o
	$(CC) $(JSFLAGS) -c $< -o $@
$(BUILD)/o/%.o: $(BUILD)/%.c Makefile | $(BUILD)/o
	$(CC) $(JSFLAGS) -c $< -o $@
$(BUILD)/JS.full.elf: $(BOBJ) $(QJS:%=$(BUILD)/q/%.o) $(SDK)/lib/sandbox.ld Makefile
	$(CC) $(JSFLAGS) $(LFLAGS) $(BOBJ) $(QJS:%=$(BUILD)/q/%.o) -lm -lc -lgcc -o $@
$(BUILD)/JS.ELF: $(BUILD)/JS.full.elf
	$(STRIP) -o $@ $<
# The runtime on the host: tests/host/jsrun.c supplies plat.h over stdio.
host: $(HOSTDIR)/jsrun
$(HOSTDIR)/js_prelude.c: src/prelude.js tools/embed_js.py | $(HOSTDIR)
	$(PYTHON) tools/embed_js.py $< $@ jsrt_prelude
$(HOSTDIR)/js_picker.c: src/picker.js tools/embed_js.py | $(HOSTDIR)
	$(PYTHON) tools/embed_js.py $< $@ jsrt_picker
$(HOSTDIR)/q_%.o: third_party/quickjs-ng/%.c $(wildcard third_party/quickjs-ng/*.h) Makefile | $(HOSTDIR)
	$(HOSTCC) -O1 -D_GNU_SOURCE -DNDEBUG -w -c $< -o $@
$(HOSTDIR)/jsrun: tests/host/jsrun.c $(JSC) $(HOSTDIR)/js_prelude.c $(HOSTDIR)/js_picker.c $(QJS:%=$(HOSTDIR)/q_%.o) $(wildcard src/*.h) Makefile | $(HOSTDIR)
	$(HOSTCC) -O1 -std=gnu11 $(WARN) -DJS_GAME=$(JS_GAME) -Isrc -isystem third_party/quickjs-ng tests/host/jsrun.c $(JSC) $(HOSTDIR)/js_prelude.c $(HOSTDIR)/js_picker.c $(QJS:%=$(HOSTDIR)/q_%.o) -lm -o $@
test: host
	$(PYTHON) tests/run.py $(HOSTDIR)/jsrun
size: $(BUILD)/JS.ELF
	$(SIZE) $(BUILD)/JS.full.elf
	$(PYTHON) tools/size_report.py $(BUILD)/JS.ELF
repro:
	$(PYTHON) tools/repro.py
clean:
	rm -rf $(BUILD)
