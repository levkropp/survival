# Survival Workstation - Build System (TCC)
# Supports: ARCH=aarch64 (default) or ARCH=x86_64
# Uses TCC cross-compilers to produce UEFI PE binaries directly.
# No GCC or gnu-efi dependency for the build itself.

ARCH     ?= aarch64
SRCDIR   := src

# ---- Architecture-specific settings ----
ifeq ($(ARCH),aarch64)
  TCC         := tools/tcc-host/arm64-tcc
  EFI_BOOT    := BOOTAA64.EFI
  TCC_TARGET  := -DTCC_TARGET_ARM64=1
  SETJMP_SRC  := $(SRCDIR)/setjmp_aarch64.c
  # No extra undefines needed — arm64-tcc doesn't predefine _WIN32
  TCC_UNDEF   :=
else ifeq ($(ARCH),x86_64)
  TCC         := tools/tcc-host/x86_64-win32-tcc
  EFI_BOOT    := BOOTX64.EFI
  TCC_TARGET  := -DTCC_TARGET_X86_64=1
  SETJMP_SRC  := $(SRCDIR)/setjmp_x86_64.S
  # x86_64-win32-tcc predefines _WIN32/_WIN64; suppress for UEFI
  TCC_UNDEF   := -U_WIN32 -U_WIN64
else
  $(error Unsupported ARCH=$(ARCH). Use aarch64 or x86_64)
endif

TCC_DIR  := tools/tinycc
BUILDDIR := build/$(ARCH)

# Flags for workstation source files
CFLAGS   := -nostdlib -nostdinc \
            -Isrc/tcc-headers -Isrc -I$(TCC_DIR) \
            -Wall

# Flags for TCC unity build (libtcc.c — TCC compiling itself)
TCC_CFLAGS := -nostdlib -nostdinc -w \
              $(TCC_UNDEF) \
              -Isrc/tcc-headers -I$(TCC_DIR) \
              -DONE_SOURCE=1 $(TCC_TARGET) -D__UEFI__

SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c $(SRCDIR)/mem.c $(SRCDIR)/font.c \
            $(SRCDIR)/fs.c $(SRCDIR)/browse.c $(SRCDIR)/edit.c \
            $(SRCDIR)/shim.c $(SRCDIR)/tcc.c \
            $(SRCDIR)/disk.c $(SRCDIR)/fat32.c \
            $(SRCDIR)/iso.c \
            $(SRCDIR)/exfat.c $(SRCDIR)/ntfs.c
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
OBJECTS  += $(BUILDDIR)/setjmp.o $(BUILDDIR)/libtcc.o

TARGET   := $(BUILDDIR)/survival.efi
ESP_DIR  := $(BUILDDIR)/esp/EFI/BOOT
INC_DIR  := $(BUILDDIR)/esp/include

.PHONY: all clean info esp copy-sources copy-headers all-arches

all: $(TARGET) esp copy-sources copy-headers

all-arches:
	$(MAKE) ARCH=aarch64
	$(MAKE) ARCH=x86_64

info:
	@echo "ARCH     = $(ARCH)"
	@echo "TCC      = $(TCC)"
	@echo "BUILDDIR = $(BUILDDIR)"

# Workstation source files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(TCC) $(CFLAGS) -c -o $@ $<

# setjmp — arch-specific (.S for x86_64, .c for aarch64)
$(BUILDDIR)/setjmp.o: $(SETJMP_SRC)
	@mkdir -p $(BUILDDIR)
	$(TCC) -c -o $@ $<

# TCC unity build — libtcc.c includes all TCC .c files via ONE_SOURCE.
# TCC's -c -o is incompatible with ONE_SOURCE (counts included .c as
# multiple files), so we compile without -o and move the result.
$(BUILDDIR)/libtcc.o: $(TCC_DIR)/libtcc.c $(TCC_DIR)/tcc.h $(TCC_DIR)/config.h
	@mkdir -p $(BUILDDIR)
	$(TCC) $(TCC_CFLAGS) -c $<
	@mv libtcc.o $@

# Link all objects into UEFI PE binary.
# -shared generates relocations (required by UEFI firmware).
$(TARGET): $(OBJECTS)
	$(TCC) -nostdlib -shared \
		-Wl,-subsystem=efiapp -Wl,-e=efi_main \
		-o $@ $(OBJECTS)
	@echo ""
	@echo "=== Built: $@ ($(ARCH)) ==="
	@ls -lh $@

esp: $(TARGET)
	@mkdir -p $(ESP_DIR)
	cp $(TARGET) $(ESP_DIR)/$(EFI_BOOT)
	@echo "ESP directory ready at $(BUILDDIR)/esp/"

# Source files and headers for self-hosting (F6 rebuild)
copy-sources:
	@mkdir -p $(BUILDDIR)/esp/src/tcc-headers/sys
	@cp src/*.c src/*.h src/*.S $(BUILDDIR)/esp/src/
	@cp -r src/tcc-headers/* $(BUILDDIR)/esp/src/tcc-headers/
	@mkdir -p $(BUILDDIR)/esp/tools/tinycc
	@cp tools/tinycc/libtcc.c tools/tinycc/libtcc.h \
	    tools/tinycc/tcc.h tools/tinycc/config.h tools/tinycc/tccdefs_.h \
	    tools/tinycc/tccpp.c tools/tinycc/tccgen.c tools/tinycc/tccdbg.c \
	    tools/tinycc/tccasm.c tools/tinycc/tccelf.c tools/tinycc/tccrun.c \
	    tools/tinycc/tccpe.c \
	    tools/tinycc/elf.h tools/tinycc/stab.h tools/tinycc/stab.def \
	    tools/tinycc/dwarf.h tools/tinycc/tcctok.h \
	    $(BUILDDIR)/esp/tools/tinycc/
ifeq ($(ARCH),aarch64)
	@cp tools/tinycc/arm64-gen.c tools/tinycc/arm64-link.c tools/tinycc/arm64-asm.c \
	    tools/tinycc/arm-tok.h \
	    $(BUILDDIR)/esp/tools/tinycc/
else
	@cp tools/tinycc/x86_64-gen.c tools/tinycc/x86_64-link.c tools/tinycc/i386-asm.c \
	    tools/tinycc/i386-tok.h tools/tinycc/x86_64-asm.h \
	    $(BUILDDIR)/esp/tools/tinycc/
endif

# User-facing headers and example programs for TCC
copy-headers:
	@mkdir -p $(INC_DIR)
	@cp src/user-headers/*.h $(INC_DIR)/ 2>/dev/null || true
	@cp src/user-headers/*.c $(BUILDDIR)/esp/ 2>/dev/null || true
	@echo "User headers installed to $(INC_DIR)/"

clean:
	rm -rf build
