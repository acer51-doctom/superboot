# SuperBoot

A UEFI bootloader (x86_64) that natively parses other bootloaders' configuration files and boots kernels directly, without chain-loading.

SuperBoot reads GRUB (`grub.cfg`), systemd-boot (`loader.conf`), and Limine (`limine.cfg`) configs, extracts boot entries, and presents them in a unified TUI menu.

## Features

- **Multi-format config parsing** -- GRUB, systemd-boot, and Limine configs are parsed natively with GRUB variable expansion support
- **Linux boot protocol** -- EFI handover (kernel >= 3.7) and legacy bzImage with E820 memory map conversion
- **EFI chainloading** -- fallback to `LoadImage`/`StartImage` for `.efi` binaries
- **VFS layer** -- FAT32 via UEFI native, built-in read-only ext4, stubs for btrfs/xfs/ntfs
- **TUI** -- boot menu with countdown, inline command-line editing, and file browser
- **Non-destructive install** -- deploy to internal ESP without modifying existing boot entries
- **Device scanning** -- automatic enumeration of all block devices and partitions

## Building

Requires [gnu-efi](https://sourceforge.net/projects/gnu-efi/) headers and libraries.

```bash
# Install dependencies
# Arch:   pacman -S gnu-efi-libs
# Debian: apt install gnu-efi
# Fedora: dnf install gnu-efi gnu-efi-devel

make                  # Build superboot.efi
make clean            # Remove build artifacts
make image            # Build + create FAT32 disk image (build/superboot.img)
make qemu             # Build + launch in QEMU with OVMF firmware
```

Override gnu-efi paths if non-standard:

```bash
make EFI_INC=/path/to/efi EFI_LIB=/path/to/lib
```

## Usage

### USB boot

```bash
make image
sudo dd if=build/superboot.img of=/dev/sdX bs=4M status=progress
```

Boot from the USB drive. SuperBoot will scan all connected devices for known bootloader configs and display a menu.

### QEMU testing

```bash
make qemu
```

Requires OVMF firmware (`/usr/share/edk2/x64/OVMF.fd` or `/usr/share/OVMF/OVMF_CODE.fd`).

### TUI controls

| Key       | Action                         |
|-----------|--------------------------------|
| Up/Down   | Navigate boot entries          |
| Enter     | Boot selected entry            |
| `e`       | Edit kernel command line        |
| `f`       | Open file browser              |
| `d`       | Deploy SuperBoot to internal ESP|

## Architecture

```
+-----------------------------------------------------------+
|                      TUI Layer                            |
|               menu.c  -  explorer.c                       |
+-------------+----------------------------+----------------+
|   Scanner   |     Config Parsers         |  Deployment    |
|   scan.c    |  grub - sd-boot - limine   |  deploy.c      |
+-------------+----------------------------+----------------+
|                 BootTarget Abstraction                     |
|                    superboot.h                            |
+-------------+---------------------------------------------+
|  VFS Layer  |         Kernel Loaders                      |
| vfs - ext4  |  linux.c (bzImage + EFI handover)           |
| btrfs - xfs |  chain.c (fallback .efi chain-load)         |
+-------------+---------------------------------------------+
|             UEFI Boot Services / Protocols                |
+-----------------------------------------------------------+
```

The central abstraction is `BootTarget` (defined in `src/superboot.h`). Every config parser produces this struct, every kernel loader consumes it. Parsers and loaders never interact directly.

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed design documentation.

## Project structure

```
src/
  main.c              Entry point and boot flow orchestration
  superboot.h         Core types (BootTarget, SuperBootContext)
  config/             Config parsers (GRUB, systemd-boot, Limine)
  boot/               Kernel loaders (Linux EFI handover, legacy bzImage, chainload)
  fs/                 VFS layer (FAT32 native, ext4 built-in, stubs)
  scan/               Block device enumeration
  tui/                Boot menu and file browser
  deploy/             Non-destructive ESP installation
  util/               String and memory utilities
```

## License

GPLv2. See [LICENSE.txt](LICENSE.txt).
