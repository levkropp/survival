# Survival Workstation - Build System
# Target: AArch64 UEFI application

CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

# GNU-EFI paths — use local build in tools/
EFI_INC  := tools/gnu-efi/include
EFI_LIB  := tools/gnu-efi/lib
EFI_CRT  := $(EFI_LIB)/crt0-efi-aarch64.o
EFI_LDS  := $(EFI_LIB)/elf_aarch64_efi.lds

# Match gnu-efi's own build flags for aarch64
CFLAGS   := -ffreestanding -fno-stack-protector -fno-stack-check \
            -fshort-wchar -mstrict-align -fPIC -fPIE \
            -fno-strict-aliasing -fno-merge-all-constants \
            -I$(EFI_INC) -I$(EFI_INC)/aarch64 -Isrc \
            -Wall -Wextra -O2

LDFLAGS  := -nostdlib -Bsymbolic -pie \
            --no-dynamic-linker \
            -z common-page-size=4096 -z max-page-size=4096 \
            -z norelro -z nocombreloc \
            -T $(EFI_LDS)

LIBGCC   := $(shell $(CC) -print-libgcc-file-name)

SRCDIR   := src
BUILDDIR := build

SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/fb.c $(SRCDIR)/kbd.c $(SRCDIR)/mem.c $(SRCDIR)/font.c
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET   := $(BUILDDIR)/survival.efi
SO       := $(BUILDDIR)/survival.so
ESP_DIR  := $(BUILDDIR)/esp/EFI/BOOT

.PHONY: all clean info esp

all: $(TARGET) esp

info:
	@echo "EFI_INC  = $(EFI_INC)"
	@echo "EFI_LIB  = $(EFI_LIB)"
	@echo "EFI_CRT  = $(EFI_CRT)"
	@echo "EFI_LDS  = $(EFI_LDS)"
	@echo "LIBGCC   = $(LIBGCC)"

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SO): $(OBJECTS)
	$(LD) $(LDFLAGS) -L$(EFI_LIB) $(EFI_CRT) $(OBJECTS) -o $@ -lefi -lgnuefi $(LIBGCC)

$(TARGET): $(SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
		-j .rel -j .rela -j .reloc \
		--target=efi-app-aarch64 $< $@
	@echo ""
	@echo "=== Built: $@ ==="
	@ls -lh $@

esp: $(TARGET)
	@mkdir -p $(ESP_DIR)
	cp $(TARGET) $(ESP_DIR)/BOOTAA64.EFI
	@echo "ESP directory ready at $(BUILDDIR)/esp/"

clean:
	rm -rf $(BUILDDIR)
