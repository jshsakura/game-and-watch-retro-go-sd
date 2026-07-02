# Host harness for the Virtual Boy core (red-viper) — mirrors the DEVICE build:
# same sources as Makefile VB_C_SOURCES, same -DGNW_VB_DEVICE branches, same
# software renderer. Headless PPM/hash output (no SDL needed).
#
#   make -f Makefile.vb                                  # new renderer -> build/vb-new/
#   git show <old-commit>:external/red-viper/source/common/video_soft.cpp > vb/video_soft_old.cpp
#   make -f Makefile.vb VARIANT=old VIDEO_SOFT=vb/video_soft_old.cpp
#
# Diff the two binaries' stdout (per-frame fb hashes) / PPMs for pixel-exact A/B.
# (vb/video_soft_old.cpp is a generated artifact — not committed.)

VARIANT    ?= new
VIDEO_SOFT ?= ../external/red-viper/source/common/video_soft.cpp

CORE   = ../external/red-viper/source/common
BUILD  = build/vb-$(VARIANT)
TARGET = $(BUILD)/retro-go-vb

CC  = gcc
CXX = g++

# Device parity: GNW_VB_DEVICE picks the exact firmware ifdef branches
# (vb_dev_calloc RAM regions, external ROM pointer). DEBUGLEVEL=0 as on device.
DEFS = -DGNW_VB_DEVICE -DDEBUGLEVEL=0
INCS = -I$(CORE) -I../external/red-viper/include -I$(CORE)/inih
OPT  = -O2 -g -fno-strict-aliasing

CFLAGS   = $(DEFS) $(INCS) $(OPT) -Wall
CXXFLAGS = $(DEFS) $(INCS) $(OPT) -Wall -fno-rtti -fno-exceptions

SRCS_C = \
	$(CORE)/v810_cpu.c \
	$(CORE)/v810_ins.c \
	$(CORE)/v810_mem.c \
	$(CORE)/interpreter.c \
	$(CORE)/vb_set.c \
	$(CORE)/rom_db.c \
	$(CORE)/patches.c \
	$(CORE)/video_common.c \
	$(CORE)/inih/ini.c \
	vb/main.c

OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS_C:.c=.o))) $(BUILD)/video_soft.o

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

# C sources live in two dirs; simple vpath does it.
vpath %.c $(CORE) $(CORE)/inih vb

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/video_soft.o: $(VIDEO_SOFT) | $(BUILD)
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -lm -o $@
	@echo "built $(TARGET) (renderer: $(VIDEO_SOFT))"

clean:
	rm -rf $(BUILD)

.PHONY: all clean
