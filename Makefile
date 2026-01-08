# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
# Makefile for Concurrent Data Structure Testing Framework
# 
# This Makefile builds BPF programs and userspace applications for testing
# concurrent data structures using BPF arena shared memory.
#
# USAGE:
#   make                    # Build all programs
#   make skeleton           # Build just the skeleton test program
#   make clean              # Remove all build artifacts
#   make test               # Run basic smoke tests
#
# CUSTOMIZATION:
#   Set CLANG to use a specific clang version
#   Set LIBBPF_SRC to point to your libbpf source
#   Set VMLINUX to use a different vmlinux.h

# ============================================================================
# OUTPUT DIRECTORY
# ============================================================================
# All build artifacts go here to keep the source tree clean
OUTPUT := .output

# ============================================================================
# TOOLCHAIN CONFIGURATION
# ============================================================================
# Compiler for BPF programs (must support BPF target)
# Use clang-20 for full BPF arena support
CLANG ?= clang-20

# Compiler for userspace programs
CC ?= gcc

# ============================================================================
# DEPENDENCY PATHS
# ============================================================================
# libbpf source directory - provides BPF loading and management APIs
LIBBPF_SRC := $(abspath ./third_party/libbpf/src)

# bpftool source directory - generates BPF skeletons
BPFTOOL_SRC := $(abspath ./third_party/bpftool/src)

# Built libbpf static library
LIBBPF_OBJ := $(abspath $(OUTPUT)/libbpf.a)

# bpftool output directory and binary
BPFTOOL_OUTPUT ?= $(abspath $(OUTPUT)/bpftool)
BPFTOOL ?= $(BPFTOOL_OUTPUT)/bootstrap/bpftool

# ============================================================================
# ARCHITECTURE DETECTION
# ============================================================================
# Detect current architecture and normalize to kernel naming
ARCH ?= $(shell uname -m | sed 's/x86_64/x86/' \
			 | sed 's/arm.*/arm/' \
			 | sed 's/aarch64/arm64/' \
			 | sed 's/ppc64le/powerpc/' \
			 | sed 's/mips.*/mips/' \
			 | sed 's/riscv64/riscv/' \
			 | sed 's/loongarch64/loongarch/')

# vmlinux.h - kernel type definitions for BPF programs
# VMLINUX := ./third_party/vmlinux/include/$(ARCH)/vmlinux.h
VMLINUX := ./third_party/vmlinux.h

# ============================================================================
# INCLUDE PATHS
# ============================================================================
# Include paths for both BPF and userspace compilation:
# - $(OUTPUT): For generated skeleton headers
# - libbpf/include/uapi: For BPF UAPI headers
# - $(dir $(VMLINUX)): For vmlinux.h
# - ./include: For local headers (ds_api.h, libarena_ds.h, etc.)
INCLUDES := -I$(OUTPUT) -I./third_party/libbpf/include/uapi -I$(dir $(VMLINUX)) -I./include

# ============================================================================
# COMPILER FLAGS
# ============================================================================
# Userspace C flags
CFLAGS := -g -Wall -Wextra -O0

# Linker flags
ALL_LDFLAGS := $(LDFLAGS) $(EXTRA_LDFLAGS)

# ============================================================================
# APPLICATION LIST
# ============================================================================
# List of all applications to build
# - BPF_APPS: BPF-backed (need skeleton generation + libbpf)
# - USERTEST_APPS: pure userspace pthread tests (no BPF, no CLI args)
BPF_APPS = skeleton skeleton_msqueue skeleton_vyukhov skeleton_mpsc skeleton_bintree skeleton_bst skeleton_folly_spsc
USERTEST_APPS = usertest_list usertest_msqueue usertest_mpsc usertest_vyukhov usertest_folly_spsc usertest_bst usertest_bintree
APPS = $(BPF_APPS) $(USERTEST_APPS)

# ============================================================================
# CLANG BPF SYSTEM INCLUDES
# ============================================================================
# Get Clang's default include directories for BPF compilation.
# This ensures architecture-specific headers are found when compiling
# with -target bpf. Without this, headers like asm/types.h would be missing.
CLANG_BPF_SYS_INCLUDES ?= $(shell $(CLANG) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

# ============================================================================
# VERBOSE BUILD CONTROL
# ============================================================================
# Set V=1 for verbose output showing full commands
ifeq ($(V),1)
	Q =
	msg =
else
	Q = @
	msg = @printf '  %-8s %s%s\n'					\
		      "$(1)"						\
		      "$(patsubst $(abspath $(OUTPUT))/%,%,$(2))"	\
		      "$(if $(3), $(3))";
	MAKEFLAGS += --no-print-directory
endif

# ============================================================================
# COMPILER OVERRIDE SUPPORT
# ============================================================================
# Allow environment variables to override default compilers
define allow-override
  $(if $(or $(findstring environment,$(origin $(1))),\
            $(findstring command line,$(origin $(1)))),,\
    $(eval $(1) = $(2)))
endef

$(call allow-override,CC,$(CROSS_COMPILE)cc)
$(call allow-override,LD,$(CROSS_COMPILE)ld)

# ============================================================================
# MAIN TARGETS
# ============================================================================

.PHONY: all
all: $(APPS)
	@echo ""
	@echo "Build complete! Built applications:"
	@for app in $(APPS); do echo "  - $$app"; done
	@echo ""
	@echo "Run tests with:"
	@echo "  sudo ./skeleton -d 5"
	@echo "  sudo ./skeleton_msqueue -d 5"

.PHONY: clean
clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT) $(APPS)

# Create output directories
$(OUTPUT) $(OUTPUT)/libbpf $(BPFTOOL_OUTPUT):
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

# ============================================================================
# DEPENDENCY BUILDS
# ============================================================================

# Build libbpf static library
# This provides functions like bpf_object__open, bpf_program__attach, etc.
$(LIBBPF_OBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(OUTPUT)/libbpf
	$(call msg,LIB,$@)
	$(Q)$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1		      \
		    OBJDIR=$(dir $@)/libbpf DESTDIR=$(dir $@)		      \
		    INCLUDEDIR= LIBDIR= UAPIDIR=			      \
		    install

# Build bpftool
# This generates BPF skeleton headers from compiled BPF objects
$(BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(call msg,BPFTOOL,$@)
	$(Q)$(MAKE) ARCH= CROSS_COMPILE= OUTPUT=$(BPFTOOL_OUTPUT)/ -C $(BPFTOOL_SRC) bootstrap

# ============================================================================
# BPF PROGRAM COMPILATION
# ============================================================================

# Compile .bpf.c source to BPF object file
# 
# Steps:
# 1. Compile to temporary BPF object with -target bpf
# 2. Use bpftool to generate final object with CO-RE relocations
#
# Key flags:
# - -target bpf: Generate BPF bytecode
# - -D__TARGET_ARCH_$(ARCH): Define target architecture
# - -D__BPF_FEATURE_ADDR_SPACE_CAST: Enable arena address space casting
# - -O2: Optimize (required for BPF verifier)
$(OUTPUT)/%.bpf.o: src/%.bpf.c $(LIBBPF_OBJ) $(wildcard include/*.h) $(VMLINUX) | $(OUTPUT) $(BPFTOOL)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -D__BPF_FEATURE_ADDR_SPACE_CAST	      \
		     $(INCLUDES) $(CLANG_BPF_SYS_INCLUDES)		      \
		     -c $(filter %.c,$^) -o $(patsubst %.bpf.o,%.tmp.bpf.o,$@)
	$(Q)$(BPFTOOL) gen object $@ $(patsubst %.bpf.o,%.tmp.bpf.o,$@)

# ============================================================================
# BPF SKELETON GENERATION
# ============================================================================

# Generate BPF skeleton header from compiled BPF object
#
# The skeleton header provides:
# - struct <name>_bpf: Contains map and program file descriptors
# - <name>_bpf__open_and_load(): Load BPF program
# - <name>_bpf__attach(): Attach to hooks
# - <name>_bpf__destroy(): Cleanup
# - Access to maps via skel->maps.<map_name>
# - Access to global variables via skel->bss-><var_name>
$(OUTPUT)/%.skel.h: $(OUTPUT)/%.bpf.o | $(OUTPUT) $(BPFTOOL)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

# ============================================================================
# USERSPACE COMPILATION
# ============================================================================

# Compile userspace .c files to object files
# These depend on the skeleton header being generated first
$(patsubst %,$(OUTPUT)/%.o,$(BPF_APPS)): %.o: %.skel.h

$(OUTPUT)/%.o: src/%.c $(wildcard include/*.h) | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

# Userspace-only test runners (no libbpf / no skeleton headers)
$(OUTPUT)/%.o: usertest/%.c $(wildcard include/*.h) usertest/usertest_common.h | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

# ============================================================================
# BINARY LINKING
# ============================================================================

# Link userspace object with libbpf to create final executable
#
# Libraries needed:
# - libbpf (BPF program loading/management)
# - libelf (ELF file parsing)
# - libz (compression, used by libbpf)
$(BPF_APPS): %: $(OUTPUT)/%.o $(LIBBPF_OBJ) | $(OUTPUT)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(ALL_LDFLAGS) -lelf -lz -lpthread -o $@

$(USERTEST_APPS): %: $(OUTPUT)/%.o | $(OUTPUT)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(ALL_LDFLAGS) -lpthread -o $@

.PHONY: usertest
usertest: $(USERTEST_APPS)
	@echo ""
	@echo "Built userspace-only test runners:"
	@for app in $(USERTEST_APPS); do echo "  - $$app"; done

# ============================================================================
# TESTING TARGETS
# ============================================================================

.PHONY: test
test: skeleton skeleton_msqueue
	@echo "Running basic tests..."
	@echo ""
	@echo "Test 1: Skeleton (list) - 5 second sleep"
	sudo ./skeleton -d 5 || (echo "FAILED: skeleton"; exit 1)
	@echo ""
	@echo "Test 2: Skeleton MS Queue - 5 second sleep"
	sudo ./skeleton_msqueue -d 5 || (echo "FAILED: skeleton_msqueue"; exit 1)
	@echo ""
	@echo "All tests passed!"

.PHONY: test-stress
test-stress: skeleton skeleton_msqueue
	@echo "Running stress tests..."
	@echo "This may take a few minutes..."
	@echo ""
	@echo "Stress test 1: Skeleton (list) - 30 second sleep"
	sudo ./skeleton -d 30 || (echo "FAILED"; exit 1)
	@echo ""
	@echo "Stress test 2: Skeleton MS Queue - 30 second sleep"
	sudo ./skeleton_msqueue -d 30 || (echo "FAILED"; exit 1)
	@echo ""
	@echo "Stress tests passed!"

.PHONY: test-verify
test-verify: skeleton skeleton_msqueue
	@echo "Running verification tests..."
	@echo ""
	@echo "Test 1: Skeleton (list) with verification"
	sudo ./skeleton -d 5 -v || (echo "FAILED"; exit 1)
	@echo ""
	@echo "Test 2: Skeleton MS Queue with verification"
	sudo ./skeleton_msqueue -d 5 -v || (echo "FAILED"; exit 1)
	@echo "Verification tests passed!"

# ============================================================================
# HELP TARGET
# ============================================================================

.PHONY: help
help:
	@echo "BPF Arena Data Structure Testing Framework"
	@echo ""
	@echo "Targets:"
	@echo "  all          Build all programs (default)"
	@echo "  skeleton     Build skeleton test program"
	@echo "  clean        Remove all build artifacts"
	@echo "  test         Run basic smoke tests"
	@echo "  test-stress  Run stress tests"
	@echo "  test-verify  Run verification tests"
	@echo "  help         Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  V=1          Verbose build output"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build everything"
	@echo "  make clean && make      # Clean build"
	@echo "  make V=1 skeleton       # Verbose build of skeleton"
	@echo "  make test               # Run tests"

# ============================================================================
# MAKE DIRECTIVES
# ============================================================================

# Delete targets if their recipe fails
.DELETE_ON_ERROR:

# Keep intermediate files (.skel.h, .bpf.o, etc)
.SECONDARY:
