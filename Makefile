ROM_DIR ?= ../arculator-mac/roms/riscos311
GEN = build/gen
HOSTOBJ = build/host
TRACES = --trace trace/boot

.PHONY: rom gen host dc cdi mkdcdisc clean

# The ROM plus bundled apps. Rebuilt every time, as cheap as checking its inputs.
rom:
	python3 tools/mkapps.py $(ROM_DIR) build

gen: rom
	python3 tools/armxlat.py build/rom.bin $(GEN) $(TRACES) --seeds trace/seeds.txt --ram-words trace/ramwords.txt

XLAT_SRC = $(wildcard $(GEN)/xlat_*.c)
OV_SRC = $(wildcard $(GEN)/ov_*.c)
XLAT_HOST = $(patsubst $(GEN)/%.c,$(HOSTOBJ)/%.o,$(XLAT_SRC) $(OV_SRC))
HOSTCC = clang
HOSTFLAGS = -O1 -g -Isrc -DSTOP_ON_MISS -fno-strict-aliasing -Wno-unused-value -Wno-parentheses-equality

$(HOSTOBJ)/%.o: $(GEN)/%.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	@$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/mach.o: src/mach.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/fdc.o: src/fdc.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/ide.o: src/ide.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/hdformat.o: src/hdformat.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/main.o: src/host/main.c src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/net.o: src/net.c src/net.h src/print.h src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/net_sock.o: src/net_sock.c src/net.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/web.o: src/web.c src/web.h src/net.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/print.o: src/print.c src/print.h src/net.h src/web.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

$(HOSTOBJ)/net_host.o: src/host/net.c src/net.h src/print.h src/cpu.h
	@mkdir -p $(HOSTOBJ)
	$(HOSTCC) $(HOSTFLAGS) -c $< -o $@

host: build/archicast-host

build/archicast-host: $(XLAT_HOST) $(HOSTOBJ)/mach.o $(HOSTOBJ)/fdc.o $(HOSTOBJ)/ide.o $(HOSTOBJ)/hdformat.o $(HOSTOBJ)/main.o $(HOSTOBJ)/net.o $(HOSTOBJ)/net_sock.o $(HOSTOBJ)/net_host.o $(HOSTOBJ)/web.o $(HOSTOBJ)/print.o
	$(HOSTCC) -o $@ $^

# Harnesses with the ARM interpreter. archicast-interp falls back to it for
# RAM and unrecompiled code; archicast-interpall runs everything through it, ROM too.
# The Dreamcast build carries the ARM interpreter, which anything loaded off a
# disc runs through, including the whole software archive. INTERP= leaves it out.
INTERP ?= 1
INTERP_OBJ = $(filter-out $(HOSTOBJ)/mach.o,$(XLAT_HOST) $(HOSTOBJ)/fdc.o $(HOSTOBJ)/ide.o $(HOSTOBJ)/hdformat.o $(HOSTOBJ)/main.o $(HOSTOBJ)/net.o $(HOSTOBJ)/net_sock.o $(HOSTOBJ)/net_host.o $(HOSTOBJ)/web.o $(HOSTOBJ)/print.o)

build/archicast-interp: src/mach.c src/interp.c src/cpu.h $(INTERP_OBJ)
	$(HOSTCC) $(HOSTFLAGS) -O2 -DARM_INTERP -c src/mach.c -o $(HOSTOBJ)/mach_i.o
	$(HOSTCC) $(HOSTFLAGS) -O2 -DARM_INTERP -c src/interp.c -o $(HOSTOBJ)/interp_i.o
	$(HOSTCC) -o $@ $(INTERP_OBJ) $(HOSTOBJ)/mach_i.o $(HOSTOBJ)/interp_i.o

build/archicast-interpall: src/mach.c src/interp.c src/cpu.h $(INTERP_OBJ)
	$(HOSTCC) $(HOSTFLAGS) -O2 -DARM_INTERP -DARM_INTERP_ALL -c src/mach.c -o $(HOSTOBJ)/mach_a.o
	$(HOSTCC) $(HOSTFLAGS) -O2 -DARM_INTERP -DARM_INTERP_ALL -c src/interp.c -o $(HOSTOBJ)/interp_a.o
	$(HOSTCC) -o $@ $(INTERP_OBJ) $(HOSTOBJ)/mach_a.o $(HOSTOBJ)/interp_a.o

# Dreamcast build; run through scripts/build-dc.sh, which sources the KOS environment.
DCOBJ = build/dc
XLAT_DC = $(patsubst $(GEN)/%.c,$(DCOBJ)/%.o,$(XLAT_SRC))
OV_DC = $(patsubst $(GEN)/%.c,$(DCOBJ)/%.o,$(filter-out $(GEN)/ov_index.c,$(OV_SRC)))
DCFLAGS = $(KOS_CFLAGS) -ffixed-r14 -Os -fno-strict-aliasing -Wno-unused-value -Wno-unused-variable -Isrc

$(DCOBJ)/%.o: $(GEN)/%.c src/cpu.h
	@mkdir -p $(DCOBJ)
	@$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/mach.o: src/mach.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -O2 $(if $(HW_LOG),-DHW_LOG) $(if $(INTERP),-DARM_INTERP) $(if $(HW_NO_NET),-DHW_NO_NET) -c $< -o $@

$(DCOBJ)/interp.o: src/interp.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -O2 $(if $(HW_LOG),-DHW_LOG) -c $< -o $@

# The build options are baked into these objects, so a change of options has to
# rebuild them even though the sources did not change: a mach.o compiled without
# ARM_INTERP next to a fresh interp.o leaves the image unable to run disc software.
DC_OPTS = $(DCOBJ)/opts.$(INTERP).$(HW_LOG).$(HW_NO_SD).$(HW_NO_NET).$(HW_REPLAY).$(HW_DUMP).$(HW_AUTOFORMAT)
$(DC_OPTS):
	@mkdir -p $(DCOBJ)
	@rm -f $(DCOBJ)/opts.*
	@touch $@
$(DCOBJ)/mach.o $(DCOBJ)/main.o $(DCOBJ)/interp.o: $(DC_OPTS)

$(DCOBJ)/fdc.o: src/fdc.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/ide.o: src/ide.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/hdformat.o: src/hdformat.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/main.o: src/dc/main.c src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) $(if $(INTERP),-DARM_INTERP) $(if $(HW_NO_SD),-DHW_NO_SD) $(if $(HW_AUTOFORMAT),-DHW_AUTOFORMAT) $(if $(HW_LOG),-DHW_LOG '-DPC_ROOT="$(CURDIR)/build/disc"') $(if $(HW_REPLAY),-DHW_REPLAY -Ibuild) $(if $(HW_DUMP),-DHW_DUMP) -c $< -o $@

$(DCOBJ)/net.o: src/net.c src/net.h src/print.h src/web.h src/cpu.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/net_sock.o: src/net_sock.c src/net.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/web.o: src/web.c src/web.h src/net.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/print.o: src/print.c src/print.h src/net.h src/web.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -c $< -o $@

$(DCOBJ)/tls.o: src/dc/tls.c src/web.h src/net.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -I$(KOS_PORTS)/include -c $< -o $@

$(DCOBJ)/net_dc.o: src/dc/net.c src/net.h src/print.h
	@mkdir -p $(DCOBJ)
	$(KOS_CC) $(DCFLAGS) -I$(KOS_BASE)/addons/include -c $< -o $@

$(DCOBJ)/images.o: src/dc/images.S assets/cmos.bin
	@mkdir -p $(DCOBJ)
	$(KOS_CC) -c $< -o $@

dc: build/archicast.elf $(OV_DC)

build/archicast.elf: $(XLAT_DC) $(DCOBJ)/mach.o $(DCOBJ)/fdc.o $(DCOBJ)/ide.o $(DCOBJ)/hdformat.o $(DCOBJ)/main.o $(DCOBJ)/images.o $(DCOBJ)/net.o $(DCOBJ)/net_sock.o $(DCOBJ)/net_dc.o $(DCOBJ)/web.o $(DCOBJ)/print.o $(DCOBJ)/tls.o $(if $(INTERP),$(DCOBJ)/interp.o)
	kos-cc -o $@ $^ -lppp -lmbedtls -lmbedx509 -lmbedcrypto -Wl,--wrap=realloc -Wl,-Map,build/archicast.map

# The Archimedes software archive as an ADFS hard disc, streamed off the CD.
# Downloads are cached under /tmp/archimedes-archive, so only the first build is slow.
# One source is 7z: with no 7z tool, py7zr goes into build/pydeps, not the system.
build/archive.hd: tools/archive-hd.py tools/adfs.py
	@command -v 7zz >/dev/null || command -v 7z >/dev/null || PYTHONPATH=build/pydeps python3 -c 'import py7zr' 2>/dev/null || \
		python3 -m pip install -q --target build/pydeps py7zr || true
	PYTHONPATH=build/pydeps python3 tools/archive-hd.py $@

# Disc image tool, vendored under tools/mkdcdisc (see tools/mkdcdisc/VENDOR.md).
# Built with the host compiler, so run this before sourcing the KOS environment.
MKDCDISC_SRC = tools/mkdcdisc
MKDCDISC_BUILD = build/mkdcdisc
MKDCDISC_BIN = $(MKDCDISC_BUILD)/mkdcdisc

mkdcdisc: $(MKDCDISC_BIN)

$(MKDCDISC_BIN): $(MKDCDISC_SRC)/meson.build $(MKDCDISC_SRC)/meson_options.txt $(shell find $(MKDCDISC_SRC)/src -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.include' -o -name '*.in' \))
	@mkdir -p build
	@[ -f $(MKDCDISC_BUILD)/build.ninja ] || meson setup $(MKDCDISC_BUILD) $(MKDCDISC_SRC)
	meson compile -C $(MKDCDISC_BUILD)

clean:
	rm -rf build
