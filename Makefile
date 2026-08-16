# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

# ============================================================================
# OUTPUT DIRECTORY
# ============================================================================
OUTPUT := .output

OUT_DIR ?= build

# ============================================================================
# TOOLCHAIN CONFIGURATION
# ============================================================================
CLANG ?= clang-20

ifeq ($(origin CLANG), file)
ifeq ($(shell command -v $(CLANG) >/dev/null 2>&1; echo $$?),1)
ifneq ($(shell command -v clang >/dev/null 2>&1; echo $$?),1)
CLANG := clang
endif
endif
endif

CC ?= gcc
OBJDUMP ?= objdump

# ============================================================================
# DEPENDENCY PATHS
# ============================================================================
LIBBPF_SRC := $(abspath ./third_party/libbpf/src)

BPFTOOL_SRC := $(abspath ./third_party/bpftool/src)

LIBBPF_OBJ := $(abspath $(OUTPUT)/libbpf.a)

BPFTOOL_OUTPUT ?= $(abspath $(OUTPUT)/bpftool)
BPFTOOL ?= $(BPFTOOL_OUTPUT)/bootstrap/bpftool

# ============================================================================
# ARCHITECTURE DETECTION
# ============================================================================
ARCH ?= $(shell uname -m | sed 's/x86_64/x86/' \
			 | sed 's/arm.*/arm/' \
			 | sed 's/aarch64/arm64/' \
			 | sed 's/ppc64le/powerpc/' \
			 | sed 's/mips.*/mips/' \
			 | sed 's/riscv64/riscv/' \
			 | sed 's/loongarch64/loongarch/')

VMLINUX := ./third_party/vmlinux.h

# ============================================================================
# INCLUDE PATHS
# ============================================================================
INCLUDES := -I$(OUTPUT) -I./third_party/libbpf/include/uapi -I$(dir $(VMLINUX)) -I./include

# ============================================================================
# COMPILER FLAGS
# ============================================================================
CFLAGS := -g -Wall -Wextra -O0 -DLKMM_OPTIMIZED

ALL_LDFLAGS := $(LDFLAGS) $(EXTRA_LDFLAGS)

# ============================================================================
# APPLICATION LIST
# ============================================================================
BPF_APPS = skeleton_msqueue skeleton_vyukhov skeleton_folly_spsc skeleton_ck_fifo_spsc skeleton_ck_ring_spsc skeleton_ck_stack_upmc skeleton_io_uring skeleton_kcov
USERTEST_APPS = usertest_msqueue usertest_vyukhov usertest_folly_spsc usertest_ck_fifo_spsc usertest_ck_ring_spsc usertest_ck_stack_upmc
APPS = $(BPF_APPS) $(USERTEST_APPS)

BINARIES := $(patsubst %,$(OUT_DIR)/%,$(APPS))

# ============================================================================
# CLANG BPF SYSTEM INCLUDES
# ============================================================================
CLANG_BPF_SYS_INCLUDES ?= $(shell $(CLANG) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

# ============================================================================
# VERBOSE BUILD CONTROL
# ============================================================================
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
all: $(BINARIES)
	@echo ""
	@echo "Built $(words $(BINARIES)) binaries into $(OUT_DIR)/"
	@echo "Next: sudo python3 scripts/run_all.py"

.PHONY: clean
clean:
	$(call msg,CLEAN)
	$(Q)rm -rf $(OUTPUT) $(OUT_DIR)

$(OUTPUT) $(OUTPUT)/libbpf $(BPFTOOL_OUTPUT) $(OUT_DIR):
	$(call msg,MKDIR,$@)
	$(Q)mkdir -p $@

# ============================================================================
# DEPENDENCY BUILDS
# ============================================================================

$(LIBBPF_OBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(OUTPUT)/libbpf
	$(call msg,LIB,$@)
	$(Q)$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1		      \
		    OBJDIR=$(dir $@)/libbpf DESTDIR=$(dir $@)		      \
		    INCLUDEDIR= LIBDIR= UAPIDIR=			      \
		    install

$(BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(call msg,BPFTOOL,$@)
	$(Q)$(MAKE) ARCH= CROSS_COMPILE= OUTPUT=$(BPFTOOL_OUTPUT)/ -C $(BPFTOOL_SRC) bootstrap

# ============================================================================
# BPF PROGRAM COMPILATION
# ============================================================================

$(OUTPUT)/%.bpf.o: src/%.bpf.c $(LIBBPF_OBJ) $(wildcard include/*.h) $(VMLINUX) | $(OUTPUT) $(BPFTOOL)
	$(call msg,BPF,$@)
	$(Q)$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -D__BPF_FEATURE_ADDR_SPACE_CAST	      \
		     $(INCLUDES) $(CLANG_BPF_SYS_INCLUDES)		      \
		     -c $(filter %.c,$^) -o $(patsubst %.bpf.o,%.tmp.bpf.o,$@)
	$(Q)$(BPFTOOL) gen object $@ $(patsubst %.bpf.o,%.tmp.bpf.o,$@)

# ============================================================================
# BPF SKELETON GENERATION
# ============================================================================

$(OUTPUT)/%.skel.h: $(OUTPUT)/%.bpf.o | $(OUTPUT) $(BPFTOOL)
	$(call msg,GEN-SKEL,$@)
	$(Q)$(BPFTOOL) gen skeleton $< > $@

# ============================================================================
# USERSPACE COMPILATION
# ============================================================================

$(patsubst %,$(OUTPUT)/%.o,$(BPF_APPS)): %.o: %.skel.h

$(OUTPUT)/%.o: src/%.c $(wildcard include/*.h) | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

$(OUTPUT)/%.o: usertest/%.c $(wildcard include/*.h) usertest/usertest_common.h | $(OUTPUT)
	$(call msg,CC,$@)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

# ============================================================================
# BINARY LINKING
# ============================================================================

$(OUT_DIR)/%: $(OUTPUT)/%.o $(LIBBPF_OBJ) | $(OUTPUT) $(OUT_DIR)
	$(call msg,BINARY,$@)
	$(Q)$(CC) $(CFLAGS) $^ $(ALL_LDFLAGS) -lelf -lz -lpthread -o $@

$(APPS): %: $(OUT_DIR)/%

.PHONY: usertest
usertest: $(patsubst %,$(OUT_DIR)/%,$(USERTEST_APPS))

# ============================================================================
# DISASSEMBLY
# ============================================================================

.PHONY: asm asm-user
asm: $(patsubst %,$(OUT_DIR)/%.S,$(BPF_APPS))
asm-user: $(patsubst %,$(OUT_DIR)/%.S,$(USERTEST_APPS))

$(OUT_DIR)/%.S: $(OUT_DIR)/% | $(OUT_DIR)
	$(call msg,ASM,$@)
	$(Q)$(OBJDUMP) -d -M intel -S $< > $@

# ============================================================================
# HELP TARGET
# ============================================================================

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all        Build all relays and usertests into $(OUT_DIR)/ (default)"
	@echo "  usertest   Build only the userspace-only pthread tests"
	@echo "  asm        Disassemble the relays into $(OUT_DIR)/*.S"
	@echo "  clean      Remove $(OUT_DIR)/ and .output/"
	@echo ""
	@echo "  V=1        Verbose build output"
	@echo ""
	@echo "Build unprivileged, then run the pipeline (it compiles nothing):"
	@echo "  make && sudo python3 scripts/run_all.py"

# ============================================================================
# MAKE DIRECTIVES
# ============================================================================

.DELETE_ON_ERROR:

.SECONDARY:
