# ======================================================================
#  SuperBoot Makefile — builds a UEFI application using gnu-efi
# ======================================================================
#
#  Prerequisites:
#    - gcc (or clang) targeting x86_64
#    - gnu-efi headers + libraries:
#        Arch:   pacman -S gnu-efi-libs
#        Debian: apt install gnu-efi
#        Fedora: dnf install gnu-efi gnu-efi-devel
#
#  Targets:
#    make            — build superboot.efi
#    make clean      — remove build artifacts
#    make image      — build + create a bootable USB disk image
#    make qemu       — build + run under QEMU with OVMF firmware
#

# ---- Toolchain -------------------------------------------------------

CC       ?= gcc
LD       ?= ld
OBJCOPY  ?= objcopy

# ---- gnu-efi paths (auto-detected, override if non-standard) --------

EFI_INC  ?= /usr/include/efi
EFI_LIB  ?= /usr/lib
EFI_CRT  ?= $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS  ?= $(EFI_LIB)/elf_x86_64_efi.lds

# ---- Build directories -----------------------------------------------

SRCDIR   := src
BUILDDIR := build
OBJDIR   := $(BUILDDIR)/obj

# ---- Source files -----------------------------------------------------

SOURCES := \
	$(SRCDIR)/main.c \
	$(SRCDIR)/config/config.c \
	$(SRCDIR)/config/grub.c \
	$(SRCDIR)/config/systemd_boot.c \
	$(SRCDIR)/config/limine.c \
	$(SRCDIR)/fs/vfs.c \
	$(SRCDIR)/fs/ext4.c \
	$(SRCDIR)/fs/btrfs.c \
	$(SRCDIR)/fs/xfs.c \
	$(SRCDIR)/fs/ntfs.c \
	$(SRCDIR)/boot/linux.c \
	$(SRCDIR)/boot/chain.c \
	$(SRCDIR)/scan/scan.c \
	$(SRCDIR)/tui/menu.c \
	$(SRCDIR)/tui/explorer.c \
	$(SRCDIR)/deploy/deploy.c \
	$(SRCDIR)/util/string.c \
	$(SRCDIR)/util/memory.c

OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

# ---- Compiler flags ---------------------------------------------------

CFLAGS := \
	-std=gnu11 \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-fshort-wchar \
	-mno-red-zone \
	-maccumulate-outgoing-args \
	-Wall -Wextra -Werror \
	-Wno-unused-parameter \
	-O2 \
	-I$(EFI_INC) \
	-I$(EFI_INC)/x86_64 \
	-I$(SRCDIR) \
	-DEFI_FUNCTION_WRAPPER \
	-DGNU_EFI_USE_MS_ABI

# ---- Linker flags -----------------------------------------------------

LDFLAGS := \
	-nostdlib \
	-znocombreloc \
	-T $(EFI_LDS) \
	-shared \
	-Bsymbolic \
	-L$(EFI_LIB) \
	$(EFI_CRT)

LIBS := -lgnuefi -lefi

# ---- Targets ----------------------------------------------------------

TARGET_SO  := $(BUILDDIR)/superboot.so
TARGET_EFI := $(BUILDDIR)/superboot.efi

.PHONY: all clean image qemu

all: $(TARGET_EFI)

$(TARGET_EFI): $(TARGET_SO)
	$(OBJCOPY) \
		-j .text -j .sdata -j .data -j .rodata \
		-j .dynamic -j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-x86_64 \
		--subsystem=10 \
		$< $@
	@echo "==> Built $@ ($(shell stat -c%s $@ 2>/dev/null || echo '?') bytes)"

$(TARGET_SO): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

# ---- Disk image (FAT32 ESP) ------------------------------------------

IMAGE     := $(BUILDDIR)/superboot.img
IMAGE_SIZE := 64  # MiB

image: $(TARGET_EFI)
	dd if=/dev/zero of=$(IMAGE) bs=1M count=$(IMAGE_SIZE) 2>/dev/null
	mkfs.fat -F 32 $(IMAGE)
	mmd -i $(IMAGE) ::/EFI ::/EFI/BOOT
	mcopy -i $(IMAGE) $(TARGET_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@echo "==> Disk image: $(IMAGE)"
	@echo "    Write to USB: sudo dd if=$(IMAGE) of=/dev/sdX bs=4M status=progress"

# ---- QEMU testing (requires OVMF) ------------------------------------

OVMF ?= /usr/share/edk2/x64/OVMF.fd
ifeq ($(wildcard $(OVMF)),)
  OVMF := /usr/share/OVMF/OVMF_CODE.fd
endif

qemu: image
	qemu-system-x86_64 \
		-bios $(OVMF) \
		-drive file=$(IMAGE),format=raw \
		-net none \
		-m 512M \
		-serial stdio
