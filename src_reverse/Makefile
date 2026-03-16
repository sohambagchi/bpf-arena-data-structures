# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# src_reverse/Makefile — self-contained build for the reversed MS-Queue demo
#
# Dependencies (relative to the repository root, one level up):
#   ../third_party/libbpf      — libbpf source (submodule)
#   ../third_party/bpftool     — bpftool source (submodule)
#   ../third_party/vmlinux.h   — pre-generated vmlinux header
#   ../include/                — shared data-structure headers
#
# Usage:
#   make              # build everything → ./build/msqueue_rev
#   make clean        # remove build artefacts
#   sudo ./build/msqueue_rev   # run the demo

# ============================================================================
# Paths
# ============================================================================
REPO_ROOT  := $(abspath ..)
# Use absolute paths throughout so sub-makes (libbpf, bpftool) resolve dirs
# correctly regardless of which directory they are invoked from.
OUTPUT     := $(abspath .output)
OUT_DIR    := $(abspath build)

LIBBPF_SRC    := $(REPO_ROOT)/third_party/libbpf/src
BPFTOOL_SRC   := $(REPO_ROOT)/third_party/bpftool/src
LIBBPF_OBJ    := $(OUTPUT)/libbpf.a
BPFTOOL_OUTPUT := $(OUTPUT)/bpftool
BPFTOOL       := $(BPFTOOL_OUTPUT)/bootstrap/bpftool
VMLINUX       := $(REPO_ROOT)/third_party/vmlinux.h
INCLUDES_DIR  := $(REPO_ROOT)/include

# ============================================================================
# Toolchain
# ============================================================================
CLANG  ?= clang-20
CC     ?= gcc

ifeq ($(origin CLANG), file)
ifeq ($(shell command -v $(CLANG) >/dev/null 2>&1; echo $$?),1)
ifneq ($(shell command -v clang >/dev/null 2>&1; echo $$?),1)
CLANG := clang
endif
endif
endif

# ============================================================================
# Architecture
# ============================================================================
ARCH ?= $(shell uname -m | sed 's/x86_64/x86/' \
                          | sed 's/aarch64/arm64/' \
                          | sed 's/arm.*/arm/')

# ============================================================================
# Include flags
# ============================================================================
INCLUDES := \
	-I$(OUTPUT) \
	-I$(LIBBPF_SRC) \
	-I$(REPO_ROOT)/third_party/libbpf/include/uapi \
	-I$(dir $(VMLINUX)) \
	-I$(INCLUDES_DIR)

# ============================================================================
# Clang BPF system includes
# ============================================================================
CLANG_BPF_SYS_INCLUDES ?= $(shell $(CLANG) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

# ============================================================================
# Compiler flags
# ============================================================================
CFLAGS := -g -Wall -Wextra -O0 -DLKMM_OPTIMIZED
LDFLAGS :=

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean

all: $(OUT_DIR)/msqueue_rev
	@echo ""
	@echo "Build complete."
	@echo "Run with:  sudo $(OUT_DIR)/msqueue_rev"
	@echo "Then see:  sudo cat /sys/kernel/debug/tracing/trace_pipe"

clean:
	rm -rf $(abspath .output) $(abspath build)

# ============================================================================
# Output directories
# ============================================================================
$(OUTPUT) $(OUTPUT)/libbpf $(BPFTOOL_OUTPUT) $(OUT_DIR):
	mkdir -p $@

# ============================================================================
# libbpf static library
# ============================================================================
$(LIBBPF_OBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(OUTPUT)/libbpf
	$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1 \
		OBJDIR=$(dir $@)/libbpf \
		DESTDIR=$(dir $@) \
		INCLUDEDIR= LIBDIR= UAPIDIR= \
		install

# ============================================================================
# bpftool
# ============================================================================
$(BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(MAKE) ARCH= CROSS_COMPILE= OUTPUT=$(BPFTOOL_OUTPUT)/ \
		-C $(BPFTOOL_SRC) bootstrap

# ============================================================================
# BPF object
# ============================================================================
$(OUTPUT)/msqueue_rev.bpf.o: msqueue_rev.bpf.c \
		$(wildcard $(INCLUDES_DIR)/*.h) $(VMLINUX) \
		$(LIBBPF_OBJ) | $(OUTPUT) $(BPFTOOL)
	$(CLANG) -g -O2 -target bpf \
		-D__TARGET_ARCH_$(ARCH) \
		-D__BPF_FEATURE_ADDR_SPACE_CAST \
		$(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) \
		-c msqueue_rev.bpf.c \
		-o $(OUTPUT)/msqueue_rev.tmp.bpf.o
	$(BPFTOOL) gen object $@ $(OUTPUT)/msqueue_rev.tmp.bpf.o

# ============================================================================
# Skeleton header
# ============================================================================
$(OUTPUT)/msqueue_rev.skel.h: $(OUTPUT)/msqueue_rev.bpf.o | $(OUTPUT) $(BPFTOOL)
	$(BPFTOOL) gen skeleton $< > $@

# ============================================================================
# Userspace object (depends on skeleton header being generated first)
# ============================================================================
$(OUTPUT)/msqueue_rev.o: msqueue_rev.c $(OUTPUT)/msqueue_rev.skel.h \
		$(wildcard $(INCLUDES_DIR)/*.h) | $(OUTPUT)
	$(CC) $(CFLAGS) $(INCLUDES) -c msqueue_rev.c -o $@

# ============================================================================
# Final binary
# ============================================================================
$(OUT_DIR)/msqueue_rev: $(OUTPUT)/msqueue_rev.o $(LIBBPF_OBJ) | $(OUT_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lelf -lz -lpthread -o $@

# Keep intermediates
.SECONDARY:
.DELETE_ON_ERROR:
