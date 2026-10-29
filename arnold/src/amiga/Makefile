# Arnold (Kevin Thacker, GPL-2.0) for AmigaOS (AGA, 68040+ / PiStorm Emu68).
# Needs bebbo's m68k-amigaos-gcc on the PATH (WSL: /opt/amiga/bin).
#   make            -> Arnold (68040)
#   make CPU=68020  -> Arnold-020
CPU ?= 68040
# FPU: software floating point by default (14 Oct 2026): the core only uses floats in
# the sound set-up, and BigMig's 68060 core has no FPU. FPU=-m68881 would use one.
FPU ?= -msoft-float
# OPT: optimisation flags (variants for speed tests, e.g. OPT="-O3 -funroll-loops")
OPT ?= -O2
CC = m68k-amigaos-gcc
CFLAGS = -m$(CPU) $(FPU) $(OPT) -fomit-frame-pointer -std=gnu89 -fcommon -noixemul -w -I.. -DCPC_NODEBUGGER
LDFLAGS = -noixemul -s
LIBS = -lm

# objects.mak may have CRLF line endings (Windows checkout): strip \r first.
CORE := $(addprefix ../,$(shell tr -d '\r' < ../objects.mak | sed -n '/^CPC_O=/,/^$$/p' | tr -d '\\' | sed 's/CPC_O=//;s/\.o/.c/g'))
OBJDIR ?= obj-$(CPU)
OBJS := $(patsubst ../%.c,$(OBJDIR)/%.o,$(CORE)) $(OBJDIR)/host_amiga.o $(OBJDIR)/amiga_sys.o $(OBJDIR)/amiga_menu.o $(OBJDIR)/amiga_wb.o $(OBJDIR)/amiga_iff.o $(OBJDIR)/amiga_logo.o $(OBJDIR)/screenshot.o $(OBJDIR)/keyscript.o $(OBJDIR)/keymaplogic.o $(OBJDIR)/amiga_keymap.o $(OBJDIR)/gamedb.o \
	$(OBJDIR)/rtgfw.o $(OBJDIR)/rtg_conv.o $(OBJDIR)/tapeauto.o
TARGET ?= $(if $(filter 68040,$(CPU)),Arnold,Arnold-$(subst 680,0,$(CPU)))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -m$(CPU) $(FPU) -o $@ $(OBJS) $(LIBS)

$(OBJDIR)/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# rtgfw (RTG pilot, 26 Oct 2026): the vendored MIT framework (rtgfw/SOURCE.txt), last in
# the link so it doesn't move the hot code; gnu99 as in its own build
$(OBJDIR)/%.o: rtgfw/src/%.c rtgfw/src/rtgfw.h rtgfw/src/rtg_conv.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu99 -Irtgfw/src -c -o $@ $<

$(OBJDIR)/%.o: %.c amiga_sys.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
