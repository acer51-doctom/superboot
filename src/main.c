/*
 * main.c — SuperBoot UEFI entry point
 *
 * Orchestrates the full boot flow:
 *   1. Initialise UEFI library and global context
 *   2. Initialise the VFS layer (load filesystem drivers)
 *   3. Scan every block device for known config files
 *   4. Present the TUI menu (or auto-boot on timeout)
 *   5. Load the selected kernel / chain-load .efi
 */

#include "superboot.h"

/* Forward declarations for local helpers. */
static EFI_STATUS sb_init_context(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                                  SuperBootContext *ctx);
static EFI_STATUS sb_boot_selected(SuperBootContext *ctx);

/* ================================================================== */
/*  EFI entry point                                                    */
/* ================================================================== */

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_STATUS        status;
    SuperBootContext   ctx;

    /* ---- Phase 0: Initialise ------------------------------------ */
    InitializeLib(image_handle, system_table);
    status = sb_init_context(image_handle, system_table, &ctx);
    if (EFI_ERROR(status))
        return status;

    SB_LOG(L"SuperBoot v0.1.0 — Universal Meta-Bootloader");
    SB_LOG(L"Firmware: %s  Rev %d",
           system_table->FirmwareVendor,
           system_table->FirmwareRevision);

    /* ---- Phase 1: Filesystem layer ------------------------------ */
    status = sb_vfs_init(&ctx);
    if (EFI_ERROR(status))
        SB_LOG(L"WARN: VFS init incomplete (%r), falling back to ESP-only", status);

    /* ---- Phase 2: Scan all block devices for boot configs ------- */
    status = sb_scan_all_devices(&ctx);
    if (EFI_ERROR(status) || ctx.targets.count == 0) {
        SB_LOG(L"No bootable entries found — launching EFI explorer.");
        sb_tui_file_browser(&ctx);
        return EFI_NOT_FOUND;
    }

    SB_LOG(L"Found %u bootable entries.", ctx.targets.count);

    /* ---- Phase 3: TUI ------------------------------------------ */
    status = sb_tui_run_menu(&ctx);
    if (EFI_ERROR(status))
        return status;

    /* ---- Phase 4: Boot ----------------------------------------- */
    status = sb_boot_selected(&ctx);

    /* If we reach here, booting failed. */
    SB_LOG(L"Boot failed: %r", status);
    SB_LOG(L"Dropping to EFI explorer.");
    sb_tui_file_browser(&ctx);

    return status;
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static EFI_STATUS
sb_init_context(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                SuperBootContext *ctx)
{
    SetMem(ctx, sizeof(*ctx), 0);

    ctx->image_handle    = image;
    ctx->system_table    = st;
    ctx->boot_services   = st->BootServices;
    ctx->runtime_services = st->RuntimeServices;
    ctx->timeout_sec     = 5;
    ctx->verbose         = FALSE;
    ctx->targets.count   = 0;
    ctx->selected        = 0;

    /* Parse our own command-line for flags (e.g. "verbose"). */
    {
        EFI_LOADED_IMAGE_PROTOCOL *loaded;
        EFI_STATUS s = ctx->boot_services->HandleProtocol(
                            image, &gEfiLoadedImageProtocolGuid,
                            (void **)&loaded);
        if (!EFI_ERROR(s) && loaded->LoadOptions) {
            CHAR16 *opts = (CHAR16 *)loaded->LoadOptions;
            if (StriStr(opts, L"verbose"))
                ctx->verbose = TRUE;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS
sb_boot_selected(SuperBootContext *ctx)
{
    const BootTarget *t = &ctx->targets.entries[ctx->selected];

    SB_LOG(L"Booting: %s", t->title);

    if (t->is_chainload)
        return sb_chainload_efi(ctx, t);

    return sb_boot_linux(ctx, t);
}
