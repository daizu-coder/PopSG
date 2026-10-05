# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
#
# Windows CE (SHARP Brain PW-G5300, ARM, no HW integer divide) port of
# PicoDrive via its libretro core. Built with the cegcc cross toolchain.
#
# Run from inside this directory (the cpu/cyclone and pico/sound/emu2413
# submodules must be checked out - see CE/README.md):
#   make clean && make && make strip
#
# Scope/architecture decisions (see the dev notes for the full rationale):
#  - platform/libretro/libretro.c is compiled as-is ("libretro frontend,
#    not a core port", same approach as the sister PopSNES port).
#    The core itself carries only two small fixes: the missing NO_32X
#    stubs in pico/pico_int.h and one load-width fix in pico/draw_arm.S
#    (see below).
#  - 32X is excluded at compile time (-DNO_32X): its SH2 cores add a lot
#    of CPU/RAM cost this device almost certainly can't spare, and one of
#    them (cpu/sh2/compiler.c) is a dynamic recompiler - unverified on
#    this OS/toolchain.
#  - Sega CD *code* is NOT excluded (pico/cd/*.c has no source-level way
#    to opt out - see platform/common/common.mak, it's unconditional) but
#    is functionally limited to raw BIN/CUE: no CHD (use_libchdr=0, no
#    libchdr submodule) and no compressed Redbook audio tracks (see
#    ce_ogg_stub.c).
#  - 68000 core: Cyclone (round 7), replacing cpu/fame (portable C
#    interpreter) used through round 6. Cyclone is NOT a runtime JIT like
#    the SH2/SVP compilers below: Cyclone.s is a static ARM asm file
#    generated once by a host-side C++ tool and never touches
#    cpu/drc/cmn.c's RWX tcache. The generator and Cyclone.h come from the
#    cpu/cyclone submodule, unmodified; the generated file is checked in
#    as CE/cyclone/Cyclone.s (so the build needs no host g++). It was
#    produced with HAVE_ARMv6=0, matching upstream's own "miyoo"
#    (armv5te/arm926ej-s) profile in Makefile.libretro rather than the
#    config header's ARMv6 default - this device is ARMv5TE, and
#    HAVE_ARMv6=1 would emit sxtb/sxth/uxth (ARMv6-only) into the
#    generated .s. To regenerate it, see CE/cyclone/README.txt. m68kif_cyclone.s
#    (small hand-written ARM asm glue already checked into pico/, no
#    submodule needed) supplies the fast memory-map callbacks Cyclone
#    calls into. 68000 is the Genesis's main CPU, so this was the
#    most promising lever after the round-6 perf profiling (see
#    the dev notes) pointed at it as the single biggest per-frame cost.
#    Cyclone's tools/idle.s (the idle-busy-loop patcher, fast-
#    forwards VBlank/DMA/VDP-status polling loops instead of interpreting
#    every spin) originally failed to assemble here: its two entry points
#    located their data tables via a position-independent "offset_table"
#    trick (.word idle_data - offset_table, a cross-section symbol
#    difference) that this PE/COFF toolchain's assembler can't represent
#    as a static relocation. Since this build links idle.s directly into
#    the .exe (never copies/relocates it at runtime), the PIC trick was
#    unnecessary - rewritten to plain `ldr rX, =symbol` absolute-address
#    loads (round 8, see the dev notes), which assembles fine under COFF and
#    keeps the actual patching logic byte-for-byte identical. That modified
#    copy lives in CE/cyclone/idle.s; the submodule's own tools/idle.s is
#    not built. This restores
#    idle-loop fast-forwarding, which round 7's Fame->Cyclone perf logs
#    showed was the likely reason Cyclone's raw interpreter speed wasn't
#    translating into a real-world win (Fame has its own, unrelated idle
#    detector - fm68k_idle_install/fm68k_idle_remove - so it never lost
#    this optimization when Cyclone's was stubbed out).
#  - Z80 core: cpu/DrZ80 (round 19), replacing cpu/cz80 (portable C
#    interpreter) used through round 18. Like Cyclone, DrZ80 is a static
#    ARM asm interpreter (cpu/DrZ80/drz80.S, already checked into this
#    tree, no submodule needed), not a runtime JIT - it never touches
#    cpu/drc/cmn.c's RWX tcache. Unlike Cyclone this is upstream
#    PicoDrive's own maintained integration (drz80.S carries a 2021-24
#    irixxxx copyright alongside the original 2004 Reesy one, and
#    pico/z80if.c/pico/sms.c/pico/memory.c/pico/pico_int.h already had
#    complete #ifdef _USE_DRZ80 branches wired in - no core changes were
#    needed, same "already fully implemented, just unused" situation as
#    EMU_C68K was for Cyclone in round 6). Confirmed clean for this
#    toolchain the same way Cyclone.s/idle.s were checked: no ELF-only
#    directives, no cross-section symbol-difference relocations. The one
#    thing that looked like it might be idle.s's PIC_LDR trouble again -
#    pico/arm_features.h's PIC_LDR() macro, which drz80.S uses to load
#    its jump/flag tables - turned out to be a non-issue: it only emits
#    the GOT-relative sequence when __PIC__ is defined, and this build
#    never compiles with -fPIC, so it expands to a plain `ldr r,=symbol`
#    literal-pool load, identical in effect to idle.s's round-8 fix.
#    Z80 is the Genesis's auxiliary sound-driver CPU, not the main one,
#    so the expected payoff is smaller than Cyclone's - but the
#    integration cost turned out to be a one-line source-list swap plus
#    a define, since the hard part (verifying the calling convention
#    against this asm-hostile PE/COFF toolchain) was already done by
#    Cyclone's round 7-8 groundwork and transfers directly. License is
#    Reesy's "free for non-commercial use" (drz80.S's own header) - fine
#    for this personal-use port, worth knowing about before shipping.
#  - SVP (Virtua Racing's chip) interpreter only (use_svpdrc=0):
#    pico/carthw/svp/compiler.c *is* a true runtime recompiler (same
#    RWX-tcache path as the SH2 compiler above, via cpu/drc/cmn.c) and is
#    excluded for that JIT-avoidance reason.
#  - cpu/drc/cmn.c (the RWX tcache-allocation helper actually used only by
#    the SH2 and SVP compilers above - not by Cyclone or DrZ80, see their
#    notes) is dropped from the source list entirely, not just left
#    uncalled: with the SH2 compiler and SVP compiler both excluded,
#    nothing in this build ever calls into it, and its page-aligned
#    tcache array (ALIGNED(4096)) doesn't build on this toolchain's
#    object format anyway (max alignment 8) - see the dev notes for the full
#    trace of how that was confirmed dead code before removing it.
#  - pico/draw_arm.S + pico/draw2_arm.S (round 10 investigated draw_arm.S
#    alone and reverted it, see the dev notes; round 11 added draw2_arm.S and
#    wired both in): hand-written ARM asm replacements for pico/draw.c's
#    and pico/draw2.c's tile/sprite renderers, enabled together via
#    -D_ASM_DRAW_C (upstream's own build switch - see both .c files'
#    #ifndef _ASM_DRAW_C guards). Investigated as a follow-up to round 6's
#    profiling, which named `draw` (5-6ms/frame in SSF2 combat) the
#    second-largest per-frame cost after m68k. Both files check out fine
#    for this toolchain (no ELF-only directives, no idle.s-style PIC
#    relocation trick, plain APCS/AAPCS register save/restore - see
#    the dev notes for the full inspection of each). Their shared
#    pico_int_offs.h prerequisite is solved the same way as before:
#    tools/mkoffsets.sh doesn't work with this toolchain for two reasons
#    documented in tools/mkoffsets_ce.sh's header comment, so that script
#    is the replacement (extended in round 11 with the two extra offsets
#    draw2_arm.S needs). Its output, pico/pico_int_offs.h, is generated at
#    build time by the rule below (upstream's .gitignore ignores that file,
#    and regenerating it keeps the offsets in step with pico_int.h).

ROOT := ..

TOOLPREFIX := /opt/cegcc/bin/arm-mingw32ce-
CC      := $(TOOLPREFIX)gcc
STRIP   := $(TOOLPREFIX)strip
OBJDUMP := $(TOOLPREFIX)objdump
NM      := $(TOOLPREFIX)nm
WINDRES := $(TOOLPREFIX)windres

# armv5te: matches the target device (has BLX/BLX-reg and other v5T
# instructions some of the portable C cores' inline asm bits assume, but
# no hardware integer divide). mno-unaligned-access: let the compiler
# emit safe byte/half accesses rather than faulting on ARM's strict
# alignment trap - this device's CPU does not tolerate unaligned word
# accesses (confirmed the hard way by both sister CE ports).
ARCH_FLAGS := -march=armv5te -mno-unaligned-access

# === Core sources (unmodified upstream code) ===

PICO_DIR := $(ROOT)/pico
CPU_DIR  := $(ROOT)/cpu
LIBRETRO_DIR      := $(ROOT)/platform/libretro
LIBRETRO_COMM_DIR := $(LIBRETRO_DIR)/libretro-common

SOURCES_CORE := \
	$(PICO_DIR)/pico.c $(PICO_DIR)/cart.c $(PICO_DIR)/memory.c \
	$(PICO_DIR)/state.c $(PICO_DIR)/sek.c $(PICO_DIR)/z80if.c \
	$(PICO_DIR)/videoport.c $(PICO_DIR)/draw2.c $(PICO_DIR)/draw.c \
	$(PICO_DIR)/mode4.c $(PICO_DIR)/misc.c $(PICO_DIR)/eeprom.c \
	$(PICO_DIR)/patch.c $(PICO_DIR)/debug.c $(PICO_DIR)/media.c \
	$(PICO_DIR)/carthw_cfg.c \
	$(PICO_DIR)/sms.c \
	$(PICO_DIR)/cd/mcd.c $(PICO_DIR)/cd/memory.c $(PICO_DIR)/cd/sek.c \
	$(PICO_DIR)/cd/cdc.c $(PICO_DIR)/cd/cdd.c $(PICO_DIR)/cd/cd_image.c \
	$(PICO_DIR)/cd/cd_parse.c $(PICO_DIR)/cd/gfx.c $(PICO_DIR)/cd/gfx_dma.c \
	$(PICO_DIR)/cd/misc.c $(PICO_DIR)/cd/pcm.c $(PICO_DIR)/cd/megasd.c \
	$(PICO_DIR)/pico/pico.c $(PICO_DIR)/pico/memory.c $(PICO_DIR)/pico/xpcm.c \
	$(PICO_DIR)/carthw/carthw.c $(PICO_DIR)/carthw/eeprom_spi.c \
	$(PICO_DIR)/carthw/svp/svp.c $(PICO_DIR)/carthw/svp/memory.c \
	$(PICO_DIR)/carthw/svp/ssp16.c \
	$(PICO_DIR)/sound/sound.c $(PICO_DIR)/sound/resampler.c \
	$(PICO_DIR)/sound/sn76496.c $(PICO_DIR)/sound/ym2612.c \
	$(PICO_DIR)/sound/ym2413.c $(PICO_DIR)/sound/vgm.c \
	$(PICO_DIR)/sound/mix.c \
	cyclone/Cyclone.s \
	cyclone/idle.s \
	$(PICO_DIR)/m68kif_cyclone.s \
	$(PICO_DIR)/draw_arm.S $(PICO_DIR)/draw2_arm.S \
	$(CPU_DIR)/DrZ80/drz80.S \
	$(LIBRETRO_DIR)/libretro.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strcasestr.c \
	$(ROOT)/platform/common/mp3.c $(ROOT)/platform/common/mp3_sync.c \
	$(ROOT)/platform/common/mp3_dummy.c \
	$(ROOT)/unzip/unzip.c \
	$(ROOT)/zlib/gzio.c $(ROOT)/zlib/inffast.c $(ROOT)/zlib/inflate.c \
	$(ROOT)/zlib/inftrees.c $(ROOT)/zlib/trees.c $(ROOT)/zlib/deflate.c \
	$(ROOT)/zlib/crc32.c $(ROOT)/zlib/adler32.c $(ROOT)/zlib/zutil.c \
	$(ROOT)/zlib/compress.c $(ROOT)/zlib/uncompr.c

SOURCES_CE := ce_main.c ce_log.c ce_display.c ce_input.c ce_audio.c ce_video.c \
	ce_config.c ce_lang.c ce_bmpfont.c ce_fileopen.c ce_ogg_stub.c ce_32x_stub.c ce_mmap_stub.c \
	ce_strerror_stub.c ce_rpng_stub.c ce_fopen_utf8.c

DEFINES  += -DNO_32X -D__LIBRETRO__ -DHAVE_STDINT_H -DHAVE_INTTYPES_H
DEFINES  += -DEMU_C68K -D_USE_DRZ80
DEFINES  += -DPICO_CE -DWINCE
# _ASM_DRAW_C: enables pico/draw_arm.S and pico/draw2_arm.S (see the
# architecture note above) - both files supply the same functions
# pico/draw.c and pico/draw2.c stub out under this flag (#ifndef
# _ASM_DRAW_C guards), so all four sources stay in SOURCES_CORE together.
DEFINES  += -D_ASM_DRAW_C
# _WIN32_WCE + NO_ERRNO_H: makes zlib use its own internal z_errno instead
# of a real <errno.h> (this toolchain doesn't have a working one - see
# CE/compat/errno.h); _WIN32_WCE alone has no other effect since nothing
# else in the compiled source set branches on it.
DEFINES  += -D_WIN32_WCE=0x0500 -DNO_ERRNO_H
# platform/common/version.h does `#define VERSION "2.05" REVISION`,
# expecting the top-level Makefile to supply REVISION as a git-describe
# string (see its `-DREVISION=\"$(GIT_REVISION)\"`); this Makefile does
# not query git, so supply a fixed marker instead.
DEFINES  += -DREVISION=\"-CE\"
INCLUDES += -I. -Icompat -I$(ROOT) -I$(ROOT)/zlib \
	-I$(LIBRETRO_COMM_DIR)/include -I$(LIBRETRO_COMM_DIR)/include/compat

# -O3: both sister CE ports measured a real gain over -O2 on this device
# with no regressions; -flto is NOT used - this cegcc ld doesn't support
# LTO plugin objects (confirmed by both prior ports).
# -fno-strict-aliasing: matches the upstream Makefile's own per-file
# overrides for these exact files (see the top-level Makefile) - applied
# blanket here rather than per-file since this build doesn't `include`
# that Makefile.
# zlib's zutil.h deliberately skips `#include <stddef.h>` when _WIN32_WCE
# is defined (its own comment: assumes some other already-included header
# pulled it in on real WinCE SDKs, e.g. windows.h). That assumption
# doesn't hold for the zlib .c files here (several use ptrdiff_t/size_t
# without including windows.h), so force it in for every file rather than
# patch zutil.h or add a per-file override for each affected zlib source.
CFLAGS  := $(ARCH_FLAGS) -O3 -fomit-frame-pointer -fno-strict-aliasing \
	-falign-functions=2 -include stddef.h $(DEFINES) $(INCLUDES)

# For preprocessed asm (draw_arm.S): same defines/includes as CFLAGS, but
# without -include stddef.h. That flag forces stddef.h's *C* typedefs
# (typedef int ptrdiff_t; ...) through cpp regardless of file type; on a
# .c file the compiler proper then parses them normally, but on a .S file
# cpp's output goes straight to the assembler, which chokes on literal C
# syntax ("Error: bad instruction `typedef int ptrdiff_t'"). draw_arm.S
# doesn't need stddef.h itself (its only preprocessor use is including
# pico_int_offs.h, a flat list of #define NUMBER lines).
ASCPPFLAGS := $(ARCH_FLAGS) -O3 $(DEFINES) $(INCLUDES)

# cdd.c uses off_t without including <sys/types.h> (relies on some other
# platform's headers pulling it in transitively via stdio.h - this
# toolchain's don't). Force-include it for just this file rather than
# patching the core source.
$(PICO_DIR)/cd/cdd.o: CFLAGS += -include sys/types.h

# No C++ anywhere in this port (core is all .c, CE frontend is written in
# plain C on purpose) - avoids ever depending on libstdc++-6.dll, which
# does not exist on this device and silently kills startup if linked in.
# aygshell.dll is not linked (ce_main.c hides the taskbar via a direct
# FindWindow("HHTaskBar")+ShowWindow instead - see the sister CE ports'
# dev notes for why depending on aygshell.dll is fragile on this device).
# commdlg (GetOpenFileName) is not linked either - the ROM picker is a
# self-drawn dialog (ce_fileopen.c), not the standard common dialog.
LDFLAGS := -static-libgcc -lm

RESOURCES := ce_res.o

OBJECTS := $(patsubst %.S,%.o,$(patsubst %.s,%.o,$(patsubst %.c,%.o,$(SOURCES_CORE)))) \
	$(SOURCES_CE:.c=.o) $(RESOURCES)

TARGET := AppMain.exe

# UI bitmap font (ce_bmpfont.c) - same switch as PopPCE (2026-09-27).
# Galmuri14 is the default; the others stay buildable:
#   make                     Galmuri14 (default)  -> AppMain.exe
#   make CE_FONT=shinonome   Shinonome 16dot      -> AppMain_shinonome.exe
#   make CE_FONT=galmuri11   GalmuriMono11        -> AppMain_galmuri11.exe
# ce_galmuri14.h / ce_galmuri11.h are copies of PopPCE's
# tools/galmuri2c.py output. Only ce_bmpfont.o and ce_fileopen.o see the
# define; the .font-<name> stamp makes them rebuild when CE_FONT changes.
CE_FONT ?= galmuri14
ifeq ($(CE_FONT),galmuri14)
FONT_DEFS := -DCE_FONT_GALMURI14
else ifeq ($(CE_FONT),shinonome)
FONT_DEFS :=
TARGET := AppMain_shinonome.exe
else ifeq ($(CE_FONT),galmuri11)
FONT_DEFS := -DCE_FONT_GALMURI11
TARGET := AppMain_galmuri11.exe
else
$(error CE_FONT must be galmuri14, shinonome or galmuri11)
endif
FONT_STAMP := .font-$(CE_FONT)

.PHONY: all clean strip

# Fail early, with a clear message, when the submodules were not checked
# out (git clone without --recursive, or a GitHub "Download ZIP") - the
# build would otherwise stop much later on a missing Cyclone.h.
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(CPU_DIR)/cyclone/Cyclone.h),)
$(error cpu/cyclone is empty - clone with --recursive, or run: git submodule update --init cpu/cyclone pico/sound/emu2413)
endif
ifeq ($(wildcard $(PICO_DIR)/sound/emu2413/emu2413.c),)
$(error pico/sound/emu2413 is empty - clone with --recursive, or run: git submodule update --init cpu/cyclone pico/sound/emu2413)
endif
endif

all: $(TARGET)

$(FONT_STAMP):
	rm -f .font-*
	touch $@

ce_bmpfont.o ce_fileopen.o: $(FONT_STAMP) ce_bmpfont.h ce_galmuri14.h ce_galmuri11.h ce_shinonome16.h
ce_bmpfont.o ce_fileopen.o: CFLAGS += $(FONT_DEFS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Cyclone.s/idle.s/m68kif_cyclone.s are hand/tool-generated GNU-syntax ARM
# asm with no C preprocessor directives (verified: no #include/#if/#define
# in any of them), so this skips CFLAGS/DEFINES/INCLUDES entirely and only
# passes the arch flags the assembler needs to accept armv5te mnemonics.
%.o: %.s
	$(CC) $(ARCH_FLAGS) -c -o $@ $<

# draw_arm.S/draw2_arm.S (uppercase - needs cpp) each have one real
# preprocessor directive, `#include "pico_int_offs.h"` (quote-include
# resolves relative to the source file's own directory, i.e. pico/, where
# the rule below generates that header - see the architecture note above), so this
# needs DEFINES/INCLUDES unlike the plain %.s rule above - but
# ASCPPFLAGS, not CFLAGS (see its own comment for why).
%.o: %.S
	$(CC) $(ASCPPFLAGS) -c -o $@ $<

# pico/pico_int_offs.h: struct offsets for draw_arm.S/draw2_arm.S, read
# back from a cross-compiled test object by tools/mkoffsets_ce.sh (see its
# header for why upstream's tools/mkoffsets.sh can't be used here).
OFFS_H := $(PICO_DIR)/pico_int_offs.h
$(OFFS_H): $(ROOT)/tools/mkoffsets_ce.sh $(PICO_DIR)/pico_int.h
	CC="$(CC)" OBJDUMP="$(OBJDUMP)" CFLAGS="$(DEFINES) $(INCLUDES)" \
		sh $(ROOT)/tools/mkoffsets_ce.sh $(PICO_DIR)
$(PICO_DIR)/draw_arm.o $(PICO_DIR)/draw2_arm.o: $(OFFS_H)

# ce_res.o also depends on the header and the resource files it pulls in,
# so it is rebuilt when the app icon or the main-menu mascot bitmap (both
# in CE/icon/) change.
ce_res.o: ce_resource.h icon/popsg.ico icon/popsg_mascot.bmp

%.o: %.rc
	$(WINDRES) -F pe-arm-wince-little -I$(ROOT) -o $@ $<

strip: all
	$(STRIP) $(TARGET)

clean:
	rm -f .font-* AppMain_shinonome.exe AppMain_galmuri11.exe
	rm -f $(OBJECTS) $(TARGET) $(OFFS_H)
