/*
 * superboot.h — Core types and declarations for SuperBoot
 *
 * Every module includes this header. It pulls in the EFI environment
 * and defines the universal BootTarget that bridges config parsers,
 * filesystem drivers, and kernel loaders.
 */

#ifndef SUPERBOOT_H
#define SUPERBOOT_H

#include <efi.h>
#include <efilib.h>

/* ------------------------------------------------------------------ */
/*  Build-time limits                                                  */
/* ------------------------------------------------------------------ */

#define SB_MAX_TARGETS        64   /* max boot entries across all configs */
#define SB_MAX_INITRDS         8   /* max initrd images per entry        */
#define SB_MAX_PATH          512
#define SB_MAX_TITLE         256
#define SB_MAX_CMDLINE      4096
#define SB_MAX_VARS          128   /* GRUB variable table size           */
#define SB_MAX_VAR_NAME       64
#define SB_MAX_VAR_VALUE     512

/* ------------------------------------------------------------------ */
/*  Config source types                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    CONFIG_TYPE_UNKNOWN = 0,
    CONFIG_TYPE_GRUB,            /* /boot/grub/grub.cfg              */
    CONFIG_TYPE_SYSTEMD_BOOT,    /* /loader/loader.conf + entries/   */
    CONFIG_TYPE_LIMINE,          /* limine.cfg                       */
} ConfigType;

/* ------------------------------------------------------------------ */
/*  BootTarget — the universal "parsed boot entry"                     */
/*                                                                     */
/*  Every config parser produces an array of these.  The kernel        */
/*  loader consumes them.  This struct is the central abstraction      */
/*  that decouples parsing from booting.                               */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Human-readable label shown in the TUI menu. */
    CHAR16      title[SB_MAX_TITLE];

    /* Absolute paths on the source filesystem. */
    CHAR16      kernel_path[SB_MAX_PATH];
    CHAR16      initrd_paths[SB_MAX_INITRDS][SB_MAX_PATH];
    UINT32      initrd_count;

    /* Kernel command line (ASCII, as the Linux protocol requires). */
    CHAR8       cmdline[SB_MAX_CMDLINE];

    /* Where this entry came from. */
    CHAR16      config_path[SB_MAX_PATH];
    ConfigType  config_type;

    /* UEFI handle of the block device / partition. */
    EFI_HANDLE  device_handle;

    /* If TRUE, this entry should chain-load an .efi instead. */
    BOOLEAN     is_chainload;
    CHAR16      efi_path[SB_MAX_PATH];

    /* Ordering hint (0 = default entry). */
    UINT32      index;
    BOOLEAN     is_default;
} BootTarget;

/* ------------------------------------------------------------------ */
/*  BootTargetList — collected results from scanning                   */
/* ------------------------------------------------------------------ */

typedef struct {
    BootTarget  entries[SB_MAX_TARGETS];
    UINTN       count;
} BootTargetList;

/* ------------------------------------------------------------------ */
/*  Global state passed through the system                             */
/* ------------------------------------------------------------------ */

typedef struct {
    EFI_HANDLE              image_handle;
    EFI_SYSTEM_TABLE       *system_table;
    EFI_BOOT_SERVICES      *boot_services;
    EFI_RUNTIME_SERVICES   *runtime_services;

    /* Collected boot targets from all scanned devices. */
    BootTargetList          targets;

    /* The target the user selected (index into targets.entries). */
    UINTN                   selected;

    /* Configuration: timeout in seconds, 0 = immediate boot. */
    UINT32                  timeout_sec;
    BOOLEAN                 verbose;
} SuperBootContext;

/* ------------------------------------------------------------------ */
/*  Convenience macros                                                 */
/* ------------------------------------------------------------------ */

#define SB_CHECK(status, msg) do {                              \
    EFI_STATUS _s = (status);                                   \
    if (EFI_ERROR(_s)) {                                        \
        Print(L"[SuperBoot] ERROR: %s: %r\n", (msg), _s);      \
        return _s;                                              \
    }                                                           \
} while (0)

#define SB_LOG(fmt, ...) \
    Print(L"[SuperBoot] " fmt L"\n", ##__VA_ARGS__)

#define SB_DBG(ctx, fmt, ...) do {                              \
    if ((ctx)->verbose)                                         \
        Print(L"[SuperBoot DBG] " fmt L"\n", ##__VA_ARGS__);   \
} while (0)

/* ------------------------------------------------------------------ */
/*  Module entry points (declared here, defined in each module)        */
/* ------------------------------------------------------------------ */

/* scan/scan.c */
EFI_STATUS sb_scan_all_devices(SuperBootContext *ctx);

/* config/config.c */
EFI_STATUS sb_parse_configs(SuperBootContext *ctx, EFI_HANDLE device);

/* boot/linux.c */
EFI_STATUS sb_boot_linux(SuperBootContext *ctx, const BootTarget *target);

/* boot/chain.c */
EFI_STATUS sb_chainload_efi(SuperBootContext *ctx, const BootTarget *target);

/* tui/menu.c */
EFI_STATUS sb_tui_run_menu(SuperBootContext *ctx);

/* tui/explorer.c */
EFI_STATUS sb_tui_file_browser(SuperBootContext *ctx);

/* deploy/deploy.c */
EFI_STATUS sb_deploy_to_esp(SuperBootContext *ctx);

/* fs/vfs.c */
EFI_STATUS sb_vfs_init(SuperBootContext *ctx);
EFI_STATUS sb_vfs_read_file(EFI_HANDLE device, const CHAR16 *path,
                            void **buffer, UINTN *size);
void       sb_vfs_shutdown(void);

/* util/string.c */
INTN    sb_strcmp8(const CHAR8 *a, const CHAR8 *b);
INTN    sb_strncmp8(const CHAR8 *a, const CHAR8 *b, UINTN n);
UINTN   sb_strlen8(const CHAR8 *a);
CHAR8  *sb_strstr8(const CHAR8 *haystack, const CHAR8 *needle);
void    sb_strcpy8(CHAR8 *dst, const CHAR8 *src, UINTN max);
void    sb_str8to16(CHAR16 *dst, const CHAR8 *src, UINTN max);
void    sb_str16to8(CHAR8 *dst, const CHAR16 *src, UINTN max);
CHAR8  *sb_skip_whitespace(CHAR8 *p);
CHAR8  *sb_next_line(CHAR8 *p);
BOOLEAN sb_starts_with8(const CHAR8 *s, const CHAR8 *prefix);

/* util/memory.c */
void   *sb_alloc(EFI_BOOT_SERVICES *bs, UINTN size);
void   *sb_alloc_pages(EFI_BOOT_SERVICES *bs, UINTN pages,
                       EFI_PHYSICAL_ADDRESS preferred);
void    sb_free(EFI_BOOT_SERVICES *bs, void *p, UINTN size);
void    sb_free_pages(EFI_BOOT_SERVICES *bs, EFI_PHYSICAL_ADDRESS addr,
                      UINTN pages);

#endif /* SUPERBOOT_H */
