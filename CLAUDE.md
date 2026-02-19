# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

SuperBoot is a UEFI bootloader (x86_64) written in C using gnu-efi. It natively parses other bootloaders' config files (grub.cfg, loader.conf, limine.cfg) to boot kernels directly, without chain-loading.

## Build Commands

```bash
make                  # Build superboot.efi (requires gnu-efi: pacman -S gnu-efi-libs)
make clean            # Remove build artifacts
make image            # Build + create FAT32 disk image (build/superboot.img)
make qemu             # Build + launch in QEMU with OVMF firmware
```

Override gnu-efi paths if non-standard: `make EFI_INC=/path/to/efi EFI_LIB=/path/to/lib`

## Architecture

The core abstraction is `BootTarget` (defined in `src/superboot.h`) — every config parser produces this struct, and every kernel loader consumes it. Parsers and loaders never interact directly.

**Key layers (bottom to top):**
- UEFI protocols (SimpleFileSystem, BlockIO, GOP)
- VFS (`src/fs/`) — FAT32 via UEFI native, ext4 built-in, btrfs/xfs/ntfs stubs
- Config parsers (`src/config/`) — each implements the `ConfigParser` vtable
- Scanner (`src/scan/`) — enumerates block devices, feeds configs to parsers
- Kernel loaders (`src/boot/`) — Linux EFI handover + legacy bzImage + chainload
- TUI (`src/tui/`) — boot menu + file browser using UEFI ConOut
- Deployment (`src/deploy/`) — non-destructive install to internal ESP

**GRUB parser (`src/config/grub.c`)** does selective extraction, not full script interpretation: tracks variables, extracts menuentry blocks, handles linux/initrd/search/chainloader commands, skips if/for/function.

**Linux boot (`src/boot/linux.c`)** implements two paths: EFI handover protocol (preferred for kernel ≥ 3.7) and legacy bzImage (fallback with ExitBootServices + E820 conversion).

## Key Constraints

- Freestanding C: no libc, no POSIX. Only gnu-efi and custom utilities (`src/util/`).
- All strings use UEFI types: `CHAR16` for paths/display, `CHAR8` for ASCII (config files, kernel cmdline).
- After `ExitBootServices()`, no memory allocation or UEFI calls are possible.
- Kernel command lines and config files are ASCII (`CHAR8`); UEFI paths are wide (`CHAR16`).
- All filesystem access goes through the VFS layer — never use SimpleFileSystem directly from parsers.
