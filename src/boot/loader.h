/*
 * loader.h — Kernel loader interface and Linux boot protocol structures
 *
 * This header defines the Linux x86 boot protocol structures needed
 * to hand off control from a UEFI bootloader to a Linux kernel.
 * Reference: Linux Documentation/arch/x86/boot.rst
 */

#ifndef SUPERBOOT_LOADER_H
#define SUPERBOOT_LOADER_H

#include "../superboot.h"

/* ------------------------------------------------------------------ */
/*  Linux boot protocol constants                                      */
/* ------------------------------------------------------------------ */

#define LINUX_BOOT_HDR_MAGIC     0x53726448  /* "HdrS" */
#define LINUX_BOOT_FLAG          0xAA55
#define LINUX_PE_MAGIC           0x5A4D      /* "MZ" for EFI stub */

/* Minimum boot protocol version we support (2.06+ for EFI handover). */
#define LINUX_MIN_BOOT_VERSION   0x0206

/* Loadflags bits */
#define LINUX_LOAD_HIGH          0x01  /* Kernel can be loaded high    */
#define LINUX_CAN_USE_HEAP       0x80  /* Boot loader provides heap    */

/* Boot loader ID: we use 0xFF (undefined). */
#define SUPERBOOT_LOADER_ID      0xFF

/* EFI handover protocol offsets. */
#define LINUX_EFI_HANDOVER_32    0x190
#define LINUX_EFI_HANDOVER_64    0x1C8

/* ------------------------------------------------------------------ */
/*  Linux setup header (at offset 0x1F1 in the bzImage)                */
/*                                                                     */
/*  Only the fields we actually read/write are included.               */
/* ------------------------------------------------------------------ */

#pragma pack(1)

typedef struct {
    UINT8   setup_sects;        /* 0x1F1: # of setup sectors          */
    UINT16  root_flags;         /* 0x1F2                              */
    UINT32  syssize;            /* 0x1F4                              */
    UINT16  ram_size;           /* 0x1F8                              */
    UINT16  vid_mode;           /* 0x1FA                              */
    UINT16  root_dev;           /* 0x1FC                              */
    UINT16  boot_flag;          /* 0x1FE: must be 0xAA55              */
    /* --- offset 0x200 --- */
    UINT16  jump;               /* 0x200: jump instruction            */
    UINT32  header;             /* 0x202: "HdrS" magic                */
    UINT16  version;            /* 0x206: boot protocol version       */
    UINT32  realmode_swtch;     /* 0x208                              */
    UINT16  start_sys_seg;      /* 0x20C                              */
    UINT16  kernel_version;     /* 0x20E                              */
    UINT8   type_of_loader;     /* 0x210                              */
    UINT8   loadflags;          /* 0x211                              */
    UINT16  setup_move_size;    /* 0x212                              */
    UINT32  code32_start;       /* 0x214                              */
    UINT32  ramdisk_image;      /* 0x218: initrd physical address     */
    UINT32  ramdisk_size;       /* 0x21C: initrd size                 */
    UINT32  bootsect_kludge;    /* 0x220                              */
    UINT16  heap_end_ptr;       /* 0x224                              */
    UINT8   ext_loader_ver;     /* 0x226                              */
    UINT8   ext_loader_type;    /* 0x227                              */
    UINT32  cmd_line_ptr;       /* 0x228: cmdline physical address    */
    UINT32  initrd_addr_max;    /* 0x22C                              */
    UINT32  kernel_alignment;   /* 0x230                              */
    UINT8   relocatable_kernel; /* 0x234                              */
    UINT8   min_alignment;      /* 0x235                              */
    UINT16  xloadflags;         /* 0x236                              */
    UINT32  cmdline_size;       /* 0x238                              */
    UINT32  hardware_subarch;   /* 0x23C                              */
    UINT64  hardware_subarch_data; /* 0x240                           */
    UINT32  payload_offset;     /* 0x248                              */
    UINT32  payload_length;     /* 0x24C                              */
    UINT64  setup_data;         /* 0x250                              */
    UINT64  pref_address;       /* 0x258                              */
    UINT32  init_size;          /* 0x260                              */
    UINT32  handover_offset;    /* 0x264: EFI handover entry offset   */
} LinuxSetupHeader;

/*
 * Minimal struct boot_params ("zero page").
 * The full structure is 4096 bytes; we define only what we touch.
 * The rest is zero-filled.
 */
typedef struct {
    UINT8             screen_info[64];   /* 0x000 */
    UINT8             _pad1[0x1C0 - 64];
    UINT8             e820_entries;       /* 0x1E8 */
    UINT8             _pad2[0x1F1 - 0x1E9];
    LinuxSetupHeader  hdr;               /* 0x1F1 */
    UINT8             _pad3[4096 - 0x1F1 - sizeof(LinuxSetupHeader)];
} LinuxBootParams;

/* E820 memory map entry. */
typedef struct {
    UINT64  addr;
    UINT64  size;
    UINT32  type;  /* 1=RAM, 2=Reserved, 3=ACPI reclaimable, etc. */
} __attribute__((packed)) E820Entry;

#pragma pack()

/* Sanity check. */
_Static_assert(sizeof(LinuxBootParams) == 4096,
               "boot_params must be exactly 4096 bytes");

/* ------------------------------------------------------------------ */
/*  EFI memory map → E820 conversion                                   */
/* ------------------------------------------------------------------ */

UINTN sb_efi_memmap_to_e820(
    EFI_MEMORY_DESCRIPTOR *mmap, UINTN mmap_size, UINTN desc_size,
    E820Entry *e820, UINTN max_entries);

#endif /* SUPERBOOT_LOADER_H */
