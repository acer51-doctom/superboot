# SuperBoot Architecture

## Overview

SuperBoot is a UEFI bootloader that **natively parses** other bootloaders'
configuration files (grub.cfg, loader.conf, limine.cfg) and boots the
referenced kernels directly — without chain-loading the original bootloader.

```
┌─────────────────────────────────────────────────────────┐
│                     TUI Layer                           │
│              menu.c  ·  explorer.c                      │
├────────────┬──────────────────────────┬─────────────────┤
│   Scanner  │    Config Parsers        │  Deployment     │
│   scan.c   │  grub · sd-boot · limine │  deploy.c       │
├────────────┴──────────────────────────┴─────────────────┤
│                  BootTarget Abstraction                  │
│                     superboot.h                         │
├────────────┬────────────────────────────────────────────┤
│  VFS Layer │        Kernel Loaders                      │
│ vfs · ext4 │  linux.c (bzImage + EFI handover)          │
│ btrfs· xfs │  chain.c (fallback .efi chain-load)        │
│ ntfs       │                                            │
├────────────┴────────────────────────────────────────────┤
│              UEFI Boot Services / Protocols             │
│  SimpleFileSystem · BlockIO · DiskIO · GOP · ConOut     │
└─────────────────────────────────────────────────────────┘
```

## The BootTarget Abstraction

The central design decision: **every config parser produces the same struct**.

```c
typedef struct {
    CHAR16  title[256];
    CHAR16  kernel_path[512];
    CHAR16  initrd_paths[8][512];
    UINT32  initrd_count;
    CHAR8   cmdline[4096];
    EFI_HANDLE device_handle;
    ConfigType config_type;
    BOOLEAN is_chainload;
    CHAR16  efi_path[512];
    // ...
} BootTarget;
```

This struct is the **only interface** between parsing and booting.  A parser
never invokes a loader, and a loader never reads a config file.  They
communicate exclusively through BootTarget.

## Config Parser → BootTarget Pipeline

```
  grub.cfg  ──→  grub.c::grub_parse()     ──┐
  loader/*  ──→  systemd_boot.c::parse()   ──┼──→  BootTargetList  ──→  TUI Menu
  limine.cfg ──→  limine.c::limine_parse() ──┘                          │
                                                                          ▼
                                                              sb_boot_linux() or
                                                              sb_chainload_efi()
```

Each parser implements the `ConfigParser` vtable (see `config.h`):

- `config_paths[]` — filesystem paths to probe (e.g., `\boot\grub\grub.cfg`)
- `parse()` — takes raw file bytes, produces BootTarget array

The scanner (`scan.c`) iterates all block devices, tries each parser's
probe paths, and accumulates results in `ctx->targets`.

## The GRUB "Transpiler"

GRUB's `grub.cfg` is a full scripting language.  SuperBoot does NOT
fully interpret it.  Instead it performs **selective extraction**:

| GRUB Construct           | SuperBoot Handling                       |
|--------------------------|------------------------------------------|
| `set var=value`          | Recorded in a variable table             |
| `menuentry 'T' { ... }` | Opens a new BootTarget                   |
| `linux /path args...`   | Sets kernel_path + cmdline               |
| `initrd /path`          | Appends to initrd_paths                  |
| `search --set=root ...` | Sets $root variable                      |
| `chainloader /path.efi` | Marks entry as chainload                 |
| `$variable` / `${var}`  | Expanded lazily at path-build time       |
| `(hdN,gptM)/path`       | Device prefix stripped; handle from scan  |
| `if` / `for` / `function` | **Skipped** (brace-depth tracked)      |
| `submenu`                | Treated like `menuentry` recursively     |

This covers >95% of `grub-mkconfig` output.  The remaining edge cases
(computed paths, sourced scripts) gracefully degrade: the entry appears
in the menu but may fail to boot, at which point the user can edit the
command line or use the file explorer.

## Linux Boot Protocol

Two paths, selected automatically:

### EFI Handover (preferred, kernel ≥ 3.7)

1. Parse the bzImage setup header at offset 0x1F1
2. Allocate `LinuxBootParams` (zero page, 4096 bytes)
3. Copy setup header; set loader ID, command line, initrd address
4. Compute handover entry: `kernel_base + 512 + hdr.handover_offset`
5. Jump — the kernel's EFI stub calls `ExitBootServices` itself

### Legacy bzImage (fallback)

1. Same setup header parsing
2. Copy protected-mode kernel to `pref_address` (or relocate)
3. Allocate initrd below 4 GiB
4. Call `GetMemoryMap` → convert to E820 → fill boot_params
5. `ExitBootServices` (tight retry loop for stale map key)
6. Jump to 64-bit entry with boot_params in RSI

## Memory Map Management

The ExitBootServices hand-off is the most delicate operation:

```
GetMemoryMap()           ← get current map + map_key
  (no allocations here!)
ExitBootServices(key)    ← if key is stale, re-fetch and retry ONCE
  (point of no return)
jump to kernel
```

After ExitBootServices, no UEFI boot services are available.  The EFI
memory map is converted to Linux E820 format:

| EFI Type                | E820 Type      |
|-------------------------|----------------|
| ConventionalMemory      | RAM (1)        |
| LoaderCode/Data         | RAM (1)        |
| BootServicesCode/Data   | RAM (1)        |
| ACPIReclaimMemory       | ACPI (3)       |
| ACPIMemoryNVS           | NVS (4)        |
| Everything else         | Reserved (2)   |

Contiguous regions of the same type are merged.

## VFS Layer

Two-tier approach:

1. **UEFI-native**: If the partition has `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
   (always true for FAT32 ESPs, and for any FS with a loaded EFI driver),
   use it directly.

2. **Built-in drivers**: For partitions with only `EFI_BLOCK_IO_PROTOCOL`,
   probe built-in read-only drivers (ext4 implemented, btrfs/xfs/ntfs
   stubbed).

External `.efi` filesystem drivers can be placed in
`\EFI\superboot\drivers\` on the SuperBoot ESP — they're loaded at
startup and automatically bind to partitions.

## Boot Flow

```
efi_main()
  ├── sb_init_context()         — parse our own cmdline
  ├── sb_vfs_init()             — load external FS drivers
  ├── sb_scan_all_devices()     — enumerate Block I/O handles
  │     └── for each partition:
  │           ├── sb_vfs_open_device()
  │           └── for each ConfigParser:
  │                 ├── check config_paths[]
  │                 └── parser->parse() → BootTargets
  ├── sb_tui_run_menu()         — arrow keys, countdown, edit cmdline
  │     ├── [e] edit cmdline
  │     ├── [f] file browser
  │     └── [d] deploy to ESP
  └── sb_boot_selected()
        ├── sb_boot_linux()     — EFI handover or legacy bzImage
        └── sb_chainload_efi()  — LoadImage + StartImage
```
